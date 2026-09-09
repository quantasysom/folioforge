#pragma once
#include "document.h"
namespace pdfengine {
struct Bitmap { int width{}, height{}, stride{}; Bytes bgra; RevisionId revision{}; PageId page{}; };
class Renderer {
public:
    static Bitmap render(const Snapshot&, std::size_t page, double scale);
    static std::u16string text(const Snapshot&, std::size_t page);
    static void validate(const Snapshot&);
};
}
