#pragma once
#include "pdfengine/document.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

namespace pdfengine::textedit { struct GlyphFont; }
namespace pdfengine::annot {
// Greedy word wrap of single-byte-encoded text; widths come from the font. Newlines start a new line.
std::vector<std::string> wrapText(const std::string& text, double fontSize, double maxWidth, const textedit::GlyphFont& font);
constexpr const char* namePrefix = "FolioForge-";
// Builds the annotation dictionary with a generated appearance stream (so every viewer draws it).
// Name of an existing annotation (/NM), empty when it has none.
std::string nameOf(QPDFObjectHandle annotation);
QPDFObjectHandle create(QPDF&, const AddAnnotation&, const std::string& name);
// True when the annotation is a passive markup type FolioForge can safely carry through edits.
bool preservable(QPDFObjectHandle annotation);
std::vector<Annotation> list(QPDFPageObjectHelper page);
}
