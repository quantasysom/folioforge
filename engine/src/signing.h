#pragma once
#include "pdfengine/document.h"
namespace pdfengine::signing {
// Produces a copy of a PDF with an invisible digital signature appended as an incremental update.
Bytes sign(const Bytes& pdf, const SignOptions&);
std::vector<SignatureCheck> verify(const Bytes& pdf);
}
