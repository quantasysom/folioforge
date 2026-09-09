#include "pdfengine/renderer.h"
#include <fpdfview.h>
#include <fpdf_text.h>
#include <cmath>
#include <mutex>

namespace pdfengine {
namespace {
std::mutex libraryMutex;
struct Library {
    Library() { FPDF_LIBRARY_CONFIG config{}; config.version = 2; FPDF_InitLibraryWithConfig(&config); }
    ~Library() { FPDF_DestroyLibrary(); }
};
struct Handle {
    FPDF_DOCUMENT value{};
    explicit Handle(const Snapshot& snapshot) {
        static Library library;
        if (!snapshot.bytes || snapshot.bytes->empty()) throw Error(ErrorCode::InvalidDocument, "Empty rendering snapshot.");
        value = FPDF_LoadMemDocument64(snapshot.bytes->data(), snapshot.bytes->size(), nullptr);
        if (!value) throw Error(ErrorCode::InvalidDocument, "PDFium could not open the committed PDF.");
    }
    ~Handle() { FPDF_CloseDocument(value); }
};
struct Page {
    FPDF_PAGE value{};
    Page(Handle& doc, std::size_t index) {
        if (index >= static_cast<std::size_t>(FPDF_GetPageCount(doc.value))) throw Error(ErrorCode::InvalidSelection, "Page is out of range.");
        value = FPDF_LoadPage(doc.value, static_cast<int>(index));
        if (!value) throw Error(ErrorCode::InvalidDocument, "This page could not be rendered.");
    }
    ~Page() { FPDF_ClosePage(value); }
};
Bitmap renderPage(Handle& doc, const Snapshot& snapshot, std::size_t index, double scale) {
    Page page(doc, index);
    double width = std::ceil(FPDF_GetPageWidthF(page.value) * scale);
    double height = std::ceil(FPDF_GetPageHeightF(page.value) * scale);
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1 || width > 16384 || height > 16384 || width * height > 32000000)
        throw Error(ErrorCode::ResourceLimit, "Page raster exceeds the 32-megapixel limit. Reduce the zoom.");
    Bitmap result;
    result.width = static_cast<int>(width); result.height = static_cast<int>(height); result.stride = result.width * 4;
    result.bgra.resize(static_cast<std::size_t>(result.stride) * result.height);
    result.revision = snapshot.revision; result.page = snapshot.pages.at(index);
    auto bitmap = FPDFBitmap_CreateEx(result.width, result.height, FPDFBitmap_BGRA, result.bgra.data(), result.stride);
    if (!bitmap) throw Error(ErrorCode::ResourceLimit, "Unable to allocate the page bitmap.");
    FPDFBitmap_FillRect(bitmap, 0, 0, result.width, result.height, 0xffffffff);
    FPDF_RenderPageBitmap(bitmap, page.value, 0, 0, result.width, result.height, 0, FPDF_ANNOT);
    FPDFBitmap_Destroy(bitmap);
    return result;
}
}
Bitmap Renderer::render(const Snapshot& snapshot, std::size_t page, double scale) {
    std::lock_guard lock(libraryMutex);
    if (!std::isfinite(scale) || scale <= 0 || scale > 8) throw Error(ErrorCode::ResourceLimit, "Zoom must be greater than zero and at most 800%.");
    Handle doc(snapshot);
    return renderPage(doc, snapshot, page, scale);
}
std::u16string Renderer::text(const Snapshot& snapshot, std::size_t index) {
    std::lock_guard lock(libraryMutex);
    Handle doc(snapshot); Page page(doc, index);
    auto textPage = FPDFText_LoadPage(page.value);
    if (!textPage) throw Error(ErrorCode::InvalidDocument, "Page text could not be extracted.");
    struct Cleanup { FPDF_TEXTPAGE page; ~Cleanup() { FPDFText_ClosePage(page); } } cleanup{textPage};
    int count = FPDFText_CountChars(textPage);
    if (count < 0 || count > 4000000) throw Error(ErrorCode::ResourceLimit, "Page text exceeds the preview limit.");
    std::vector<unsigned short> buffer(static_cast<std::size_t>(count) * 2 + 1);
    int length = FPDFText_GetText(textPage, 0, count, buffer.data());
    std::u16string result;
    for (int i = 0; i < length - 1; ++i) result.push_back(static_cast<char16_t>(buffer[i]));
    return result;
}
void Renderer::validate(const Snapshot& snapshot) {
    std::lock_guard lock(libraryMutex);
    Handle doc(snapshot);
    if (FPDF_GetPageCount(doc.value) != static_cast<int>(snapshot.pages.size()))
        throw Error(ErrorCode::InvalidDocument, "Renderer and engine disagree on the page count.");
    for (std::size_t i = 0; i < snapshot.pages.size(); ++i) renderPage(doc, snapshot, i, 0.1);
}
}
