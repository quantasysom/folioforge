#pragma once
#include "font_codec.h"
#include "pdfengine/document.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
namespace pdfengine::font { class FontSource; }
namespace pdfengine::textedit {
struct SourceRun {
    TextRun run;
    std::size_t offset{}, length{};
    double advanceUnits{}, maxAdvanceUnits{};
    std::shared_ptr<const GlyphFont> font;
    std::string fontResource;
    double fontSizeOperand{};
};
struct Inventory { std::vector<SourceRun> runs; std::string content, explanation; };
Inventory inspect(QPDFPageObjectHelper, PageId, RevisionId);
// Returns the replacement operators. May add fallback fonts to `page`'s resources.
std::string replacement(QPDF&, QPDFPageObjectHelper page, const SourceRun&, const std::string&, font::FontSource*);
}
