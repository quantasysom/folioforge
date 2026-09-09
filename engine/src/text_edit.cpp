#include "text_edit.h"
#include <qpdf/QPDFObjectHandle.hh>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <vector>

namespace pdfengine::textedit {
namespace {
using Obj = QPDFObjectHandle;
// Standard PDF font advance metrics, in 1/1000-em units, ASCII 32..126.
constexpr std::array<int, 95> helvetica = {
278,278,355,556,556,889,667,191,333,333,389,584,278,333,278,278,556,556,556,556,556,556,556,556,556,556,278,278,584,584,584,556,1015,667,667,722,722,667,611,778,722,278,500,667,556,833,722,778,667,778,722,667,611,722,667,944,667,667,611,278,278,278,469,556,333,556,556,500,556,556,278,556,556,222,222,500,222,833,556,556,556,556,333,500,278,556,500,722,500,500,500,334,260,334,584};
constexpr std::array<int, 95> helveticaBold = {
278,333,474,556,556,889,722,238,333,333,389,584,278,333,278,278,556,556,556,556,556,556,556,556,556,556,333,333,584,584,584,611,975,722,722,722,722,667,611,778,722,278,556,722,611,833,722,778,667,778,722,667,611,722,667,944,667,667,611,333,278,333,584,556,333,556,611,556,611,556,333,611,611,278,278,556,278,889,611,611,611,611,389,556,333,611,556,778,556,556,500,389,280,389,584};
bool ascii(const std::string& text, bool standard) {
    return std::all_of(text.begin(), text.end(), [standard](unsigned char c) {
        // StandardEncoding maps these two ASCII positions to typographic quotes.
        return c >= 32 && c <= 126 && (!standard || (c != 39 && c != 96));
    });
}
double width(const std::string& text, const std::string& font) {
    double result = 0;
    for (unsigned char c : text) {
        if (font.starts_with("Courier")) result += 600;
        else result += font.find("Bold") != std::string::npos ? helveticaBold[c - 32] : helvetica[c - 32];
    }
    return result;
}
struct Matrix {
    double a{1}, b{}, c{}, d{1}, e{}, f{};
    Matrix then(const Matrix& m) const { // m(this(point))
        return {m.a*a+m.c*b, m.b*a+m.d*b, m.a*c+m.c*d, m.b*c+m.d*d, m.a*e+m.c*f+m.e, m.b*e+m.d*f+m.f};
    }
    bool horizontal() const { return a > 0 && d > 0 && std::abs(b) < 1e-8 && std::abs(c) < 1e-8; }
};
struct Graphics {
    Matrix ctm;
    std::string font;
    double size{}, charSpace{}, wordSpace{}, hscale{100}, leading{}, rise{}, mode{};
    bool safe{true}, standard{};
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
            gs.font.clear();
            if (operands.size() == 2 && operands[0].value.isName() && operands[1].value.isNumber()) {
                gs.size = operands[1].value.getNumericValue();
                auto font = fonts.getKeyIfDict(operands[0].value.getName());
                auto encoding = font.getKeyIfDict("/Encoding");
                gs.standard = encoding.isNull() || encoding.isNameAndEquals("/StandardEncoding");
                auto base = font.getKeyIfDict("/BaseFont");
                if (font.getKeyIfDict("/Subtype").isNameAndEquals("/Type1") && base.isName() &&
                    (gs.standard || encoding.isNameAndEquals("/WinAnsiEncoding")) &&
                    font.getKeyIfDict("/FontDescriptor").isNull() && font.getKeyIfDict("/ToUnicode").isNull() && font.getKeyIfDict("/Widths").isNull()) {
                    auto name = base.getName().substr(1);
                    if (name == "Helvetica" || name == "Helvetica-Bold" || name == "Helvetica-Oblique" || name == "Helvetica-BoldOblique" ||
                        name == "Courier" || name == "Courier-Bold" || name == "Courier-Oblique" || name == "Courier-BoldOblique") gs.font = name;
                }
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
        bool supported = !gs.font.empty() && gs.size > 0 && gs.size <= 500;
        bool beginsWithString = !parts.empty() && parts.front().isString();
        std::string text;
        double advance = 0;
        for (auto part : parts) {
            if (part.isString()) {
                auto bytes = part.getStringValue();
                if (!ascii(bytes, gs.standard) || bytes.size() + text.size() > 4096) supported = false;
                if (supported) advance += width(bytes, gs.font);
                text += bytes;
            } else if (part.isNumber() && std::isfinite(part.getNumericValue()) && std::abs(part.getNumericValue()) < 1e7) advance -= part.getNumericValue();
            else supported = false;
        }
        if (!supported) { positionKnown = false; return; }
        // Nonzero spacing and glyph stretch are not qualified for replacement.
        bool spacing = gs.charSpace == 0 && gs.wordSpace == 0 && gs.hscale == 100 && gs.rise == 0;
        Matrix combined = tm.then(gs.ctm);
        double renderedSize = gs.size * combined.d;
        double renderedWidth = advance * gs.size / 1000.0 * combined.a;
        if (positionKnown && gs.safe && spacing && gs.mode == 0 && beginsWithString && !text.empty() && advance > 0 &&
            tm.horizontal() && gs.ctm.horizontal() && std::abs(combined.a - combined.d) < 1e-6 &&
            combined.e >= 0 && combined.f - renderedSize*0.3 >= 0 && combined.e + renderedWidth <= pageWidth && combined.f + renderedSize <= pageHeight) {
            TextRun run{operands.front().offset, page, revision, text, gs.font, combined.e, combined.f, renderedWidth, renderedSize};
            runs.push_back({std::move(run), operands.front().offset, end - operands.front().offset, advance, gs.standard});
        }
        if (spacing) { double tx = advance * gs.size / 1000.0; tm.e += tx*tm.a; tm.f += tx*tm.b; }
        else positionKnown = false;
    }
};
}
Inventory inspect(QPDFPageObjectHelper page, PageId id, RevisionId revision) {
    Inventory result;
    result.explanation = "Only horizontal printable-ASCII runs in standard Helvetica/Courier fonts are editable. Custom fonts/encodings, text clipping, shaped scripts, marked content, and rotated/cropped pages are not yet supported.";
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
std::string replacement(const SourceRun& source, const std::string& text) {
    if (text.size() > 4096 || !ascii(text, source.standardEncoding))
        throw Error(ErrorCode::Unsupported, "This font run supports printable ASCII only. With StandardEncoding, straight apostrophes/backticks are also unavailable.");
    double advance = width(text, source.run.font);
    if (advance > source.advanceUnits + 0.01) throw Error(ErrorCode::TextOverflow, "The replacement is wider than the original text slot. Shorten the text; automatic shrinking and reflow are not enabled.");
    std::ostringstream adjustment;
    adjustment.imbue(std::locale::classic()); adjustment << std::fixed << std::setprecision(6) << advance - source.advanceUnits;
    // TJ compensates for the changed glyph advance, preserving subsequent text positions.
    return "[" + Obj::newString(text).unparse() + " " + adjustment.str() + "] TJ";
}
}
