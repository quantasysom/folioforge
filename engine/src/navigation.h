#pragma once
#include "pdfengine/document.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

namespace pdfengine::nav {
std::vector<Bookmark> bookmarks(QPDF&);
void addBookmark(QPDF&, const std::string& title, QPDFObjectHandle page);
void renameBookmark(QPDF&, std::uint32_t index, const std::string& title);
void removeBookmark(QPDF&, std::uint32_t index);
// Points bookmarks that targeted `from` at `to` (used when a page object is replaced).
void retarget(QPDF&, QPDFObjectHandle from, QPDFObjectHandle to);
// Removes bookmarks whose destination page is no longer in the document.
void prune(QPDF&);
std::vector<Layer> layers(QPDF&);
void setLayerVisible(QPDF&, std::uint32_t index, bool visible);
void renameLayer(QPDF&, std::uint32_t index, const std::string& name);
}
