#include "text_edit.h"
#include "font_codec.h"
#include "sfnt.h"
#include <qpdf/QPDFObjectHandle.hh>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <cmath>
#include <cstdio>
#include <vector>

namespace pdfengine::textedit {
namespace {
using Obj = QPDFObjectHandle;
struct Matrix {
    double a{1}, b{}, c{}, d{1}, e{}, f{};
    Matrix then(const Matrix& m) const { // m(this(point))
        return {m.a*a+m.c*b, m.b*a+m.d*b, m.a*c+m.c*d, m.b*c+m.d*d, m.a*e+m.c*f+m.e, m.b*e+m.d*f+m.f};
    }
    bool horizontal() const { return a > 0 && d > 0 && std::abs(b) < 1e-8 && std::abs(c) < 1e-8; }
};
struct Graphics {
    Matrix ctm;
    std::shared_ptr<const GlyphFont> font;
    std::string fontResource, fontName;
    double size{}, charSpace{}, wordSpace{}, hscale{100}, leading{}, rise{}, mode{};
    bool safe{true};
};
struct Token { Obj value; std::size_t offset{}, length{}; };
class Parser : public Obj::ParserCallbacks {
public:
    Parser(Obj fonts, PageId page, RevisionId revision, double w, double h) : fonts(fonts), page(page), revision(revision), pageWidth(w), pageHeight(h) {}
    std::vector<SourceRun> runs;
    bool malformed{};
    void handleObject(Obj value, size_t offset, size_t length) override {
        if (++tokens > 100000) { malformed = true; terminateParsing(); }
        if (!value.isOperator()) {
            if (operands.size() >= 32 || value.isInlineImage()) { malformed = true; terminateParsing(); }
            operands.push_back({value, offset, length}); return;
        }
        const auto op = value.getOperatorValue();
        if (op == "q") { if (!operands.empty() || stack.size() >= 64) malformed = true; else stack.push_back(gs); }
        else if (op == "Q") { if (!operands.empty() || stack.empty()) malformed = true; else { gs = stack.back(); stack.pop_back(); } }
        else if (op == "cm") { if (numbers(6)) gs.ctm = matrix().then(gs.ctm); else malformed = true; }
        else if (op == "BT") { if (inText || !operands.empty()) malformed = true; inText = true; tm = line = {}; positionKnown = true; }
        else if (op == "ET") { if (!inText || !operands.empty()) malformed = true; inText = false; }
        else if (op == "Tf") {
            gs.font.reset();
            if (operands.size() == 2 && operands[0].value.isName() && operands[1].value.isNumber()) {
                gs.size = operands[1].value.getNumericValue();
                gs.fontResource = operands[0].value.getName();
                auto cached = cache.find(gs.fontResource);
                if (cached == cache.end()) {
                    auto dict = fonts.getKeyIfDict(gs.fontResource);
                    cached = cache.emplace(gs.fontResource, dict.isNull() ? nullptr : loadFont(dict)).first;
                    if (cached->second && dict.getKey("/BaseFont").isName()) gs.fontName = dict.getKey("/BaseFont").getName().substr(1);
                    names[gs.fontResource] = gs.fontName;
                }
                gs.font = cached->second; gs.fontName = names[gs.fontResource];
            } else malformed = true;
        }
        else if (op == "Tm") { if (inText && numbers(6)) { tm = line = matrix(); positionKnown = true; } else malformed = true; }
        else if (op == "Td" || op == "TD") {
            if (inText && numbers(2)) {
                auto x = number(0), y = number(1); if (op == "TD") gs.leading = -y;
                translateLine(x, y);
            } else malformed = true;
        }
        else if (op == "T*") { if (inText && operands.empty()) translateLine(0, -gs.leading); else malformed = true; }
        else if (op == "Tc" || op == "Tw" || op == "Tz" || op == "TL" || op == "Ts" || op == "Tr") {
            if (!numbers(1)) malformed = true;
            else if (op == "Tc") gs.charSpace = number(0);
            else if (op == "Tw") gs.wordSpace = number(0);
            else if (op == "Tz") gs.hscale = number(0);
            else if (op == "TL") gs.leading = number(0);
            else if (op == "Ts") gs.rise = number(0);
            else gs.mode = number(0);
        }
        else if (op == "Tj" || op == "TJ") show(op, offset + length);
        else if (op == "W" || op == "W*" || op == "gs" || op == "sh") gs.safe = false;
        else if (op == "'" || op == "\"") { malformed = true; }
        else if (op == "BMC" || op == "BDC" || op == "EMC" || op == "MP" || op == "DP" || op == "BX" || op == "EX") {
            // Marked content may carry ActualText or other semantic replacements.
            malformed = true;
        }
        else {
            const std::string known = "|m|l|c|v|y|h|re|S|s|f|F|f*|B|B*|b|b*|n|w|J|j|M|d|ri|i|G|g|RG|rg|K|k|CS|cs|SC|SCN|sc|scn|Do|";
            if (known.find("|" + op + "|") == std::string::npos) malformed = true;
        }
        operands.clear();
    }
    void handleEOF() override { if (inText || !stack.empty() || !operands.empty()) malformed = true; }
private:
    Obj fonts;
    std::map<std::string, std::shared_ptr<const GlyphFont>> cache;
    std::map<std::string, std::string> names;
    PageId page;
    RevisionId revision;
    double pageWidth, pageHeight;
    Graphics gs;
    std::vector<Graphics> stack;
    Matrix tm, line;
    bool inText{}, positionKnown{true};
    std::size_t tokens{};
    std::vector<Token> operands;
    double number(std::size_t i) { return operands[i].value.getNumericValue(); }
    bool numbers(std::size_t count) {
        return operands.size() == count && std::all_of(operands.begin(), operands.end(), [](const Token& t) {
            return t.value.isNumber() && std::isfinite(t.value.getNumericValue()) && std::abs(t.value.getNumericValue()) < 1e7;
        });
    }
    Matrix matrix() { return {number(0), number(1), number(2), number(3), number(4), number(5)}; }
    void translateLine(double x, double y) { line.e += x*line.a + y*line.c; line.f += x*line.b + y*line.d; tm = line; positionKnown = true; }
    void show(const std::string& op, std::size_t end) {
        if (!inText || operands.size() != 1) { malformed = true; return; }
        auto argument = operands.front().value;
        std::vector<Obj> parts;
        if (op == "Tj" && argument.isString()) parts.push_back(argument);
        else if (op == "TJ" && argument.isArray() && argument.getArrayNItems() <= 4096) parts = argument.getArrayAsVector();
        else { positionKnown = false; return; }
        bool supported = gs.font && gs.size > 0 && gs.size <= 500;
        bool beginsWithString = !parts.empty() && parts.front().isString();
        bool editable = true;
        std::string text;
        double advance = 0;
        for (auto part : parts) {
            if (part.isString() && supported) {
                std::string piece;
                if (!gs.font->decode(part.getStringValue(), piece, advance)) supported = false;
                else {
                    for (std::size_t i = 0; i < piece.size(); i += utf8Length(static_cast<unsigned char>(piece[i]))) {
                        auto cp = decodeUtf8(piece, i);
                        if (cp == 0xFFFFFFFF || cp < 32 || needsShaping(cp)) editable = false;
                    }
                    if (piece.size() + text.size() > 4096) editable = false;
                    text += piece;
                }
            } else if (part.isNumber() && std::isfinite(part.getNumericValue()) && std::abs(part.getNumericValue()) < 1e7) advance -= part.getNumericValue();
            else supported = false;
        }
        if (!supported) { positionKnown = false; return; }
        // Nonzero spacing and glyph stretch are not qualified for replacement.
        bool spacing = gs.charSpace == 0 && gs.wordSpace == 0 && gs.hscale == 100 && gs.rise == 0;
        Matrix combined = tm.then(gs.ctm);
        double renderedSize = gs.size * combined.d;
        double renderedWidth = advance * gs.size / 1000.0 * combined.a;
        if (positionKnown && editable && gs.safe && spacing && gs.mode == 0 && beginsWithString && !text.empty() && advance > 0 &&
            tm.horizontal() && gs.ctm.horizontal() && std::abs(combined.a - combined.d) < 1e-6 &&
            combined.e >= 0 && combined.f - renderedSize*0.3 >= 0 && combined.e + renderedWidth <= pageWidth && combined.f + renderedSize <= pageHeight) {
            TextRun run{operands.front().offset, page, revision, text, gs.fontName, combined.e, combined.f, renderedWidth, renderedSize};
            // Room to the right page edge, in glyph-space units; longer replacements would leave the page.
            double maxAdvance = (pageWidth - combined.e) / (gs.size / 1000.0 * combined.a);
            runs.push_back({std::move(run), operands.front().offset, end - operands.front().offset, advance, maxAdvance, gs.font, gs.fontResource, gs.size});
        }
        if (spacing) { double tx = advance * gs.size / 1000.0; tm.e += tx*tm.a; tm.f += tx*tm.b; }
        else positionKnown = false;
    }
};
}
Inventory inspect(QPDFPageObjectHelper page, PageId id, RevisionId revision) {
    Inventory result;
    result.explanation = "Editable: horizontal text in simple fonts (WinAnsi or ToUnicode-mapped) and Identity-H Type0/CID fonts with a ToUnicode map, including embedded and subset fonts. Characters missing from the font are added from an embedded fallback font. Not yet supported: text clipping, shaped scripts (Arabic, Hebrew, Indic, Thai, ...), vertical writing, marked content, and rotated/cropped pages.";
    auto rotation = page.getAttribute("/Rotate", false);
    auto box = page.getMediaBox().getArrayAsRectangle();
    auto crop = page.getCropBox().getArrayAsRectangle();
    if ((rotation.isInteger() && rotation.getIntValueAsInt() % 360 != 0) || box.llx != 0 || box.lly != 0 ||
        crop.llx != 0 || crop.lly != 0 || crop.urx != box.urx || crop.ury != box.ury ||
        page.getObjectHandle().hasKey("/UserUnit")) return result;
    auto stream = page.getObjectHandle().getKey("/Contents");
    if (!stream.isStream()) { result.explanation = "Text editing currently requires one page content stream. This page remains viewable."; return result; }
    auto data = stream.getStreamData();
    if (data->getSize() > 4*1024*1024) { result.explanation = "This page exceeds the 4 MiB text-analysis limit."; return result; }
    result.content.assign(reinterpret_cast<const char*>(data->getBuffer()), data->getSize());
    Parser parser(page.getAttribute("/Resources", false).getKeyIfDict("/Font"), id, revision, box.urx, box.ury);
    stream.parseAsContents(&parser);
    if (!parser.malformed) result.runs = std::move(parser.runs);
    return result;
}
namespace {
using Obj = QPDFObjectHandle;
std::string utf16(const std::string& text) {
    std::string out; char buffer[8];
    for (std::size_t i = 0; i < text.size(); i += utf8Length(static_cast<unsigned char>(text[i]))) {
        auto cp = decodeUtf8(text, i);
        auto unit = [&](unsigned v) { std::snprintf(buffer, sizeof buffer, "%04X", v & 0xFFFF); out += buffer; };
        if (cp >= 0x10000) { cp -= 0x10000; unit(0xD800 + (cp >> 10)); unit(0xDC00 + (cp & 0x3FF)); } else unit(cp);
    }
    return out;
}
struct Segment { std::shared_ptr<font::Sfnt> face; std::string bytes; std::vector<char32_t> chars; };
Obj embed(QPDF& pdf, const font::Sfnt& face, const std::map<std::uint32_t, std::string>& glyphs, const std::string& label) {
    std::set<std::uint32_t> ids;
    for (const auto& item : glyphs) ids.insert(item.first);
    auto file = face.subset(ids);
    std::size_t hash = 1469598103934665603ull;
    for (auto id : ids) hash = (hash ^ id) * 1099511628211ull;
    std::string tag; for (int i = 0; i < 6; ++i) { tag += static_cast<char>('A' + hash % 26); hash /= 26; }
    std::string base;
    for (char c : face.name) if (std::isalnum(static_cast<unsigned char>(c))) base += c;
    if (base.empty()) base = "Fallback";
    auto scale = [&](double v) { return static_cast<long long>(std::llround(v * 1000.0 / face.unitsPerEm)); };
    auto stream = pdf.newStream(std::string(file.begin(), file.end()));
    stream.replaceDict(Obj::parse("<< /Length1 " + std::to_string(file.size()) + " >>"));
    auto descriptor = Obj::newDictionary();
    descriptor.replaceKey("/Type", Obj::newName("/FontDescriptor"));
    descriptor.replaceKey("/FontName", Obj::newName("/" + tag + "+" + base));
    descriptor.replaceKey("/Flags", Obj::newInteger(4));
    descriptor.replaceKey("/FontBBox", Obj::parse("[" + std::to_string(scale(face.xMin)) + " " + std::to_string(scale(face.yMin)) + " " + std::to_string(scale(face.xMax)) + " " + std::to_string(scale(face.yMax)) + "]"));
    descriptor.replaceKey("/ItalicAngle", Obj::newInteger(0));
    descriptor.replaceKey("/Ascent", Obj::newInteger(scale(face.ascent)));
    descriptor.replaceKey("/Descent", Obj::newInteger(scale(face.descent)));
    descriptor.replaceKey("/CapHeight", Obj::newInteger(scale(face.ascent)));
    descriptor.replaceKey("/StemV", Obj::newInteger(80));
    descriptor.replaceKey("/FontFile2", stream);
    auto widths = Obj::newArray();
    for (const auto& item : glyphs) {
        widths.appendItem(Obj::newInteger(item.first));
        widths.appendItem(Obj::parse("[" + std::to_string(scale(face.advance(item.first))) + "]"));
    }
    auto cid = Obj::newDictionary();
    cid.replaceKey("/Type", Obj::newName("/Font"));
    cid.replaceKey("/Subtype", Obj::newName("/CIDFontType2"));
    cid.replaceKey("/BaseFont", Obj::newName("/" + tag + "+" + base));
    cid.replaceKey("/CIDSystemInfo", Obj::parse("<< /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>"));
    cid.replaceKey("/FontDescriptor", pdf.makeIndirectObject(descriptor));
    cid.replaceKey("/CIDToGIDMap", Obj::newName("/Identity"));
    cid.replaceKey("/DW", Obj::newInteger(1000));
    cid.replaceKey("/W", widths);
    std::string map = "/CIDInit /ProcSet findresource begin 12 dict begin begincmap /CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def /CMapName /Adobe-Identity-UCS def /CMapType 2 def\n1 begincodespacerange <0000> <FFFF> endcodespacerange\n";
    std::size_t count = 0; std::string body;
    auto flush = [&] { if (count) { map += std::to_string(count) + " beginbfchar\n" + body + "endbfchar\n"; body.clear(); count = 0; } };
    for (const auto& item : glyphs) {
        char code[16]; std::snprintf(code, sizeof code, "<%04X> ", item.first);
        body += code + std::string("<") + utf16(item.second) + ">\n";
        if (++count == 100) flush();
    }
    flush();
    map += "endcmap CMapName currentdict /CMap defineresource pop end end\n";
    auto type0 = Obj::newDictionary();
    type0.replaceKey("/Type", Obj::newName("/Font"));
    type0.replaceKey("/Subtype", Obj::newName("/Type0"));
    type0.replaceKey("/BaseFont", Obj::newName("/" + tag + "+" + base));
    type0.replaceKey("/Encoding", Obj::newName("/Identity-H"));
    type0.replaceKey("/DescendantFonts", Obj::newArray(std::vector<Obj>{pdf.makeIndirectObject(cid)}));
    type0.replaceKey("/ToUnicode", pdf.newStream(map));
    (void)label;
    return pdf.makeIndirectObject(type0);
}
std::string number(double v) { return Obj::newReal(v).unparse(); }
}

std::string replacement(QPDF& pdf, QPDFPageObjectHelper page, const SourceRun& source, const std::string& text, font::FontSource* fallback) {
    const auto& codec = *source.font;
    if (text.size() > 4096 || !validUtf8(text)) throw Error(ErrorCode::Unsupported, "The text is too long or is not valid UTF-8.");
    for (std::size_t i = 0; i < text.size(); i += utf8Length(static_cast<unsigned char>(text[i]))) {
        auto cp = decodeUtf8(text, i);
        if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) throw Error(ErrorCode::Unsupported, "Line breaks and control characters are not supported inside a text run.");
        if (needsShaping(cp)) throw Error(ErrorCode::Unsupported, "Scripts that need shaping or right-to-left layout (Arabic, Hebrew, Indic, Thai, ...) are not supported yet.");
    }
    std::vector<Segment> segments;
    double advance = 0;
    for (std::size_t at = 0; at < text.size();) {
        std::uint32_t code; std::size_t used;
        // Spaces between fallback characters stay in the fallback font to avoid needless font switches.
        const bool keepSpace = text[at] == ' ' && !segments.empty() && segments.back().face && segments.back().face->glyph(' ') &&
            at + 1 < text.size() && !codec.encodeAt(text, at + 1, code, used);
        if (!keepSpace && codec.encodeAt(text, at, code, used)) {
            if (segments.empty() || segments.back().face) segments.push_back({});
            segments.back().bytes += codec.bytesFor(code);
            advance += codec.width(code);
            at += used; continue;
        }
        auto cp = decodeUtf8(text, at);
        std::shared_ptr<font::Sfnt> face = fallback ? fallback->find(cp) : nullptr;
        if (!face) {
            std::string shown; appendUtf8(shown, cp);
            throw Error(ErrorCode::Unsupported, "No font with a glyph for U+" + [&] { char b[16]; std::snprintf(b, sizeof b, "%04X", static_cast<unsigned>(cp)); return std::string(b); }() +
                " (" + shown + ") is available. Install a font that covers it or provide one with a fallback font.");
        }
        if (segments.empty() || segments.back().face != face) segments.push_back({face, {}, {}});
        segments.back().chars.push_back(cp);
        advance += std::llround(face->advance(face->glyph(cp)) * 1000.0 / face->unitsPerEm);
        at += utf8Length(static_cast<unsigned char>(text[at]));
    }
    if (advance > source.maxAdvanceUnits + 0.01)
        throw Error(ErrorCode::TextOverflow, "The text would extend past the right edge of the page. Shorten it or split it across lines.");
    if (segments.empty()) return Obj::newString("").unparse() + " Tj";

    std::map<font::Sfnt*, std::map<std::uint32_t, std::string>> used;
    for (const auto& segment : segments) if (segment.face) for (auto cp : segment.chars) { std::string s; appendUtf8(s, cp); used[segment.face.get()][segment.face->glyph(cp)] = s; }
    auto resources = page.getAttribute("/Resources", false);
    auto newResources = resources.isDictionary() ? resources.shallowCopy() : Obj::newDictionary();
    auto fontsDict = newResources.getKey("/Font");
    auto newFonts = fontsDict.isDictionary() ? fontsDict.shallowCopy() : Obj::newDictionary();
    std::map<font::Sfnt*, std::string> names; int counter = 0;
    for (const auto& [face, glyphs] : used) {
        std::string name;
        do name = "/FFallback" + std::to_string(++counter); while (newFonts.hasKey(name));
        newFonts.replaceKey(name, embed(pdf, *face, glyphs, name));
        names[face] = name;
    }
    newResources.replaceKey("/Font", newFonts);
    page.getObjectHandle().replaceKey("/Resources", newResources);

    std::string out; const std::string restore = source.fontResource + " " + number(source.fontSizeOperand) + " Tf ";
    bool switched = false;
    for (const auto& segment : segments) {
        if (segment.face) {
            std::string bytes;
            for (auto cp : segment.chars) { auto gid = segment.face->glyph(cp); bytes += static_cast<char>(gid >> 8 & 0xFF); bytes += static_cast<char>(gid & 0xFF); }
            out += names[segment.face.get()] + " " + number(source.fontSizeOperand) + " Tf " + Obj::newString(bytes).unparse() + " Tj ";
            switched = true;
        } else {
            if (switched) { out += restore; switched = false; }
            out += Obj::newString(segment.bytes).unparse() + " Tj ";
        }
    }
    if (switched) out += restore;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
}
