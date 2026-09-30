#include "annotation.h"
#include "font_codec.h"
#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>

namespace pdfengine::annot {
namespace {
using Obj = QPDFObjectHandle;
constexpr std::size_t maxStrokes = 1000, maxPoints = 100000, maxContents = 20000;

std::string num(double v) {
    std::ostringstream out; out.imbue(std::locale::classic()); out.setf(std::ios::fixed); out.precision(3); out << v;
    auto text = out.str();
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return text == "-0" ? "0" : text;
}
Obj numbers(std::initializer_list<double> values) {
    std::vector<Obj> items;
    for (double v : values) items.push_back(Obj::newReal(v, 3));
    return Obj::newArray(items);
}
Obj colorArray(const std::array<double, 3>& c) { return numbers({c[0], c[1], c[2]}); }
std::string rg(const std::array<double, 3>& c, bool stroke) { return num(c[0]) + " " + num(c[1]) + " " + num(c[2]) + (stroke ? " RG " : " rg "); }
const char* subtype(AnnotationKind kind) {
    switch (kind) {
    case AnnotationKind::Highlight: return "/Highlight"; case AnnotationKind::Underline: return "/Underline";
    case AnnotationKind::StrikeOut: return "/StrikeOut"; case AnnotationKind::Note: return "/Text";
    case AnnotationKind::FreeText: return "/FreeText"; case AnnotationKind::Ink: return "/Ink";
    case AnnotationKind::Rectangle: return "/Square"; case AnnotationKind::Ellipse: return "/Circle";
    default: return nullptr;
    }
}
AnnotationKind kindOf(const std::string& s) {
    static const std::pair<const char*, AnnotationKind> table[] = {
        {"/Highlight", AnnotationKind::Highlight}, {"/Underline", AnnotationKind::Underline}, {"/StrikeOut", AnnotationKind::StrikeOut},
        {"/Text", AnnotationKind::Note}, {"/FreeText", AnnotationKind::FreeText}, {"/Ink", AnnotationKind::Ink},
        {"/Square", AnnotationKind::Rectangle}, {"/Circle", AnnotationKind::Ellipse}};
    for (auto& [name, kind] : table) if (s == name) return kind;
    return AnnotationKind::Other;
}
void requireFinite(std::initializer_list<double> values) {
    for (double v : values) if (!std::isfinite(v) || std::abs(v) > 1e7) throw Error(ErrorCode::InvalidSelection, "The annotation geometry is not valid.");
}
std::vector<std::string> wrap(const std::string& text, double fontSize, double maxWidth, const textedit::GlyphFont& font) {
    std::vector<std::string> lines;
    std::istringstream paragraphs(text);
    std::string paragraph;
    auto widthOf = [&](const std::string& s) { double w = 0; for (unsigned char c : s) w += font.width(c); return w * fontSize / 1000; };
    bool any = false;
    while (std::getline(paragraphs, paragraph)) {
        any = true;
        std::string line, word;
        auto flush = [&] { lines.push_back(line); line.clear(); };
        std::istringstream words(paragraph);
        while (words >> word) {
            std::string candidate = line.empty() ? word : line + " " + word;
            if (widthOf(candidate) <= maxWidth || line.empty()) {
                line = candidate;
                while (widthOf(line) > maxWidth && line.size() > 1) { // Break a single overlong word.
                    std::size_t cut = line.size() - 1;
                    while (cut > 1 && widthOf(line.substr(0, cut)) > maxWidth) --cut;
                    lines.push_back(line.substr(0, cut)); line = line.substr(cut);
                }
            } else { flush(); line = word; }
        }
        flush();
    }
    if (!any) lines.push_back({});
    return lines;
}
std::string hex(const std::string& s) {
    static const char* digits = "0123456789ABCDEF"; std::string out = "<";
    for (unsigned char c : s) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out + ">";
}
std::string ellipse(double x0, double y0, double x1, double y1) {
    const double k = 0.5522847498, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    std::string s = num(x1) + " " + num(cy) + " m ";
    auto curve = [&](double a, double b, double c, double d, double e, double f) { s += num(a) + " " + num(b) + " " + num(c) + " " + num(d) + " " + num(e) + " " + num(f) + " c "; };
    curve(x1, cy + k * ry, cx + k * rx, y1, cx, y1); curve(cx - k * rx, y1, x0, cy + k * ry, x0, cy);
    curve(x0, cy - k * ry, cx - k * rx, y0, cx, y0); curve(cx + k * rx, y0, x1, cy - k * ry, x1, cy);
    return s + "h ";
}
}

Obj create(QPDF& pdf, const AddAnnotation& request, const std::string& name) {
    const char* type = subtype(request.kind);
    if (!type) throw Error(ErrorCode::Unsupported, "This annotation type cannot be created.");
    requireFinite({request.x0, request.y0, request.x1, request.y1, request.lineWidth, request.fontSize});
    for (double c : request.color) if (!(c >= 0 && c <= 1)) throw Error(ErrorCode::InvalidSelection, "Annotation colors must be between 0 and 1.");
    if (request.contents.size() > maxContents) throw Error(ErrorCode::ResourceLimit, "Annotation text is too long.");
    const double lw = std::clamp(request.lineWidth, 0.25, 40.0);
    double x0 = std::min(request.x0, request.x1), x1 = std::max(request.x0, request.x1);
    double y0 = std::min(request.y0, request.y1), y1 = std::max(request.y0, request.y1);
    std::string content;
    auto resources = Obj::newDictionary();
    std::vector<Obj> extra; // Extra dictionary keys applied after construction.
    auto annotation = Obj::newDictionary();
    switch (request.kind) {
    case AnnotationKind::Highlight: case AnnotationKind::Underline: case AnnotationKind::StrikeOut: {
        if (x1 - x0 < 1 || y1 - y0 < 1) throw Error(ErrorCode::InvalidSelection, "Select an area at least one point in size.");
        annotation.replaceKey("/QuadPoints", numbers({x0, y1, x1, y1, x0, y0, x1, y0}));
        if (request.kind == AnnotationKind::Highlight) {
            auto states = Obj::newDictionary(), gs = Obj::newDictionary();
            gs.replaceKey("/Type", Obj::newName("/ExtGState")); gs.replaceKey("/BM", Obj::newName("/Multiply"));
            states.replaceKey("/GS", gs); resources.replaceKey("/ExtGState", states);
            content = "q /GS gs " + rg(request.color, false) + num(x0) + " " + num(y0) + " " + num(x1 - x0) + " " + num(y1 - y0) + " re f Q";
        } else {
            double y = request.kind == AnnotationKind::Underline ? y0 + 1.5 : (y0 + y1) / 2;
            content = "q " + rg(request.color, true) + "1 w " + num(x0) + " " + num(y) + " m " + num(x1) + " " + num(y) + " l S Q";
        }
        break;
    }
    case AnnotationKind::Rectangle: case AnnotationKind::Ellipse: {
        if (x1 - x0 < 2 || y1 - y0 < 2) throw Error(ErrorCode::InvalidSelection, "Draw a shape at least two points in size.");
        double h = lw / 2;
        content = "q " + rg(request.color, true) + num(lw) + " w ";
        if (request.kind == AnnotationKind::Rectangle) content += num(x0 + h) + " " + num(y0 + h) + " " + num(x1 - x0 - lw) + " " + num(y1 - y0 - lw) + " re S Q";
        else content += ellipse(x0 + h, y0 + h, x1 - h, y1 - h) + "S Q";
        auto bs = Obj::newDictionary(); bs.replaceKey("/W", Obj::newReal(lw, 3)); annotation.replaceKey("/BS", bs);
        break;
    }
    case AnnotationKind::Ink: {
        if (request.strokes.empty() || request.strokes.size() > maxStrokes) throw Error(ErrorCode::InvalidSelection, "Draw at least one stroke.");
        std::size_t total = 0; double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
        std::vector<Obj> list;
        content = "q " + rg(request.color, true) + num(lw) + " w 1 J 1 j ";
        for (const auto& stroke : request.strokes) {
            if (stroke.empty()) continue;
            total += stroke.size(); if (total > maxPoints) throw Error(ErrorCode::ResourceLimit, "The drawing has too many points.");
            std::vector<Obj> points;
            for (std::size_t i = 0; i < stroke.size(); ++i) {
                requireFinite({stroke[i].x, stroke[i].y});
                minX = std::min(minX, stroke[i].x); maxX = std::max(maxX, stroke[i].x); minY = std::min(minY, stroke[i].y); maxY = std::max(maxY, stroke[i].y);
                points.push_back(Obj::newReal(stroke[i].x, 3)); points.push_back(Obj::newReal(stroke[i].y, 3));
                content += num(stroke[i].x) + " " + num(stroke[i].y) + (i == 0 ? " m " : " l ");
            }
            if (stroke.size() == 1) content += num(stroke[0].x) + " " + num(stroke[0].y) + " l ";
            content += "S ";
            list.push_back(Obj::newArray(points));
        }
        if (list.empty()) throw Error(ErrorCode::InvalidSelection, "Draw at least one stroke.");
        content += "Q";
        annotation.replaceKey("/InkList", Obj::newArray(list));
        auto bs = Obj::newDictionary(); bs.replaceKey("/W", Obj::newReal(lw, 3)); annotation.replaceKey("/BS", bs);
        double pad = lw / 2 + 1; x0 = minX - pad; y0 = minY - pad; x1 = maxX + pad; y1 = maxY + pad;
        break;
    }
    case AnnotationKind::Note: {
        x0 = request.x0; y0 = request.y0; x1 = x0 + 20; y1 = y0 + 20; // The note is anchored at (x0, y0).
        content = "q 1 0.87 0.2 rg 0.35 0.25 0 RG 1 w " + num(x0 + 0.5) + " " + num(y0 + 0.5) + " 19 19 re B 0.35 0.25 0 RG 1 w ";
        for (double dy : {5.5, 9.5, 13.5}) content += num(x0 + 4) + " " + num(y0 + dy) + " m " + num(x0 + 16) + " " + num(y0 + dy) + " l S ";
        content += "Q";
        annotation.replaceKey("/Name", Obj::newName("/Note"));
        annotation.replaceKey("/Open", Obj::newBool(false));
        break;
    }
    case AnnotationKind::FreeText: {
        const double fs = std::clamp(request.fontSize, 4.0, 200.0);
        if (request.contents.empty()) throw Error(ErrorCode::InvalidSelection, "Enter the text to place.");
        for (unsigned char c : request.contents) if ((c < 32 || c > 126) && c != '\n' && c != '\r') throw Error(ErrorCode::Unsupported, "Text annotations currently support printable ASCII text.");
        auto font = Obj::newDictionary();
        font.replaceKey("/Type", Obj::newName("/Font")); font.replaceKey("/Subtype", Obj::newName("/Type1"));
        font.replaceKey("/BaseFont", Obj::newName("/Helvetica")); font.replaceKey("/Encoding", Obj::newName("/WinAnsiEncoding"));
        auto metrics = textedit::loadFont(font);
        if (!metrics) throw Error(ErrorCode::InvalidDocument, "Helvetica metrics are unavailable.");
        if (x1 - x0 < fs) x1 = x0 + std::max(fs * 8, 60.0);
        std::string plain; for (char c : request.contents) if (c != '\r') plain += c;
        auto lines = wrap(plain, fs, x1 - x0 - 4, *metrics);
        if (lines.size() > 500) throw Error(ErrorCode::ResourceLimit, "Annotation text is too long.");
        const double leading = fs * 1.2;
        y1 = std::max(y1, y0 + lines.size() * leading + 4);
        content = "q " + num(x0) + " " + num(y0) + " " + num(x1 - x0) + " " + num(y1 - y0) + " re W n BT " + rg(request.color, false) + "/Helv " + num(fs) + " Tf " + num(leading) + " TL " +
                  num(x0 + 2) + " " + num(y1 - 2 - fs) + " Td ";
        for (std::size_t i = 0; i < lines.size(); ++i) content += hex(lines[i]) + " Tj " + (i + 1 < lines.size() ? "T* " : "");
        content += "ET Q";
        auto fonts = Obj::newDictionary(); fonts.replaceKey("/Helv", font); resources.replaceKey("/Font", fonts);
        annotation.replaceKey("/DA", Obj::newString("/Helv " + num(fs) + " Tf " + rg(request.color, false)));
        annotation.replaceKey("/Q", Obj::newInteger(0));
        break;
    }
    default: break;
    }
    auto form = pdf.newStream(content);
    auto dict = form.getDict();
    dict.replaceKey("/Type", Obj::newName("/XObject")); dict.replaceKey("/Subtype", Obj::newName("/Form"));
    dict.replaceKey("/BBox", numbers({x0, y0, x1, y1})); dict.replaceKey("/Resources", resources);
    auto appearance = Obj::newDictionary(); appearance.replaceKey("/N", form);
    annotation.replaceKey("/Type", Obj::newName("/Annot"));
    annotation.replaceKey("/Subtype", Obj::newName(type));
    annotation.replaceKey("/Rect", numbers({x0, y0, x1, y1}));
    annotation.replaceKey("/F", Obj::newInteger(4));
    annotation.replaceKey("/C", colorArray(request.color));
    annotation.replaceKey("/NM", Obj::newUnicodeString(name));
    annotation.replaceKey("/T", Obj::newUnicodeString("FolioForge"));
    if (!request.contents.empty()) annotation.replaceKey("/Contents", Obj::newUnicodeString(request.contents));
    annotation.replaceKey("/AP", appearance);
    return pdf.makeIndirectObject(annotation);
}

bool preservable(Obj annotation) {
    if (!annotation.isDictionary()) return false;
    auto sub = annotation.getKey("/Subtype");
    if (!sub.isName()) return false;
    static const char* allowed[] = {"/Highlight", "/Underline", "/StrikeOut", "/Squiggly", "/Text", "/FreeText", "/Ink", "/Square", "/Circle", "/Line", "/Polygon", "/PolyLine", "/Stamp", "/Caret", "/Popup", "/FileAttachment"};
    bool known = false;
    for (auto name : allowed) known |= sub.getName() == name;
    return known && !annotation.hasKey("/A") && !annotation.hasKey("/AA") && !annotation.hasKey("/Dest");
}

std::vector<Annotation> list(QPDFPageObjectHelper page) {
    std::vector<Annotation> out;
    auto annots = page.getObjectHandle().getKey("/Annots");
    if (!annots.isArray()) return out;
    for (int i = 0; i < annots.getArrayNItems(); ++i) {
        auto item = annots.getArrayItem(i);
        if (!item.isDictionary() || !item.getKey("/Subtype").isName()) continue;
        Annotation a; a.index = static_cast<std::uint32_t>(i); a.kind = kindOf(item.getKey("/Subtype").getName());
        if (a.kind == AnnotationKind::Other) continue;
        auto rect = item.getKey("/Rect");
        if (rect.isArray() && rect.getArrayNItems() == 4) {
            double v[4]{};
            for (int k = 0; k < 4; ++k) if (rect.getArrayItem(k).isNumber()) v[k] = rect.getArrayItem(k).getNumericValue();
            a.x0 = std::min(v[0], v[2]); a.x1 = std::max(v[0], v[2]); a.y0 = std::min(v[1], v[3]); a.y1 = std::max(v[1], v[3]);
        }
        auto color = item.getKey("/C");
        if (color.isArray() && color.getArrayNItems() == 3)
            for (int k = 0; k < 3; ++k) if (color.getArrayItem(k).isNumber()) a.color[k] = color.getArrayItem(k).getNumericValue();
        auto contents = item.getKey("/Contents");
        if (contents.isString()) a.contents = contents.getUTF8Value();
        auto name = item.getKey("/NM");
        a.removable = name.isString() && name.getUTF8Value().rfind(namePrefix, 0) == 0;
        out.push_back(std::move(a));
    }
    return out;
}
}
