#include "pdfengine/document.h"
#include "text_edit.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <algorithm>
#include <numeric>

namespace pdfengine {
namespace {
constexpr std::size_t historyBudget = 128 * 1024 * 1024;
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
    if (buffer->getSize() > 256u * 1024u * 1024u)
        throw Error(ErrorCode::ResourceLimit, "This preview supports PDF snapshots up to 256 MiB.");
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
    for (const char* key : {"/AcroForm", "/Perms", "/StructTreeRoot", "/Outlines", "/Names", "/Dests", "/PageLabels", "/OCProperties", "/OpenAction", "/AA", "/Collection"}) {
        if (root.hasKey(key)) return "This PDF contains forms, navigation, signatures, layers, or document structures whose editing is not yet qualified. Read-only mode preserves the original.";
    }
    for (auto page : QPDFPageDocumentHelper(pdf).getAllPages()) {
        auto object = page.getObjectHandle();
        for (const char* key : {"/Annots", "/AA", "/StructParents", "/B", "/PresSteps"})
            if (object.hasKey(key)) return "Pages with annotations, actions, or structural references are read-only in this preview.";
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
void Document::commit(State state) {
    // Prepare allocations before publishing anything. A failed candidate never touches the session.
    undo_.push_back(current_);
    current_ = std::move(state);
    redo_.clear();
    ++revision_;
    std::size_t total = current_.bytes->size();
    for (const auto& item : undo_) total += item.bytes->size();
    while (!undo_.empty() && (total > historyBudget || undo_.size() > 100)) {
        total -= undo_.front().bytes->size();
        undo_.erase(undo_.begin());
        historyPruned_ = true;
    }
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
        if (pages.size() >= maxPages) throw Error(ErrorCode::ResourceLimit, "The 10,000-page preview limit has been reached.");
        auto added = command.kind == CommandKind::InsertBlank ? QPDFPageObjectHelper(blank(store.pdf)) : selected.shallowCopyPage();
        helper.addPageAt(added, false, selected);
        ids.insert(ids.begin() + index + 1, nextPage_); break;
    }
    case CommandKind::Delete:
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
    redo_.push_back(current_); current_ = std::move(undo_.back()); undo_.pop_back(); ++revision_;
}
void Document::redo(RevisionId expected) {
    checkRevision(expected);
    if (redo_.empty()) return;
    undo_.push_back(current_); current_ = std::move(redo_.back()); redo_.pop_back(); ++revision_;
}
void Document::insertDocument(const std::filesystem::path& path, PageId after, RevisionId expected) {
    checkRevision(expected);
    auto incoming = Document::open(path);
    if (!incoming->editable_) throw Error(ErrorCode::Unsupported, incoming->restriction_);
    auto ids = idsOf(current_.pages);
    auto found = std::find(ids.begin(), ids.end(), after);
    if (found == ids.end()) throw Error(ErrorCode::InvalidSelection, "Select a destination page.");
    if (ids.size() + incoming->current_.pages.size() > maxPages) throw Error(ErrorCode::ResourceLimit, "The merged PDF exceeds 10,000 pages.");
    auto index = static_cast<std::size_t>(found - ids.begin());
    Store store(current_.bytes), source(incoming->current_.bytes);
    QPDFPageDocumentHelper helper(store.pdf);
    auto anchor = helper.getAllPages()[index];
    PageId next = nextPage_;
    for (auto page : QPDFPageDocumentHelper(source.pdf).getAllPages()) {
        helper.addPageAt(page, false, anchor);
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
    auto updated = textedit::replacement(*run, request.text);
    if (request.text == run->run.text) return;
    if (run->offset > inventory.content.size() || run->length > inventory.content.size() - run->offset)
        throw Error(ErrorCode::InvalidDocument, "The text source span could not be validated.");
    inventory.content.replace(run->offset, run->length, updated);
    // Always install a fresh stream. A duplicated page can share the original stream.
    page.getObjectHandle().replaceKey("/Contents", store.pdf.newStream(inventory.content));
    auto bytes = serialize(store.pdf);
    Store checked(bytes);
    auto metadata = inspect(checked.pdf, ids);
    auto checkedText = textedit::inspect(QPDFPageDocumentHelper(checked.pdf).getAllPages()[selected - ids.begin()], request.page, revision_ + 1);
    auto rewritten = std::find_if(checkedText.runs.begin(), checkedText.runs.end(), [&](const auto& item) { return item.run.id == request.run && item.run.text == request.text; });
    if ((!request.text.empty() && rewritten == checkedText.runs.end()) || !store.pdf.getWarnings().empty() || !checked.pdf.getWarnings().empty())
        throw Error(ErrorCode::InvalidDocument, "Text edit validation failed. The original document was retained.");
    commit({std::move(bytes), std::move(metadata), nextIdentity_}); ++nextIdentity_;
}
}
