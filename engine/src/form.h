#pragma once
#include "pdfengine/document.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

namespace pdfengine::form {
// Empty when the document's AcroForm can be filled; otherwise the reason it stays read-only.
std::string restriction(QPDFObjectHandle root);
bool hasWidgets(QPDFPageObjectHelper page);
std::vector<FormField> list(QPDFPageObjectHelper page);
// Applies the value to the candidate document (field /V, widget /AS, regenerated /AP).
void set(QPDF& pdf, QPDFPageObjectHelper page, const SetFormValue& request);
}
