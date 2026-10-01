#include "pdfengine/document.h"
#include "pdfengine/renderer.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <filesystem>
#include <iostream>

using namespace pdfengine;
using Obj = QPDFObjectHandle;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Fn> void rejected(Fn fn, ErrorCode code) {
    try { fn(); } catch (const Error& error) { require(error.code == code, "Wrong error category"); return; }
    throw std::runtime_error("Expected rejection");
}
void fixture(const std::filesystem::path& path, int rotate) {
    QPDF pdf; pdf.emptyPDF();
    auto font = pdf.makeIndirectObject(Obj::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
    auto page = pdf.makeIndirectObject(Obj::parse("<< /Type /Page /MediaBox [0 0 400 200] /Resources << >> >>"));
    auto fonts = Obj::newDictionary(); fonts.replaceKey("/F1", font);
    auto resources = Obj::newDictionary(); resources.replaceKey("/Font", fonts); page.replaceKey("/Resources", resources);
    page.replaceKey("/Contents", pdf.newStream("BT /F1 24 Tf 40 100 Td (TOPSECRET) Tj ET 0 0 1 rg 300 20 50 50 re f"));
    page.replaceKey("/Annots", Obj::newArray(std::vector<Obj>{pdf.makeIndirectObject(Obj::parse("<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /Contents (HIDDENNOTE) >>"))}));
    if (rotate) page.replaceKey("/Rotate", Obj::newInteger(rotate));
    QPDFPageDocumentHelper(pdf).addPage(QPDFPageObjectHelper(page), false);
    QPDFWriter writer(pdf); writer.setOutputFilename(path.string().c_str()); writer.write();
}
// Renders the page and blacks out the left half, as the desktop does with the user's boxes.
ImagePage flatten(const Snapshot& snapshot, int pixelsPerPoint) {
    auto bitmap = Renderer::render(snapshot, 0, pixelsPerPoint);
    ImagePage raster; raster.width = bitmap.width; raster.height = bitmap.height; raster.components = 3;
    for (int y = 0; y < bitmap.height; ++y) for (int x = 0; x < bitmap.width; ++x) {
        auto at = static_cast<std::size_t>(y) * bitmap.stride + x * 4; bool black = x < bitmap.width / 2;
        raster.data.push_back(black ? 0 : bitmap.bgra[at + 2]); raster.data.push_back(black ? 0 : bitmap.bgra[at + 1]); raster.data.push_back(black ? 0 : bitmap.bgra[at]);
    }
    return raster;
}
bool contains(const Bytes& bytes, const std::string& needle) { return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end(); }
}
int main() {
    try {
        auto root = std::filesystem::temp_directory_path() / "folioforge-redaction-tests"; std::filesystem::remove_all(root); std::filesystem::create_directories(root);
        for (int rotate : {0, 90}) {
            auto path = root / ("in" + std::to_string(rotate) + ".pdf"); fixture(path, rotate);
            auto doc = Document::open(path);
            auto info = doc->info();
            require(Renderer::text(doc->snapshot(), 0).find(u"TOPSECRET") != std::u16string::npos, "Fixture text missing");
            require(contains(*doc->snapshot().bytes, "HIDDENNOTE"), "Fixture note missing");
            doc->redactPage(info.pages[0].id, info.revision, flatten(doc->snapshot(), 1));
            auto after = doc->info();
            require(after.pages.size() == 1 && after.pages[0].id == info.pages[0].id && after.pages[0].rotation == 0, "Page identity must be kept");
            const bool turned = rotate == 90;
            require(std::abs(after.pages[0].width - (turned ? 200 : 400)) < 0.01 && std::abs(after.pages[0].height - (turned ? 400 : 200)) < 0.01, "Page size must be kept");
            require(Renderer::text(doc->snapshot(), 0).empty(), "Redacted page must have no text");
            require(!contains(*doc->snapshot().bytes, "TOPSECRET") && !contains(*doc->snapshot().bytes, "HIDDENNOTE"), "Secrets must be gone from the file bytes");
            auto bitmap = Renderer::render(doc->snapshot(), 0, 1);
            auto at = [&](int x, int y) { return bitmap.bgra[static_cast<std::size_t>(y) * bitmap.stride + x * 4]; };
            require(at(5, 5) == 0 && at(bitmap.width / 4, bitmap.height / 2) == 0, "Redaction box must be solid black");
            doc->undo(after.revision);
            require(Renderer::text(doc->snapshot(), 0).find(u"TOPSECRET") != std::u16string::npos, "Undo must restore the page");
            doc->redo(doc->info().revision);
            doc->save(root / ("out" + std::to_string(rotate) + ".pdf"));
            require(!contains(readFile(root / ("out" + std::to_string(rotate) + ".pdf")), "TOPSECRET"), "Saved file must not contain redacted text");
            auto live = doc->info();
            auto wrong = flatten(doc->snapshot(), 1); wrong.width *= 3;
            rejected([&] { doc->redactPage(live.pages[0].id, live.revision, wrong); }, ErrorCode::InvalidSelection);
            rejected([&] { doc->redactPage(live.pages[0].id, live.revision + 9, flatten(doc->snapshot(), 1)); }, ErrorCode::StaleRevision);
            ImagePage jpeg; jpeg.jpeg = true;
            rejected([&] { doc->redactPage(live.pages[0].id, live.revision, jpeg); }, ErrorCode::Unsupported);
        }
        std::cout << "redaction workflows passed\n";
    } catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
