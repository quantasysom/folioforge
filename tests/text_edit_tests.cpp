#include "pdfengine/document.h"
#include "pdfengine/renderer.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <cmath>
#include <iostream>
#include <random>

using namespace pdfengine;
namespace {
using Obj = QPDFObjectHandle;
int checks{};
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class Fn> void rejected(Fn fn, ErrorCode code) {
    try { fn(); } catch (const Error& error) { require(error.code == code, "Wrong error category"); return; }
    throw std::runtime_error("Expected rejection");
}
Bytes fixture(const std::string& commands, const std::string& font = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>", bool split = false) {
    QPDF pdf; pdf.emptyPDF();
    auto page = Obj::parse("<< /Type /Page /MediaBox [0 0 600 800] >>");
    auto fonts = Obj::newDictionary(); fonts.replaceKey("/F1", pdf.makeIndirectObject(Obj::parse(font)));
    auto resources = Obj::newDictionary(); resources.replaceKey("/Font", fonts); page.replaceKey("/Resources", resources);
    if (split) page.replaceKey("/Contents", Obj::newArray(std::vector<Obj>{pdf.newStream(commands), pdf.newStream("\n")}));
    else page.replaceKey("/Contents", pdf.newStream(commands));
    QPDFPageDocumentHelper(pdf).addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
    QPDFWriter writer(pdf); writer.setOutputMemory(); writer.write(); auto bytes = writer.getBufferSharedPointer();
    return Bytes(bytes->getBuffer(), bytes->getBuffer() + bytes->getSize());
}
std::string stream(const Snapshot& snapshot, std::size_t page = 0) {
    QPDF pdf; pdf.processMemoryFile("fixture", reinterpret_cast<const char*>(snapshot.bytes->data()), snapshot.bytes->size());
    auto buffer = QPDFPageDocumentHelper(pdf).getAllPages()[page].getObjectHandle().getKey("/Contents").getStreamData();
    return std::string(reinterpret_cast<const char*>(buffer->getBuffer()), buffer->getSize());
}
void outsideEqual(const Bitmap& before, const Bitmap& after, const TextRun& run) {
    require(before.width == after.width && before.height == after.height, "Text edit changed page geometry");
    bool different = false;
    for (int y = 0; y < before.height; ++y) for (int x = 0; x < before.width; ++x) {
        bool inside = x >= std::floor(run.x - 4) && x <= std::ceil(run.x + run.width + 4) &&
            y >= std::floor(before.height - run.baseline - run.fontSize - 4) && y <= std::ceil(before.height - run.baseline + run.fontSize*0.3 + 4);
        for (int channel = 0; channel < 4; ++channel) {
            auto offset = static_cast<std::size_t>(y) * before.stride + x*4 + channel;
            if (before.bgra[offset] != after.bgra[offset]) {
                if (!inside) throw std::runtime_error("Pixels outside the edited run changed");
                different = true;
            }
        }
    }
    require(different, "Text edit did not change rendered content");
}
}
int main() {
    auto root = std::filesystem::temp_directory_path() / ("folio-text-tests-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(p, ec); } } cleanup{root};
    try {
        int fileIndex = 0;
        auto open = [&](const std::string& commands, const std::string& font = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>", bool split = false) {
            auto path = root / (std::to_string(++fileIndex) + ".pdf"); atomicWrite(path, fixture(commands, font, split), false); return Document::open(path);
        };
        const std::string commands = "% (ORIGINAL) in a comment must survive\n0 0 1 rg 40 600 200 40 re f\nBT /F1 18 Tf 1 0 0 1 40 500 Tm (ORIGINAL) Tj ( NEXT) Tj 0 -50 Td (Lower line) Tj ET\n";
        auto doc = open(commands); auto page = doc->info().pages[0].id;
        auto inventory = doc->textRuns(page); require(inventory.runs.size() == 3, "Expected three mapped runs");
        auto run = inventory.runs.front(); require(run.text == "ORIGINAL" && run.x == 40 && run.baseline == 500, "Incorrect run source or coordinates");
        auto before = Renderer::render(doc->snapshot(), 0, 1); auto original = doc->snapshot();
        doc->replaceText({page, run.id, run.revision, "NEW"});
        require(doc->info().dirty, "Text replacement must dirty the document");
        auto extracted = Renderer::text(doc->snapshot(), 0);
        require(extracted.find(u"NEW") != std::u16string::npos && extracted.find(u"ORIGINAL") == std::u16string::npos, "Replacement did not remove original extractable text");
        require(extracted.find(u"NEXT") != std::u16string::npos, "Following run lost");
        require(stream(doc->snapshot()).starts_with("% (ORIGINAL) in a comment must survive"), "Edited an unrelated comment");
        outsideEqual(before, Renderer::render(doc->snapshot(), 0, 1), run);
        auto output = root / "edited.pdf"; Renderer::validate(doc->snapshot()); doc->save(output);
        auto reopened = Document::open(output);
        require(Renderer::text(reopened->snapshot(), 0).find(u"NEW") != std::u16string::npos, "Edited text lost on save/reopen");
        doc->undo(doc->info().revision); require(Renderer::render(doc->snapshot(), 0, 1).bgra == before.bgra, "Undo did not restore exact appearance");
        doc->redo(doc->info().revision); require(!doc->info().dirty, "Redo to saved edited state should be clean");
        rejected([&] { doc->replaceText({page, run.id, run.revision, "OLD"}); }, ErrorCode::StaleRevision);
        auto current = doc->textRuns(page).runs.front(); auto checkpoint = doc->snapshot();
        rejected([&] { doc->replaceText({page, current.id, current.revision, "Replacement that does not fit"}); }, ErrorCode::TextOverflow);
        rejected([&] { doc->replaceText({page, current.id, current.revision, "\xE0\xB0\x85"}); }, ErrorCode::Unsupported);
        require(doc->snapshot().bytes == checkpoint.bytes && doc->info().revision == checkpoint.revision, "Rejected text edit changed committed state");
        // Short edits retain the original advance, allowing later growth within that slot.
        doc->replaceText({page, current.id, current.revision, "ORIGINAL"});
        require(Renderer::render(doc->snapshot(), 0, 1).bgra == before.bgra, "Re-edit did not retain original text slot");
        current = doc->textRuns(page).runs.front();
        doc->replaceText({page, current.id, current.revision, ""});
        require(Renderer::text(doc->snapshot(), 0).find(u"ORIGINAL") == std::u16string::npos, "Empty replacement failed to remove text");
        outsideEqual(before, Renderer::render(doc->snapshot(), 0, 1), run);
        auto duplicate = open(commands);
        duplicate->execute({CommandKind::Duplicate, duplicate->info().pages[0].id, duplicate->info().revision});
        auto secondPage = duplicate->info().pages[1].id; auto secondRun = duplicate->textRuns(secondPage).runs.front();
        duplicate->replaceText({secondPage, secondRun.id, secondRun.revision, "COPY"});
        require(Renderer::render(duplicate->snapshot(), 0, 1).bgra == before.bgra, "Shared stream edit changed original page");
        require(Renderer::text(duplicate->snapshot(), 1).find(u"COPY") != std::u16string::npos, "Duplicate page not edited");
        for (const auto& show : {"<4F524947494E414C> Tj", "[(ORI) 20 (GINAL)] TJ", "(ORIG\\(INAL\\)\\\\) Tj"}) {
            auto escaped = open(std::string("BT /F1 18 Tf 1 0 0 1 40 500 Tm ") + show + " ( NEXT) Tj ET");
            auto item = escaped->textRuns(escaped->info().pages[0].id).runs.front(); auto bitmap = Renderer::render(escaped->snapshot(), 0, 1);
            escaped->replaceText({item.page, item.id, item.revision, "(Hi)\\"});
            require(Renderer::text(escaped->snapshot(), 0).find(u"(Hi)\\") != std::u16string::npos, "Escaped text round trip failed");
            outsideEqual(bitmap, Renderer::render(escaped->snapshot(), 0, 1), item);
        }
        auto scaled = open("q 2 0 0 2 10 20 cm BT /F1 12 Tf 1 0 0 1 20 100 Tm (ORIGINAL) Tj ( NEXT) Tj ET Q");
        auto scaledRun = scaled->textRuns(scaled->info().pages[0].id).runs.front();
        require(scaledRun.x == 50 && scaledRun.baseline == 220 && scaledRun.fontSize == 24, "Graphics/text transform mapping failed");
        auto scaledBefore = Renderer::render(scaled->snapshot(), 0, 1);
        scaled->replaceText({scaledRun.page, scaledRun.id, scaledRun.revision, "NEW"});
        outsideEqual(scaledBefore, Renderer::render(scaled->snapshot(), 0, 1), scaledRun);
        for (const auto& font : {"Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique", "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"}) {
            auto fonts = open(commands, std::string("<< /Type /Font /Subtype /Type1 /BaseFont /") + font + " /Encoding /WinAnsiEncoding >>");
            auto item = fonts->textRuns(fonts->info().pages[0].id).runs.front(); auto bitmap = Renderer::render(fonts->snapshot(), 0, 1);
            fonts->replaceText({item.page, item.id, item.revision, "NEW"});
            outsideEqual(bitmap, Renderer::render(fonts->snapshot(), 0, 1), item);
        }
        for (const auto& unsafe : {"BT /F1 18 Tf 1 0 0 1 40 500 Tm 7 Tr (ORIGINAL) Tj ET", "0 0 20 20 re W n BT /F1 18 Tf 40 500 Td (ORIGINAL) Tj ET", "BT /F1 18 Tf 0 1 -1 0 40 500 Tm (ORIGINAL) Tj ET", "/Span BMC BT /F1 18 Tf 40 500 Td (ORIGINAL) Tj ET EMC", "BT /F1 18 Tf 40 500 Td (ORIGINAL) Tj", "BT /F1 18 Tf 20 TL 40 500 Td (ORIGINAL) ' 0 -20 Td (NEXT) Tj ET", "BT /F1 18 Tf 40 500 Td 1 unknown 0 -20 Td (NEXT) Tj ET"}) {
            auto unsupported = open(unsafe); require(unsupported->textRuns(unsupported->info().pages[0].id).runs.empty(), "Unsafe text was offered for editing");
        }
        auto custom = open(commands, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding << /BaseEncoding /WinAnsiEncoding /Differences [65 /B] >> >>");
        require(custom->textRuns(custom->info().pages[0].id).runs.empty(), "Custom encoding accepted");
        auto multiple = open(commands, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>", true);
        require(multiple->textRuns(multiple->info().pages[0].id).runs.empty(), "Multiple streams accepted without source qualification");
        require(Renderer::render(original, 0, 1).bgra == before.bgra, "Prior snapshot lifetime broken");
        std::cout << checks << " text-edit checks passed: source mapping, escaped tokens, slot preservation, shared resources, round trips, raster invariants, undo, and rejection paths\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
