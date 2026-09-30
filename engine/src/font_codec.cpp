#include "font_codec.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>

namespace pdfengine::textedit {
namespace {
using Obj = QPDFObjectHandle;
// Standard PDF font advance metrics, in 1/1000-em units, ASCII 32..126.
constexpr std::array<int, 95> helvetica = {
278,278,355,556,556,889,667,191,333,333,389,584,278,333,278,278,556,556,556,556,556,556,556,556,556,556,278,278,584,584,584,556,1015,667,667,722,722,667,611,778,722,278,500,667,556,833,722,778,667,778,722,667,611,722,667,944,667,667,611,278,278,278,469,556,333,556,556,500,556,556,278,556,556,222,222,500,222,833,556,556,556,556,333,500,278,556,500,722,500,500,500,334,260,334,584};
constexpr std::array<int, 95> helveticaBold = {
278,333,474,556,556,889,722,238,333,333,389,584,278,333,278,278,556,556,556,556,556,556,556,556,556,556,333,333,584,584,584,611,975,722,722,722,722,667,611,778,722,278,556,722,611,833,722,778,667,778,722,667,611,722,667,944,667,667,611,333,278,333,584,556,333,556,611,556,611,556,333,611,611,278,278,556,278,889,611,611,611,611,389,556,333,556,556,778,556,556,500,389,280,389,584};
// Windows-1252 code points for 0x80..0x9F (0 = undefined).
constexpr std::array<char32_t, 32> cp1252High = {
0x20AC,0,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0,0x017D,0,
0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0,0x017E,0x0178};

bool subsetTag(const std::string& base) {
    if (base.size() < 8 || base[6] != '+') return false;
    return std::all_of(base.begin(), base.begin() + 6, [](unsigned char c) { return std::isupper(c); });
}
int hexValue(char c) { return std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::tolower(c) - 'a' + 10; }
std::string utf16Hex(const std::string& hex) {
    std::string out;
    for (std::size_t i = 0; i + 3 < hex.size(); i += 4) {
        auto unit = [&](std::size_t at) { return char32_t(hexValue(hex[at]) << 12 | hexValue(hex[at + 1]) << 8 | hexValue(hex[at + 2]) << 4 | hexValue(hex[at + 3])); };
        char32_t cp = unit(i);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 7 < hex.size()) {
            auto low = unit(i + 4);
            if (low >= 0xDC00 && low < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00); i += 4; }
        }
        if (cp >= 0xD800 && cp < 0xE000) return {};
        appendUtf8(out, cp);
    }
    return out;
}
struct CMapToken { enum Kind { Word, Hex, Open, Close } kind; std::string text; };
std::vector<CMapToken> tokens(const std::string& source) {
    std::vector<CMapToken> out;
    for (std::size_t i = 0; i < source.size() && out.size() < 400000;) {
        char c = source[i];
        if (std::isspace(static_cast<unsigned char>(c))) ++i;
        else if (c == '%') { while (i < source.size() && source[i] != '\n' && source[i] != '\r') ++i; }
        else if (c == '<' && i + 1 < source.size() && source[i + 1] != '<') {
            auto end = source.find('>', i);
            if (end == std::string::npos) break;
            std::string hex;
            for (auto k = i + 1; k < end; ++k) if (std::isxdigit(static_cast<unsigned char>(source[k]))) hex += source[k];
            out.push_back({CMapToken::Hex, hex}); i = end + 1;
        }
        else if (c == '[') { out.push_back({CMapToken::Open, {}}); ++i; }
        else if (c == ']') { out.push_back({CMapToken::Close, {}}); ++i; }
        else { auto start = i; while (i < source.size() && !std::isspace(static_cast<unsigned char>(source[i])) && !std::strchr("<[]%", source[i])) ++i; if (i == start) ++i; else out.push_back({CMapToken::Word, source.substr(start, i - start)}); }
    }
    return out;
}
std::uint32_t code(const std::string& hex) { return hex.size() > 8 ? 0xFFFFFFFF : static_cast<std::uint32_t>(std::stoull(hex.empty() ? "0" : hex, nullptr, 16)); }
bool parseToUnicode(const std::string& source, std::map<std::uint32_t, std::string>& map) {
    auto list = tokens(source);
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].kind != CMapToken::Word) continue;
        if (list[i].text == "beginbfchar") {
            for (++i; i + 1 < list.size() && list[i].kind == CMapToken::Hex && list[i + 1].kind == CMapToken::Hex; i += 2) {
                auto text = utf16Hex(list[i + 1].text);
                if (!text.empty()) map[code(list[i].text)] = text;
                if (map.size() > 70000) return false;
            }
            --i;
        } else if (list[i].text == "beginbfrange") {
            for (++i; i + 2 < list.size() && list[i].kind == CMapToken::Hex && list[i + 1].kind == CMapToken::Hex; ) {
                auto low = code(list[i].text), high = code(list[i + 1].text);
                if (high < low || high - low > 65535) return false;
                if (list[i + 2].kind == CMapToken::Hex) {
                    auto base = utf16Hex(list[i + 2].text);
                    if (!base.empty()) {
                        // Only the final code point advances across the range.
                        std::size_t start = base.size() - 1;
                        while (start > 0 && (static_cast<unsigned char>(base[start]) & 0xC0) == 0x80) --start;
                        auto tail = decodeUtf8(base, start);
                        for (std::uint32_t k = 0; k <= high - low; ++k) { auto text = base.substr(0, start); appendUtf8(text, tail + k); map[low + k] = text; }
                    }
                    i += 3;
                } else if (list[i + 2].kind == CMapToken::Open) {
                    std::size_t at = i + 3; std::uint32_t k = 0;
                    for (; at < list.size() && list[at].kind == CMapToken::Hex && k <= high - low; ++at, ++k) { auto text = utf16Hex(list[at].text); if (!text.empty()) map[low + k] = text; }
                    if (at >= list.size() || list[at].kind != CMapToken::Close) return false;
                    i = at + 1;
                } else return false;
                if (map.size() > 70000) return false;
            }
            --i;
        }
    }
    return true;
}
std::string streamText(Obj stream) {
    if (!stream.isStream()) return {};
    auto data = stream.getStreamData();
    if (data->getSize() > 2 * 1024 * 1024) return {};
    return std::string(reinterpret_cast<const char*>(data->getBuffer()), data->getSize());
}
void finish(GlyphFont& font, const std::map<std::uint32_t, std::string>& available) {
    for (const auto& [c, text] : available) {
        if (font.fromUnicode.find(text) == font.fromUnicode.end()) font.fromUnicode[text] = c;
        font.longestKey = std::max(font.longestKey, text.size());
    }
}
}

std::size_t utf8Length(unsigned char lead) { return lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 0; }
char32_t decodeUtf8(const std::string& s, std::size_t at) {
    auto n = utf8Length(static_cast<unsigned char>(s[at]));
    if (n == 0 || at + n > s.size()) return 0xFFFFFFFF;
    char32_t cp = n == 1 ? static_cast<unsigned char>(s[at]) : static_cast<unsigned char>(s[at]) & (0xFF >> (n + 1));
    for (std::size_t i = 1; i < n; ++i) {
        auto c = static_cast<unsigned char>(s[at + i]);
        if ((c & 0xC0) != 0x80) return 0xFFFFFFFF;
        cp = cp << 6 | (c & 0x3F);
    }
    static constexpr char32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < minimum[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return 0xFFFFFFFF;
    return cp;
}
bool validUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) { auto cp = decodeUtf8(s, i); if (cp == 0xFFFFFFFF) return false; i += utf8Length(static_cast<unsigned char>(s[i])); }
    return true;
}
void appendUtf8(std::string& s, char32_t cp) {
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) { s += static_cast<char>(0xC0 | cp >> 6); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += static_cast<char>(0xE0 | cp >> 12); s += static_cast<char>(0x80 | (cp >> 6 & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { s += static_cast<char>(0xF0 | cp >> 18); s += static_cast<char>(0x80 | (cp >> 12 & 0x3F)); s += static_cast<char>(0x80 | (cp >> 6 & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
}
bool needsShaping(char32_t c) {
    return (c >= 0x0590 && c <= 0x0DFF) || (c >= 0x0E00 && c <= 0x0EFF) || (c >= 0x0F00 && c <= 0x109F) || (c >= 0x1780 && c <= 0x17FF) ||
        (c >= 0x1A00 && c <= 0x1AFF) || (c >= 0x1B00 && c <= 0x1BFF) || (c >= 0xA800 && c <= 0xA8FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF) ||
        (c >= 0x200E && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0x200C || c == 0x200D || (c >= 0x10800 && c <= 0x10FFF) || (c >= 0x1E800 && c <= 0x1EFFF);
}

double GlyphFont::width(std::uint32_t c) const { auto it = widths.find(c); return it == widths.end() ? defaultWidth : it->second; }
bool GlyphFont::decode(const std::string& bytes, std::string& text, double& advance) const {
    if (twoByte && bytes.size() % 2) return false;
    for (std::size_t i = 0; i < bytes.size(); i += twoByte ? 2 : 1) {
        std::uint32_t c = static_cast<unsigned char>(bytes[i]);
        if (twoByte) c = c << 8 | static_cast<unsigned char>(bytes[i + 1]);
        auto mapped = toUnicode.find(c);
        if (mapped == toUnicode.end()) return false;
        text += mapped->second; advance += width(c);
    }
    return true;
}
bool GlyphFont::encodeAt(const std::string& text, std::size_t at, std::uint32_t& out, std::size_t& consumed) const {
    std::size_t length = 0, position = at;
    std::size_t bestLength = 0; std::uint32_t best = 0;
    while (position < text.size() && length < longestKey) {
        auto step = utf8Length(static_cast<unsigned char>(text[position]));
        if (step == 0) return false;
        position += step; length = position - at;
        auto found = fromUnicode.find(text.substr(at, length));
        if (found != fromUnicode.end()) { best = found->second; bestLength = length; }
    }
    if (!bestLength) return false;
    out = best; consumed = bestLength;
    return true;
}
std::string GlyphFont::bytesFor(std::uint32_t c) const {
    std::string out;
    if (twoByte) out += static_cast<char>(c >> 8 & 0xFF);
    out += static_cast<char>(c & 0xFF);
    return out;
}

std::shared_ptr<const GlyphFont> loadFont(Obj dict) {
    if (!dict.isDictionary()) return {};
    auto font = std::make_shared<GlyphFont>();
    auto base = dict.getKey("/BaseFont");
    std::string baseName = base.isName() ? base.getName().substr(1) : "";
    font->name = subsetTag(baseName) ? baseName.substr(7) : baseName;
    auto subtype = dict.getKey("/Subtype");
    std::map<std::uint32_t, std::string> toUnicode;
    bool hasToUnicode = dict.getKey("/ToUnicode").isStream();
    if (hasToUnicode && !parseToUnicode(streamText(dict.getKey("/ToUnicode")), toUnicode)) return {};
    if (hasToUnicode && toUnicode.empty()) return {};
    if (subtype.isNameAndEquals("/Type0")) {
        auto encoding = dict.getKey("/Encoding");
        auto descendants = dict.getKey("/DescendantFonts");
        if (!encoding.isNameAndEquals("/Identity-H") || !hasToUnicode || !descendants.isArray() || descendants.getArrayNItems() != 1) return {};
        auto cid = descendants.getArrayItem(0);
        if (!cid.isDictionary() || !(cid.getKey("/Subtype").isNameAndEquals("/CIDFontType2") || cid.getKey("/Subtype").isNameAndEquals("/CIDFontType0"))) return {};
        font->twoByte = true;
        font->defaultWidth = cid.getKey("/DW").isNumber() ? cid.getKey("/DW").getNumericValue() : 1000;
        auto w = cid.getKey("/W");
        if (w.isArray()) {
            auto items = w.getArrayAsVector();
            for (std::size_t i = 0; i < items.size();) {
                if (!items[i].isInteger() || i + 1 >= items.size()) return {};
                auto first = items[i].getIntValue();
                if (items[i + 1].isArray()) {
                    auto values = items[i + 1].getArrayAsVector();
                    for (std::size_t k = 0; k < values.size(); ++k) if (values[k].isNumber()) font->widths[static_cast<std::uint32_t>(first + k)] = values[k].getNumericValue();
                    i += 2;
                } else {
                    if (i + 2 >= items.size() || !items[i + 1].isInteger() || !items[i + 2].isNumber()) return {};
                    auto last = items[i + 1].getIntValue();
                    if (last < first || last - first > 65535) return {};
                    for (auto k = first; k <= last; ++k) font->widths[static_cast<std::uint32_t>(k)] = items[i + 2].getNumericValue();
                    i += 3;
                }
            }
        }
        for (const auto& [c, text] : toUnicode) if (c > 0xFFFF) return {};
        font->toUnicode = toUnicode;
        finish(*font, toUnicode);
        return font;
    }
    if (!(subtype.isNameAndEquals("/Type1") || subtype.isNameAndEquals("/TrueType") || subtype.isNameAndEquals("/MMType1"))) return {};
    auto encoding = dict.getKey("/Encoding");
    bool standard = encoding.isNull() || encoding.isNameAndEquals("/StandardEncoding");
    bool winAnsi = encoding.isNameAndEquals("/WinAnsiEncoding");
    bool differences = false;
    if (encoding.isDictionary()) {
        differences = encoding.hasKey("/Differences");
        standard = false;
        winAnsi = encoding.getKey("/BaseEncoding").isNameAndEquals("/WinAnsiEncoding");
    }
    font->standardEncoding = standard;
    const bool plainStandard14 = dict.getKey("/FontDescriptor").isNull() && !dict.hasKey("/Widths") && !hasToUnicode && (standard || winAnsi) && !differences &&
        (baseName == "Helvetica" || baseName == "Helvetica-Bold" || baseName == "Helvetica-Oblique" || baseName == "Helvetica-BoldOblique" ||
         baseName == "Courier" || baseName == "Courier-Bold" || baseName == "Courier-Oblique" || baseName == "Courier-BoldOblique");
    if (plainStandard14) {
        const bool courier = baseName.starts_with("Courier"), bold = baseName.find("Bold") != std::string::npos;
        for (unsigned c = 32; c <= 126; ++c) {
            // StandardEncoding maps these two ASCII positions to typographic quotes.
            if (standard && (c == 39 || c == 96)) continue;
            toUnicode[c] = std::string(1, static_cast<char>(c));
            font->widths[c] = courier ? 600 : bold ? helveticaBold[c - 32] : helvetica[c - 32];
        }
        font->toUnicode = toUnicode;
        finish(*font, toUnicode);
        return font;
    }
    auto widths = dict.getKey("/Widths"), first = dict.getKey("/FirstChar");
    if (!widths.isArray() || !first.isInteger() || first.getIntValue() < 0 || first.getIntValue() > 255 || widths.getArrayNItems() > 256) return {};
    for (int i = 0; i < widths.getArrayNItems(); ++i) {
        auto item = widths.getArrayItem(i);
        if (!item.isNumber()) return {};
        auto c = static_cast<std::uint32_t>(first.getIntValue() + i);
        if (c > 255) return {};
        font->widths[c] = item.getNumericValue();
    }
    std::map<std::uint32_t, std::string> available;
    if (hasToUnicode) {
        for (const auto& [c, text] : toUnicode) {
            if (c > 255) return {};
            auto w = font->widths.find(c);
            if (w != font->widths.end() && w->second > 0) available[c] = text;
        }
    } else {
        // Without ToUnicode the standard Windows encoding is the only reliable mapping; subset fonts cannot be trusted to have other glyphs.
        if (!winAnsi || differences || subsetTag(baseName)) return {};
        for (const auto& [c, w] : font->widths) {
            if (w <= 0 || c < 32) continue;
            char32_t cp = c < 127 || c >= 160 ? c : (c >= 128 && c < 160 ? cp1252High[c - 128] : 0);
            if (cp && c != 127) { std::string text; appendUtf8(text, cp); available[c] = text; }
        }
    }
    if (available.empty()) return {};
    font->toUnicode = available;
    finish(*font, available);
    return font;
}
}
