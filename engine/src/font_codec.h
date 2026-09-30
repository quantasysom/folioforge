#pragma once
#include <qpdf/QPDFObjectHandle.hh>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace pdfengine::textedit {
// Maps a PDF font's character codes to Unicode text and back, using only glyphs the font is known to contain.
struct GlyphFont {
    std::string name;
    bool twoByte{}, standardEncoding{};
    std::map<std::uint32_t, std::string> toUnicode;
    std::map<std::string, std::uint32_t> fromUnicode;
    std::map<std::uint32_t, double> widths;
    double defaultWidth{};
    std::size_t longestKey{1};

    double width(std::uint32_t code) const;
    bool decode(const std::string& bytes, std::string& text, double& advance) const;
    // Encodes the longest available piece of `text` at `at`; false when that character has no glyph here.
    bool encodeAt(const std::string& text, std::size_t at, std::uint32_t& code, std::size_t& consumed) const;
    std::string bytesFor(std::uint32_t code) const;
};

std::shared_ptr<const GlyphFont> loadFont(QPDFObjectHandle font);

std::size_t utf8Length(unsigned char lead);
bool validUtf8(const std::string&);
char32_t decodeUtf8(const std::string&, std::size_t at);
void appendUtf8(std::string&, char32_t);
// Scripts that need contextual shaping, reordering or mark positioning are outside the supported set.
bool needsShaping(char32_t);
}
