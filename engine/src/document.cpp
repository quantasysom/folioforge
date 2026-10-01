#include "pdfengine/document.h"
#include "sfnt.h"
#include "text_edit.h"
#include "annotation.h"
#include "form.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <algorithm>
#include <iomanip>
#include <numeric>
#include <cctype>
#include <cstring>
#include <fstream>
#include <random>
#include <locale>
#include <sstream>

namespace pdfengine {
namespace {
constexpr std::size_t maxPages = 10000;
using Obj = QPDFObjectHandle;
struct Store {
    std::shared_ptr<const Bytes> backing; // Outlives the lazy parser.
    QPDF pdf;
    explicit Store(std::shared_ptr<const Bytes> bytes, const std::string& password = {}) : backing(std::move(bytes)) {
        pdf.setSuppressWarnings(true);
        try {
            pdf.processMemoryFile("document", reinterpret_cast<const char*>(backing->data()), backing->size(), password.c_str());
        } catch (const QPDFExc& e) {
            if (e.getErrorCode() == qpdf_e_password)
                throw Error(ErrorCode::PasswordRequired, "A password is required, or the supplied password is incorrect.");
            throw Error(ErrorCode::InvalidDocument, "The PDF could not be parsed.");
        }
    }
};
std::shared_ptr<const Bytes> serialize(QPDF& pdf) {
    QPDFWriter writer(pdf);
    writer.setOutputMemory();
    writer.setPreserveEncryption(false);
    writer.write();
    auto buffer = writer.getBufferSharedPointer();
    if (buffer->getSize() > maxDocumentBytes)
        throw Error(ErrorCode::ResourceLimit, "This preview supports PDF snapshots up to 1 GiB.");
    return std::make_shared<const Bytes>(buffer->getBuffer(), buffer->getBuffer() + buffer->getSize());
}
std::vector<PageInfo> inspect(QPDF& pdf, const std::vector<PageId>& ids) {
    auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
    if (pages.empty() || pages.size() > maxPages || pages.size() != ids.size())
        throw Error(ErrorCode::InvalidDocument, "The document must contain between 1 and 10,000 valid pages.");
    std::vector<PageInfo> out;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        auto box = pages[i].getMediaBox().getArrayAsRectangle();
        auto rotate = pages[i].getAttribute("/Rotate", false);
        int angle = rotate.isInteger() ? rotate.getIntValueAsInt() : 0;
        angle = ((angle % 360) + 360) % 360;
        if (angle % 90 != 0 || box.urx <= box.llx || box.ury <= box.lly)
            throw Error(ErrorCode::InvalidDocument, "The PDF contains unsupported page geometry.");
        out.push_back({ids[i], box.urx - box.llx, box.ury - box.lly, angle});
    }
    return out;
}
std::vector<PageId> idsOf(const std::vector<PageInfo>& pages) {
    std::vector<PageId> ids;
    for (const auto& page : pages) ids.push_back(page.id);
    return ids;
}
std::string restriction(QPDF& pdf) {
    if (pdf.isEncrypted()) return "Encrypted PDFs are read-only in this preview.";
    auto root = pdf.getRoot();
    for (const char* key : {"/Perms", "/StructTreeRoot", "/Outlines", "/Names", "/Dests", "/PageLabels", "/OCProperties", "/OpenAction", "/AA", "/Collection"}) {
        if (root.hasKey(key)) return "This PDF contains forms, navigation, signatures, layers, or document structures whose editing is not yet qualified. Read-only mode preserves the original.";
    }
    if (auto reason = form::restriction(root); !reason.empty()) return reason;
    for (auto page : QPDFPageDocumentHelper(pdf).getAllPages()) {
        auto object = page.getObjectHandle();
        for (const char* key : {"/AA", "/StructParents", "/B", "/PresSteps"})
            if (object.hasKey(key)) return "Pages with actions or structural references are read-only in this preview.";
        auto annots = object.getKey("/Annots");
        if (object.hasKey("/Annots")) {
            bool safe = annots.isArray();
            for (int i = 0; safe && i < annots.getArrayNItems(); ++i) safe = annot::preservable(annots.getArrayItem(i));
            if (!safe) return "Pages with links, form fields, or other interactive annotations are read-only in this preview.";
        }
    }
    return {};
}
Obj blank(QPDF& pdf) {
    auto page = Obj::newDictionary();
    page.replaceKey("/Type", Obj::newName("/Page"));
    page.replaceKey("/MediaBox", Obj::newArray(Obj::Rectangle(0, 0, 595.276, 841.89)));
    page.replaceKey("/Resources", Obj::newDictionary());
    page.replaceKey("/Contents", pdf.newStream(""));
    return pdf.makeIndirectObject(page);
}
constexpr double defaultImageDpi = 96, maxImagePageSide = 842;
Obj imageStream(QPDF& pdf, const ImagePage& image, bool alpha) {
    const auto& data = alpha ? image.alpha : image.data;
    auto stream = pdf.newStream(std::string(data.begin(), data.end()));
    auto dict = stream.getDict();
    dict.replaceKey("/Type", Obj::newName("/XObject"));
    dict.replaceKey("/Subtype", Obj::newName("/Image"));
    dict.replaceKey("/Width", Obj::newInteger(image.width));
    dict.replaceKey("/Height", Obj::newInteger(image.height));
    dict.replaceKey("/ColorSpace", Obj::newName(alpha || image.components == 1 ? "/DeviceGray" : "/DeviceRGB"));
    dict.replaceKey("/BitsPerComponent", Obj::newInteger(8));
    if (image.jpeg && !alpha) dict.replaceKey("/Filter", Obj::newName("/DCTDecode"));
    return stream;
}
Obj imagePage(QPDF& pdf, const ImagePage& image) {
    const std::uint64_t pixels = static_cast<std::uint64_t>(image.width) * image.height;
    if (image.width == 0 || image.height == 0 || image.width > 30000 || image.height > 30000 || pixels > 100'000'000)
        throw Error(ErrorCode::ResourceLimit, "Images must be at most 30,000 pixels per side and 100 megapixels.");
    if (image.components != 1 && image.components != 3) throw Error(ErrorCode::Unsupported, "Only gray and RGB images are supported.");
    if (!image.jpeg && image.data.size() != pixels * static_cast<std::uint64_t>(image.components))
        throw Error(ErrorCode::InvalidDocument, "The image pixel data has the wrong size.");
    if (image.data.empty() || (!image.alpha.empty() && image.alpha.size() != pixels))
        throw Error(ErrorCode::InvalidDocument, "The image data is incomplete.");
    double dpi = image.dpi >= 1 && image.dpi <= 10000 ? image.dpi : defaultImageDpi;
    double width = image.width * 72.0 / dpi, height = image.height * 72.0 / dpi;
    if (double longest = std::max(width, height); longest > maxImagePageSide) { width *= maxImagePageSide / longest; height *= maxImagePageSide / longest; }
    width = std::max(width, 1.0); height = std::max(height, 1.0);
    auto picture = imageStream(pdf, image, false);
    if (!image.alpha.empty()) picture.getDict().replaceKey("/SMask", pdf.makeIndirectObject(imageStream(pdf, image, true)));
    std::ostringstream content; content.imbue(std::locale::classic()); content << std::fixed << std::setprecision(4);
    content << "q " << width << " 0 0 " << height << " 0 0 cm /Im0 Do Q\n";
    auto xobjects = Obj::newDictionary(); xobjects.replaceKey("/Im0", pdf.makeIndirectObject(picture));
    auto resources = Obj::newDictionary(); resources.replaceKey("/XObject", xobjects);
    auto page = Obj::newDictionary();
    page.replaceKey("/Type", Obj::newName("/Page"));
    page.replaceKey("/MediaBox", Obj::newArray(Obj::Rectangle(0, 0, width, height)));
    page.replaceKey("/Resources", resources);
    page.replaceKey("/Contents", pdf.newStream(content.str()));
    return pdf.makeIndirectObject(page);
}
}
ImagePage jpegImage(const Bytes& file, double dpi) {
    auto bad = [] { return Error(ErrorCode::Unsupported, "This JPEG is not supported. Use an 8-bit gray or RGB JPEG."); };
    if (file.size() < 4 || file[0] != 0xff || file[1] != 0xd8) throw Error(ErrorCode::InvalidDocument, "The file is not a valid JPEG image.");
    std::size_t i = 2;
    while (i + 4 <= file.size()) {
        if (file[i] != 0xff) { ++i; continue; }
        unsigned marker = file[i + 1];
        if (marker == 0xff) { ++i; continue; }
        if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) { i += 2; continue; }
        if (marker == 0xd9 || marker == 0xda) break;
        std::size_t length = (static_cast<std::size_t>(file[i + 2]) << 8) | file[i + 3];
        if (length < 2 || i + 2 + length > file.size()) break;
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (length < 8) break;
            int precision = file[i + 4], components = file[i + 9];
            std::uint32_t height = (file[i + 5] << 8) | file[i + 6], width = (file[i + 7] << 8) | file[i + 8];
            if (precision != 8 || (components != 1 && components != 3) || !width || !height) throw bad();
            ImagePage image; image.width = width; image.height = height; image.components = components;
            image.jpeg = true; image.data = file; image.dpi = dpi;
            return image;
        }
        if ((marker >= 0xc3 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc)) throw bad();
        i += 2 + length;
    }
    throw Error(ErrorCode::InvalidDocument, "The JPEG header could not be read.");
}
std::shared_ptr<Document> Document::createFromImage(const ImagePage& image) {
    auto doc = std::shared_ptr<Document>(new Document);
    QPDF pdf;
    pdf.emptyPDF();
    QPDFPageDocumentHelper(pdf).addPage(QPDFPageObjectHelper(imagePage(pdf, image)), false);
    auto bytes = serialize(pdf);
    Store checked(bytes);
    auto metadata = inspect(checked.pdf, {doc->nextPage_++});
    if (!pdf.getWarnings().empty() || !checked.pdf.getWarnings().empty())
        throw Error(ErrorCode::InvalidDocument, "The image could not be converted to a valid PDF page.");
    doc->current_ = {std::move(bytes), std::move(metadata), doc->nextIdentity_++};
    return doc;
}
std::shared_ptr<Document> Document::create() {
    auto doc = std::shared_ptr<Document>(new Document);
    QPDF pdf;
    pdf.emptyPDF();
    QPDFPageDocumentHelper(pdf).addPage(QPDFPageObjectHelper(blank(pdf)), false);
    doc->current_ = {serialize(pdf), inspect(pdf, {doc->nextPage_++}), doc->nextIdentity_++};
    return doc;
}
std::shared_ptr<Document> Document::open(const std::filesystem::path& path, const std::string& password) {
    auto doc = std::shared_ptr<Document>(new Document);
    doc->sourceBytes_ = std::make_shared<const Bytes>(readFile(path));
    Store store(doc->sourceBytes_, password);
    auto pages = QPDFPageDocumentHelper(store.pdf).getAllPages();
    std::vector<PageId> ids;
    for (std::size_t i = 0; i < pages.size(); ++i) ids.push_back(doc->nextPage_++);
    doc->restriction_ = restriction(store.pdf);
    auto info = inspect(store.pdf, ids);
    auto bytes = serialize(store.pdf);
    if (!store.pdf.getWarnings().empty()) doc->restriction_ = "The PDF required repairs. Editing and saving are disabled to preserve the original.";
    doc->editable_ = doc->restriction_.empty();
    doc->current_ = {std::move(bytes), std::move(info), doc->nextIdentity_++};
    doc->savedIdentity_ = doc->current_.identity;
    doc->path_ = std::filesystem::absolute(path).lexically_normal();
    return doc;
}
DocumentInfo Document::info() const {
    return {current_.pages, revision_, current_.identity != savedIdentity_, !undo_.empty(), !redo_.empty(),
            editable_, restriction_, path_, historyPruned_};
}
Snapshot Document::snapshot() const { return {current_.bytes, revision_, idsOf(current_.pages)}; }
void Document::checkRevision(RevisionId expected) const {
    if (expected != revision_) throw Error(ErrorCode::StaleRevision, "The document changed. Select the page again and retry.");
    if (!editable_) throw Error(ErrorCode::Unsupported, restriction_);
}
struct Document::SpillFile {
    std::filesystem::path path;
    ~SpillFile() { std::error_code ec; std::filesystem::remove(path, ec); }
};
Document::~Document() {
    undo_.clear(); redo_.clear(); current_ = {};
    if (!spillDirectory_.empty()) { std::error_code ec; std::filesystem::remove_all(spillDirectory_, ec); }
}
void Document::setHistoryLimits(const HistoryLimits& limits) { limits_ = limits; enforceHistoryBudget(); }
bool Document::spill(State& state) {
    if (!state.spill) {
        try {
            if (spillDirectory_.empty()) {
                auto directory = std::filesystem::temp_directory_path() / ("folioforge-history-" + std::to_string(std::random_device{}()) + "-" + std::to_string(std::random_device{}()));
                std::filesystem::create_directories(directory);
                spillDirectory_ = directory;
            }
            auto file = std::make_shared<SpillFile>();
            file->path = spillDirectory_ / (std::to_string(++spillCounter_) + ".pdf");
            {
                std::ofstream out(file->path, std::ios::binary);
                out.write(reinterpret_cast<const char*>(state.bytes->data()), static_cast<std::streamsize>(state.bytes->size()));
                out.flush();
                if (!out) return false;
            }
            state.spill = std::move(file); state.spillSize = state.bytes->size();
        } catch (const std::exception&) { return false; }
    }
    state.bytes.reset();
    return true;
}
void Document::materialize(State& state) const {
    if (state.bytes) return;
    try { state.bytes = std::make_shared<const Bytes>(readFile(state.spill->path)); }
    catch (const std::exception&) { throw Error(ErrorCode::InvalidDocument, "An earlier document state could not be restored from disk."); }
}
void Document::enforceHistoryBudget() {
    auto prune = [this](std::vector<State>& list, std::size_t index) { list.erase(list.begin() + static_cast<std::ptrdiff_t>(index)); historyPruned_ = true; };
    while (undo_.size() > limits_.entries) prune(undo_, 0);
    auto memory = [this] {
        std::uint64_t total = 0;
        for (const auto* list : {&undo_, &redo_}) for (const auto& item : *list) if (item.bytes) total += item.bytes->size();
        return total;
    };
    while (memory() > limits_.memoryBytes) {
        // Oldest undo states are farthest from the user; redo states are farthest at the front.
        State* candidate = nullptr; std::vector<State>* owner = nullptr; std::size_t index = 0;
        for (std::size_t i = 0; i < undo_.size() && !candidate; ++i) if (undo_[i].bytes) { candidate = &undo_[i]; owner = &undo_; index = i; }
        for (std::size_t i = 0; i < redo_.size() && !candidate; ++i) if (redo_[i].bytes) { candidate = &redo_[i]; owner = &redo_; index = i; }
        if (!candidate) break;
        if (!spill(*candidate)) prune(*owner, index); // Disk unavailable: keep only what fits in memory.
    }
    auto disk = [this] {
        std::uint64_t total = 0;
        for (const auto* list : {&undo_, &redo_}) for (const auto& item : *list) if (item.spill) total += item.spillSize;
        return total;
    };
    while (disk() > limits_.diskBytes) {
        if (!undo_.empty()) prune(undo_, 0);
        else if (!redo_.empty()) prune(redo_, 0);
        else break;
    }
}
void Document::commit(State state) {
    // Prepare allocations before publishing anything. A failed candidate never touches the session.
    undo_.push_back(current_);
    current_ = std::move(state);
    redo_.clear();
    ++revision_;
    enforceHistoryBudget();
}
void Document::execute(const Command& command) {
    checkRevision(command.expectedRevision);
    auto ids = idsOf(current_.pages);
    auto found = std::find(ids.begin(), ids.end(), command.page);
    if (found == ids.end()) throw Error(ErrorCode::InvalidSelection, "The selected page no longer exists.");
    auto index = static_cast<std::size_t>(found - ids.begin());
    Store store(current_.bytes);
    QPDFPageDocumentHelper helper(store.pdf);
    auto pages = helper.getAllPages();
    auto selected = pages[index];
    switch (command.kind) {
    case CommandKind::RotateLeft: selected.rotatePage(-90, true); break;
    case CommandKind::RotateRight: selected.rotatePage(90, true); break;
    case CommandKind::InsertBlank:
    case CommandKind::Duplicate: {
        if (command.kind == CommandKind::Duplicate && form::hasWidgets(selected))
            throw Error(ErrorCode::Unsupported, "Pages with form fields cannot be duplicated, because the copies would share one field.");
        if (pages.size() >= maxPages) throw Error(ErrorCode::ResourceLimit, "The 10,000-page preview limit has been reached.");
        auto added = command.kind == CommandKind::InsertBlank ? QPDFPageObjectHelper(blank(store.pdf)) : selected.shallowCopyPage();
        helper.addPageAt(added, false, selected);
        ids.insert(ids.begin() + index + 1, nextPage_); break;
    }
    case CommandKind::Delete:
        if (form::hasWidgets(selected)) throw Error(ErrorCode::Unsupported, "Pages with form fields cannot be deleted yet.");
        if (pages.size() == 1) throw Error(ErrorCode::InvalidSelection, "Keep at least one page in the document.");
        helper.removePage(selected); ids.erase(ids.begin() + index); break;
    case CommandKind::MoveEarlier:
    case CommandKind::MoveLater: {
        bool earlier = command.kind == CommandKind::MoveEarlier;
        if ((earlier && index == 0) || (!earlier && index + 1 == pages.size())) return;
        auto neighbor = earlier ? index - 1 : index + 1;
        // Preserve inherited resources/boxes before detaching the page.
        helper.pushInheritedAttributesToPage();
        helper.removePage(selected);
        helper.addPageAt(selected, earlier, pages[neighbor]);
        std::swap(ids[index], ids[neighbor]); break;
    }
    }
    auto bytes = serialize(store.pdf);
    Store checked(bytes);
    auto metadata = inspect(checked.pdf, ids);
    if (!store.pdf.getWarnings().empty() || !checked.pdf.getWarnings().empty())
        throw Error(ErrorCode::InvalidDocument, "The operation produced PDF warnings and was rolled back.");
    commit({std::move(bytes), std::move(metadata), nextIdentity_});
    ++nextIdentity_;
    if (command.kind == CommandKind::InsertBlank || command.kind == CommandKind::Duplicate) ++nextPage_;
}
void Document::undo(RevisionId expected) {
    checkRevision(expected);
    if (undo_.empty()) return;
    auto target = undo_.back(); materialize(target);
    redo_.push_back(current_); current_ = std::move(target); undo_.pop_back(); ++revision_;
    enforceHistoryBudget();
}
void Document::redo(RevisionId expected) {
    checkRevision(expected);
    if (redo_.empty()) return;
    auto target = redo_.back(); materialize(target);
    undo_.push_back(current_); current_ = std::move(target); redo_.pop_back(); ++revision_;
    enforceHistoryBudget();
}
std::vector<std::size_t> parsePageRanges(const std::string& text, std::size_t count) {
    std::vector<std::size_t> out;
    auto bad = [&](const std::string& piece) { return Error(ErrorCode::InvalidSelection, "'" + piece + "' is not a valid page range for a " + std::to_string(count) + "-page PDF."); };
    auto number = [&](const std::string& s, std::size_t& value) {
        if (s.empty() || s.size() > 7 || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) return false;
        value = std::stoul(s); return true;
    };
    std::string trimmed;
    for (char c : text) if (!std::isspace(static_cast<unsigned char>(c))) trimmed += c;
    if (trimmed.empty()) { for (std::size_t i = 0; i < count; ++i) out.push_back(i); return out; }
    std::stringstream stream(trimmed); std::string piece;
    while (std::getline(stream, piece, ',')) {
        std::size_t first = 1, last = count;
        auto dash = piece.find('-');
        if (dash == std::string::npos) { if (!number(piece, first)) throw bad(piece); last = first; }
        else {
            auto a = piece.substr(0, dash), b = piece.substr(dash + 1);
            if (!a.empty() && !number(a, first)) throw bad(piece);
            if (!b.empty() && !number(b, last)) throw bad(piece);
            if (a.empty() && b.empty()) throw bad(piece);
        }
        if (first < 1 || last < first || last > count) throw bad(piece);
        for (auto i = first; i <= last; ++i) { out.push_back(i - 1); if (out.size() > maxPages) throw Error(ErrorCode::ResourceLimit, "The selection exceeds 10,000 pages."); }
    }
    if (out.empty()) throw bad(text);
    return out;
}
void Document::insertDocument(const std::filesystem::path& path, PageId after, RevisionId expected, const std::string& ranges) {
    checkRevision(expected);
    auto incoming = Document::open(path);
    if (!incoming->editable_) throw Error(ErrorCode::Unsupported, incoming->restriction_);
    {
        Store probe(incoming->current_.bytes);
        if (probe.pdf.getRoot().hasKey("/AcroForm")) throw Error(ErrorCode::Unsupported, "Importing pages from a PDF with form fields is not supported.");
    }
    auto ids = idsOf(current_.pages);
    auto found = std::find(ids.begin(), ids.end(), after);
    if (found == ids.end()) throw Error(ErrorCode::InvalidSelection, "Select a destination page.");
    auto selection = parsePageRanges(ranges, incoming->current_.pages.size());
    if (ids.size() + selection.size() > maxPages) throw Error(ErrorCode::ResourceLimit, "The merged PDF exceeds 10,000 pages.");
    auto index = static_cast<std::size_t>(found - ids.begin());
    Store store(current_.bytes), source(incoming->current_.bytes);
    QPDFPageDocumentHelper helper(store.pdf);
    auto anchor = helper.getAllPages()[index];
    PageId next = nextPage_;
    auto sourcePages = QPDFPageDocumentHelper(source.pdf).getAllPages();
    for (auto pick : selection) {
        helper.addPageAt(sourcePages.at(pick), false, anchor);
        anchor = helper.getAllPages()[++index];
        ids.insert(ids.begin() + index, next++);
    }
    auto bytes = serialize(store.pdf); // source remains alive through serialization
    Store checked(bytes);
    auto metadata = inspect(checked.pdf, ids);
    if (!store.pdf.getWarnings().empty() || !source.pdf.getWarnings().empty() || !checked.pdf.getWarnings().empty())
        throw Error(ErrorCode::InvalidDocument, "Import produced PDF warnings and was rolled back.");
    commit({std::move(bytes), std::move(metadata), nextIdentity_});
    ++nextIdentity_; nextPage_ = next;
}
void Document::insertImage(const ImagePage& image, PageId after, RevisionId expected) {
    checkRevision(expected);
    auto ids = idsOf(current_.pages);
    auto found = std::find(ids.begin(), ids.end(), after);
    if (found == ids.end()) throw Error(ErrorCode::InvalidSelection, "Select a destination page.");
    if (ids.size() >= maxPages) throw Error(ErrorCode::ResourceLimit, "The 10,000-page preview limit has been reached.");
    auto index = static_cast<std::size_t>(found - ids.begin());
    Store store(current_.bytes);
    QPDFPageDocumentHelper helper(store.pdf);
    helper.addPageAt(QPDFPageObjectHelper(imagePage(store.pdf, image)), false, helper.getAllPages()[index]);
    ids.insert(ids.begin() + index + 1, nextPage_);
    auto bytes = serialize(store.pdf);
    Store checked(bytes);
    auto metadata = inspect(checked.pdf, ids);
    if (!store.pdf.getWarnings().empty() || !checked.pdf.getWarnings().empty())
        throw Error(ErrorCode::InvalidDocument, "The image import produced PDF warnings and was rolled back.");
    commit({std::move(bytes), std::move(metadata), nextIdentity_});
    ++nextIdentity_; ++nextPage_;
}
void Document::save(const std::filesystem::path& path, bool overwrite) {
    if (!editable_) throw Error(ErrorCode::Unsupported, restriction_);
    auto target = std::filesystem::absolute(path).lexically_normal();
    bool same = !path_.empty() && target == path_;
    if (!path_.empty() && std::filesystem::exists(target) && std::filesystem::exists(path_))
        same = same || std::filesystem::equivalent(target, path_);
    Store checked(current_.bytes);
    inspect(checked.pdf, idsOf(current_.pages));
    if (!checked.pdf.getWarnings().empty()) throw Error(ErrorCode::SaveFailed, "PDF validation failed. The destination was not changed.");
    atomicWrite(target, *current_.bytes, overwrite || same, same ? sourceBytes_.get() : nullptr);
    path_ = std::move(target); sourceBytes_ = current_.bytes; savedIdentity_ = current_.identity;
}
TextInventory Document::textRuns(PageId id) const {
    if (!editable_) return {{}, restriction_};
    auto ids = idsOf(current_.pages);
    auto selected = std::find(ids.begin(), ids.end(), id);
    if (selected == ids.end()) throw Error(ErrorCode::InvalidSelection, "The selected page no longer exists.");
    Store store(current_.bytes);
    auto source = textedit::inspect(QPDFPageDocumentHelper(store.pdf).getAllPages()[selected - ids.begin()], id, revision_);
    TextInventory result{{}, source.explanation};
    if (store.pdf.getWarnings().empty()) for (auto& item : source.runs) result.runs.push_back(std::move(item.run));
    return result;
}
std::vector<Annotation> Document::annotations(PageId id) const {
    auto ids = idsOf(current_.pages);
    auto selected = std::find(ids.begin(), ids.end(), id);
    if (selected == ids.end()) throw Error(ErrorCode::InvalidSelection, "The selected page no longer exists.");
    Store store(current_.bytes);
    return annot::list(QPDFPageDocumentHelper(store.pdf).getAllPages()[selected - ids.begin()]);
}
namespace {
// Installs a private copy of /Annots so pages sharing an array (duplicates) are not affected.
QPDFObjectHandle privateAnnots(QPDFObjectHandle page) {
    std::vector<QPDFObjectHandle> items;
    auto old = page.getKey("/Annots");
    if (old.isArray()) for (int i = 0; i < old.getArrayNItems(); ++i) items.push_back(old.getArrayItem(i));
    auto copy = QPDFObjectHandle::newArray(items);
    page.replaceKey("/Annots", copy);
    return copy;
}
}
void Document::addAnnotation(const AddAnnotation& request) {
    checkRevision(request.expectedRevision);
    auto ids = idsOf(current_.pages);
    auto selected = std::find(ids.begin(), ids.end(), request.page);
    if (selected == ids.end()) throw Error(ErrorCode::InvalidSelection, "The selected page no longer exists.");
    Store store(current_.bytes);
    auto page = QPDFPageDocumentHelper(store.pdf).getAllPages()[selected - ids.begin()];
    const auto before = annot::list(page).size();
    std::random_device device;
    std::ostringstream name; name << annot::namePrefix << std::hex << device() << device();
    auto annotation = annot::create(store.pdf, request, name.str());
    auto annots = privateAnnots(page.getObjectHandle());
    annots.appendItem(annotation);
    auto bytes = serialize(store.pdf);
    Store checked(bytes);
    auto metadata = inspect(checked.pdf, ids);
    auto after = annot::list(QPDFPageDocumentHelper(checked.pdf).getAllPages()[selected - ids.begin()]);
    if (after.size() != before + 1 || !store.pdf.getWarnings().empty() || !checked.pdf.getWarnings().empty())
        throw Error(ErrorCode::InvalidDocument, "Annotation validation failed. The original document was retained.");
    commit({std::move(bytes), std::move(metadata), nextIdentity_}); ++nextIdentity_;
}
void Document::removeAnnotation(PageId id, std::uint32_t index, RevisionId expected) {
    checkRevision(expected);
    auto ids = idsOf(current_.pages);
    auto selected = std::find(ids.begin(), ids.end(), id);
    if (selected == ids.end()) throw Error(ErrorCode::InvalidSelection, "The selected page no longer exists.");
    Store store(current_.bytes);
    auto page = QPDFPageDocumentHelper(store.pdf).getAllPages()[selected - ids.begin()];
    auto listed = annot::list(page);
    auto target = std::find_if(listed.begin(), listed.end(), [&](const Annotation& a) { return a.index == index; });
    if (target == listed.end()) throw Error(ErrorCode::InvalidSelection, "The annotation no longer exists.");
    if (!target->removable) throw Error(ErrorCode::Unsupported, "Only annotations created in FolioForge can be removed.");
    auto annots = privateAnnots(page.getObjectHandle());
    annots.eraseItem(static_cast<int>(index));
    auto bytes = serialize(store.pdf);
    Store checked(bytes);
    auto metadata = inspect(checked.pdf, ids);
    if (annot::list(QPDFPageDocumentHelper(checked.pdf).getAllPages()[selected - ids.begin()]).size() + 1 != listed.size() || !checked.pdf.getWarnings().empty())
        throw Error(ErrorCode::InvalidDocument, "Annotation removal validation failed. The original document was retained.");
    commit({std::move(bytes), std::move(metadata), nextIdentity_}); ++nextIdentity_;
}
std::vector<FormField> Document::formFields(PageId id) const {
    auto ids = idsOf(current_.pages);
    auto selected = std::find(ids.begin(), ids.end(), id);
    if (selected == ids.end()) throw Error(ErrorCode::InvalidSelection, "The selected page no longer exists.");
    Store store(current_.bytes);
    return form::list(QPDFPageDocumentHelper(store.pdf).getAllPages()[selected - ids.begin()]);
}
void Document::setFormValue(const SetFormValue& request) {
    checkRevision(request.expectedRevision);
    auto ids = idsOf(current_.pages);
    auto selected = std::find(ids.begin(), ids.end(), request.page);
    if (selected == ids.end()) throw Error(ErrorCode::InvalidSelection, "The selected page no longer exists.");
    const auto index = selected - ids.begin();
    Store store(current_.bytes);
    form::set(store.pdf, QPDFPageDocumentHelper(store.pdf).getAllPages()[index], request);
    auto bytes = serialize(store.pdf);
    Store checked(bytes);
    auto metadata = inspect(checked.pdf, ids);
    auto fields = form::list(QPDFPageDocumentHelper(checked.pdf).getAllPages()[index]);
    if (fields.empty() || !store.pdf.getWarnings().empty() || !checked.pdf.getWarnings().empty()) {
        throw Error(ErrorCode::InvalidDocument, "Form validation failed. The original document was retained.");
    }
    commit({std::move(bytes), std::move(metadata), nextIdentity_}); ++nextIdentity_;
}
void Document::setFallbackFonts(const std::vector<std::filesystem::path>& paths) {
    auto source = std::make_shared<font::FontSource>();
    try { source->setFonts(paths); } catch (const std::exception& error) { throw Error(ErrorCode::Unsupported, error.what()); }
    fonts_ = std::move(source);
}
void Document::replaceText(const ReplaceText& request) {
    checkRevision(request.expectedRevision);
    auto ids = idsOf(current_.pages);
    auto selected = std::find(ids.begin(), ids.end(), request.page);
    if (selected == ids.end()) throw Error(ErrorCode::InvalidSelection, "The selected page no longer exists.");
    Store store(current_.bytes);
    auto page = QPDFPageDocumentHelper(store.pdf).getAllPages()[selected - ids.begin()];
    auto inventory = textedit::inspect(page, request.page, revision_);
    auto run = std::find_if(inventory.runs.begin(), inventory.runs.end(), [&](const auto& item) { return item.run.id == request.run; });
    if (run == inventory.runs.end()) throw Error(ErrorCode::Unsupported, "This text run is not supported or its source mapping is stale.");
    if (request.text == run->run.text) return;
    if (!fonts_) fonts_ = std::make_shared<font::FontSource>();
    auto updated = textedit::replacement(store.pdf, page, *run, request.text, fonts_.get());
    if (run->offset > inventory.content.size() || run->length > inventory.content.size() - run->offset)
        throw Error(ErrorCode::InvalidDocument, "The text source span could not be validated.");
    inventory.content.replace(run->offset, run->length, updated);
    // Always install a fresh stream. A duplicated page can share the original stream.
    page.getObjectHandle().replaceKey("/Contents", store.pdf.newStream(inventory.content));
    auto bytes = serialize(store.pdf);
    Store checked(bytes);
    auto metadata = inspect(checked.pdf, ids);
    auto checkedText = textedit::inspect(QPDFPageDocumentHelper(checked.pdf).getAllPages()[selected - ids.begin()], request.page, revision_ + 1);
    // A replacement may span several runs when fallback fonts are used; rejoin them before comparing.
    std::string rewritten;
    for (const auto& item : checkedText.runs)
        if (item.run.id >= run->offset && item.run.id < run->offset + updated.size()) rewritten += item.run.text;
    if (rewritten != request.text || !store.pdf.getWarnings().empty() || !checked.pdf.getWarnings().empty())
        throw Error(ErrorCode::InvalidDocument, "Text edit validation failed. The original document was retained.");
    commit({std::move(bytes), std::move(metadata), nextIdentity_}); ++nextIdentity_;
}
}
