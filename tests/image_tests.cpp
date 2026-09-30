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
// 16x8 JPEG: left half red, right half blue.
const Bytes redBlueJpeg = {255,216,255,224,0,16,74,70,73,70,0,1,1,1,0,96,0,96,0,0,255,219,0,67,0,2,1,1,1,1,1,2,1,1,1,2,2,2,2,2,4,3,2,2,2,2,5,4,4,3,4,6,5,6,6,6,5,6,6,6,7,9,8,6,7,9,7,6,6,8,11,8,9,10,10,10,10,10,6,8,11,12,11,10,12,9,10,10,10,255,219,0,67,1,2,2,2,2,2,2,5,3,3,5,10,7,6,7,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,255,194,0,17,8,0,8,0,16,3,1,17,0,2,17,1,3,17,1,255,196,0,20,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,255,196,0,22,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,9,7,8,255,218,0,12,3,1,0,2,16,3,16,0,0,1,23,148,239,226,21,80,215,255,196,0,20,16,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,16,255,218,0,8,1,1,0,1,5,2,63,255,196,0,20,17,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,16,255,218,0,8,1,3,1,1,63,1,63,255,196,0,20,17,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,16,255,218,0,8,1,2,1,1,63,1,63,255,196,0,20,16,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,16,255,218,0,8,1,1,0,6,63,2,63,255,196,0,20,16,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,16,255,218,0,8,1,1,0,1,63,33,63,255,218,0,12,3,1,0,2,0,3,0,0,0,16,3,255,196,0,20,17,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,16,255,218,0,8,1,3,1,1,63,16,63,255,196,0,20,17,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,16,255,218,0,8,1,2,1,1,63,16,63,255,196,0,20,16,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,16,255,218,0,8,1,1,0,1,63,16,63,255,217};
std::array<int, 3> pixel(const Bitmap& bitmap, int x, int y) {
    auto offset = static_cast<std::size_t>(y) * bitmap.stride + x * 4;
    return {bitmap.bgra[offset + 2], bitmap.bgra[offset + 1], bitmap.bgra[offset]};
}
}
int main() {
    auto root = std::filesystem::temp_directory_path() / ("folio-image-tests-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(p, ec); } } cleanup{root};
    try {
        auto jpeg = jpegImage(redBlueJpeg);
        require(jpeg.width == 16 && jpeg.height == 8 && jpeg.components == 3 && jpeg.jpeg, "JPEG header not parsed");
        auto doc = Document::createFromImage(jpeg);
        auto info = doc->info();
        require(info.pages.size() == 1 && info.dirty && info.editable, "Image document must be an editable unsaved PDF");
        require(std::abs(info.pages[0].width - 12) < 0.01 && std::abs(info.pages[0].height - 6) < 0.01, "96 DPI page size wrong");
        auto bitmap = Renderer::render(doc->snapshot(), 0, 8);
        auto left = pixel(bitmap, 10, bitmap.height / 2), right = pixel(bitmap, bitmap.width - 10, bitmap.height / 2);
        require(left[0] > 200 && left[2] < 60 && right[2] > 200 && right[0] < 60, "JPEG page did not render its pixels");
        auto path = root / "photo.pdf"; Renderer::validate(doc->snapshot()); doc->save(path);
        require(Document::open(path)->info().pages.size() == 1, "Saved image PDF did not reopen");

        ImagePage raw; raw.width = 4; raw.height = 2; raw.components = 3;
        for (int i = 0; i < 8; ++i) { raw.data.push_back(0); raw.data.push_back(200); raw.data.push_back(0); }
        raw.alpha.assign(8, 255); raw.dpi = 72;
        doc->insertImage(raw, info.pages[0].id, info.revision);
        info = doc->info();
        require(info.pages.size() == 2 && info.pages[1].width == 4 && info.pages[1].height == 2, "Inserted raw image page missing");
        auto green = Renderer::render(doc->snapshot(), 1, 8);
        require(pixel(green, green.width / 2, green.height / 2)[1] > 150, "Raw RGB page did not render");
        doc->execute({CommandKind::RotateRight, info.pages[1].id, info.revision});
        doc->undo(doc->info().revision); doc->undo(doc->info().revision);
        require(doc->info().pages.size() == 1, "Undo did not remove inserted image page");

        ImagePage large; large.width = 3000; large.height = 1500; large.components = 1; large.data.assign(3000 * 1500, 128);
        auto scaled = Document::createFromImage(large)->info().pages[0];
        require(std::abs(scaled.width - 842) < 0.01 && std::abs(scaled.height - 421) < 0.01, "Large images must fit an A4-sized page");

        auto stale = doc->info().revision;
        rejected([&] { doc->insertImage(raw, info.pages[0].id, stale - 1); }, ErrorCode::StaleRevision);
        rejected([&] { doc->insertImage(raw, 999999, stale); }, ErrorCode::InvalidSelection);
        auto truncated = raw; truncated.data.pop_back();
        rejected([&] { Document::createFromImage(truncated); }, ErrorCode::InvalidDocument);
        auto huge = raw; huge.width = 40000;
        rejected([&] { Document::createFromImage(huge); }, ErrorCode::ResourceLimit);
        rejected([&] { jpegImage(Bytes{1, 2, 3, 4, 5}); }, ErrorCode::InvalidDocument);
        auto cmyk = redBlueJpeg; for (std::size_t i = 0; i + 9 < cmyk.size(); ++i) if (cmyk[i] == 0xff && cmyk[i + 1] >= 0xc0 && cmyk[i + 1] <= 0xc2) { cmyk[i + 9] = 4; break; }
        rejected([&] { jpegImage(cmyk); }, ErrorCode::Unsupported);
        std::cout << checks << " image checks passed: JPEG passthrough, raw RGB+alpha, page sizing, insertion, undo, save/reopen, and rejection paths\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
