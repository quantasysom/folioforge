#pragma once
#include "pdfengine/document.h"
#include <qpdf/QPDFPageObjectHelper.hh>
namespace pdfengine::textedit {
struct SourceRun {
    TextRun run;
    std::size_t offset{}, length{};
    double advanceUnits{};
    bool standardEncoding{};
};
struct Inventory { std::vector<SourceRun> runs; std::string content, explanation; };
Inventory inspect(QPDFPageObjectHelper, PageId, RevisionId);
std::string replacement(const SourceRun&, const std::string&);
}
