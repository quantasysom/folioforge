#include "pdfengine/document.h"
#include "pdfengine/renderer.h"
#include <array>
#include <cmath>
#include <iostream>
#include <random>

using namespace pdfengine;
namespace {
int checks{};
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class Fn> void rejected(Fn fn, ErrorCode code) {
    try { fn(); } catch (const Error& error) { require(error.code == code, "Wrong error category"); return; }
    throw std::runtime_error("Expected rejection");
}
std::array<int, 3> at(const Bitmap& b, double x, double y) {
    auto o = static_cast<std::size_t>(b.height - 1 - static_cast<int>(y)) * b.stride + static_cast<int>(x) * 4;
    return {b.bgra[o + 2], b.bgra[o + 1], b.bgra[o]};
}
bool near(std::array<int, 3> c, int r, int g, int b, int tol = 30) { return std::abs(c[0] - r) <= tol && std::abs(c[1] - g) <= tol && std::abs(c[2] - b) <= tol; }
AddAnnotation make(const std::shared_ptr<Document>& doc, AnnotationKind kind, double x0, double y0, double x1, double y1, std::array<double, 3> color) {
    AddAnnotation a; a.page = doc->info().pages[0].id; a.expectedRevision = doc->info().revision;
    a.kind = kind; a.x0 = x0; a.y0 = y0; a.x1 = x1; a.y1 = y1; a.color = color; return a;
}
}
int main() {
    auto root = std::filesystem::temp_directory_path() / ("folio-annot-tests-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(p, ec); } } cleanup{root};
    try {
        auto doc = Document::create();
        auto page = doc->info().pages[0].id;
        require(doc->annotations(page).empty(), "Blank page must have no annotations");

        doc->addAnnotation(make(doc, AnnotationKind::Highlight, 100, 700, 200, 720, {1, 1, 0}));
        doc->addAnnotation(make(doc, AnnotationKind::Underline, 100, 600, 200, 620, {1, 0, 0}));
        doc->addAnnotation(make(doc, AnnotationKind::StrikeOut, 100, 500, 200, 520, {0, 0, 1}));
        doc->addAnnotation(make(doc, AnnotationKind::Rectangle, 300, 700, 400, 760, {0, 0.6, 0}));
        doc->addAnnotation(make(doc, AnnotationKind::Ellipse, 300, 600, 400, 660, {0.6, 0, 0.6}));
        auto note = make(doc, AnnotationKind::Note, 300, 500, 0, 0, {1, 0.87, 0.2}); note.contents = "Café note ✓";
        doc->addAnnotation(note);
        auto free = make(doc, AnnotationKind::FreeText, 100, 400, 260, 420, {0, 0, 0}); free.contents = "Hello annotation world, wrapped\nsecond paragraph"; free.fontSize = 14;
        doc->addAnnotation(free);
        auto ink = make(doc, AnnotationKind::Ink, 0, 0, 0, 0, {1, 0, 0}); ink.lineWidth = 6;
        ink.strokes = {{{100, 300}, {150, 340}, {200, 300}}, {{300, 300}}};
        doc->addAnnotation(ink);

        auto list = doc->annotations(page);
        require(list.size() == 8, "All annotations must be listed");
        require(list[0].kind == AnnotationKind::Highlight && list[5].contents == "Café note ✓" && list[6].kind == AnnotationKind::FreeText, "Annotation metadata wrong");
        for (auto& a : list) require(a.removable, "Created annotations must be removable");

        auto bitmap = Renderer::render(doc->snapshot(), 0, 1);
        require(near(at(bitmap, 150, 710), 255, 255, 0), "Highlight not rendered");
        require(near(at(bitmap, 150, 601), 255, 0, 0), "Underline not rendered");
        bool struck = false;
        for (int dy = -2; dy <= 2; ++dy) struck |= (at(bitmap, 150, 510 + dy)[2] > at(bitmap, 150, 510 + dy)[0] + 60);
        require(struck, "Strike-out not rendered");
        bool boxed = false, ring = false;
        for (int dx = -1; dx <= 2; ++dx) boxed |= at(bitmap, 300 + dx, 730)[1] > at(bitmap, 300 + dx, 730)[0] + 50;
        require(boxed, "Rectangle outline not rendered");
        require(near(at(bitmap, 350, 730), 255, 255, 255), "Rectangle must be hollow");
        for (int dy = -2; dy <= 2; ++dy) ring |= at(bitmap, 350, 600 + dy)[1] + 50 < at(bitmap, 350, 600 + dy)[0];
        require(ring, "Ellipse outline not rendered");
        require(at(bitmap, 310, 510) != std::array<int, 3>{255, 255, 255}, "Note icon not rendered");
        require(near(at(bitmap, 150, 338), 255, 0, 0, 60), "Ink stroke not rendered");

        // Save, reopen, and keep everything editable.
        auto path = root / "annotated.pdf"; Renderer::validate(doc->snapshot()); doc->save(path);
        auto reopened = Document::open(path);
        require(reopened->info().editable && reopened->annotations(reopened->info().pages[0].id).size() == 8, "Annotations must survive save and reopen");
        require(near(at(Renderer::render(reopened->snapshot(), 0, 1), 150, 710), 255, 255, 0), "Reopened highlight not rendered");

        // Removal, undo, redo.
        auto info = reopened->info();
        reopened->removeAnnotation(info.pages[0].id, 0, info.revision);
        require(reopened->annotations(info.pages[0].id).size() == 7, "Removal failed");
        require(near(at(Renderer::render(reopened->snapshot(), 0, 1), 150, 710), 255, 255, 255), "Removed highlight still drawn");
        reopened->undo(reopened->info().revision);
        require(reopened->annotations(info.pages[0].id).size() == 8, "Undo must restore the annotation");
        reopened->redo(reopened->info().revision);
        require(reopened->annotations(info.pages[0].id).size() == 7, "Redo must remove it again");

        // A duplicated page shares no annotation state with the original.
        auto shared = Document::open(path);
        auto sInfo = shared->info();
        // Page duplication is not exposed here; verify stale revisions are rejected instead.
        rejected([&] { shared->removeAnnotation(sInfo.pages[0].id, 0, sInfo.revision + 5); }, ErrorCode::StaleRevision);

        // Validation.
        auto bad = make(doc, AnnotationKind::Highlight, 10, 10, 10, 10, {1, 1, 0});
        rejected([&] { doc->addAnnotation(bad); }, ErrorCode::InvalidSelection);
        auto nan = make(doc, AnnotationKind::Rectangle, 0, 0, std::nan(""), 40, {1, 0, 0});
        rejected([&] { doc->addAnnotation(nan); }, ErrorCode::InvalidSelection);
        auto color = make(doc, AnnotationKind::Rectangle, 0, 0, 40, 40, {2, 0, 0});
        rejected([&] { doc->addAnnotation(color); }, ErrorCode::InvalidSelection);
        auto text = make(doc, AnnotationKind::FreeText, 10, 10, 100, 40, {0, 0, 0}); text.contents = "日本";
        rejected([&] { doc->addAnnotation(text); }, ErrorCode::Unsupported);
        rejected([&] { doc->removeAnnotation(page, 99, doc->info().revision); }, ErrorCode::InvalidSelection);
        std::cout << "annotation workflows passed (" << checks << " checks)\n";
    } catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
