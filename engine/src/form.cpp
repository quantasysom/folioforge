#include "form.h"
#include "annotation.h"
#include "font_codec.h"
#include <algorithm>
#include <cmath>
#include <locale>
#include <set>
#include <sstream>

namespace pdfengine::form {
namespace {
using Obj = QPDFObjectHandle;
constexpr int maxDepth = 32;
constexpr std::size_t maxFields = 200000, maxText = 20000;
enum Flags : long long { ReadOnly = 1, Multiline = 1 << 12, Password = 1 << 13, Radio = 1 << 15, Pushbutton = 1 << 16, Combo = 1 << 17,
                         Edit = 1 << 18, MultiSelect = 1 << 21, Comb = 1 << 24, RichText = 1 << 25, FileSelect = 1 << 20 };

std::string num(double v) {
    std::ostringstream out; out.imbue(std::locale::classic()); out.setf(std::ios::fixed); out.precision(3); out << v;
    auto text = out.str();
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return text == "-0" ? "0" : text;
}
Obj numbers(std::initializer_list<double> values) {
    std::vector<Obj> items; for (double v : values) items.push_back(Obj::newReal(v, 3)); return Obj::newArray(items);
}
Obj sub(Obj node, const char* key) { return node.isDictionary() ? node.getKey(key) : Obj::newNull(); }
Obj parentOf(Obj node) { auto p = node.getKey("/Parent"); return p.isDictionary() ? p : Obj::newNull(); }
Obj inherited(Obj node, const char* key) {
    for (int depth = 0; depth < maxDepth && node.isDictionary(); ++depth) {
        auto value = node.getKey(key);
        if (!value.isNull()) return value;
        node = parentOf(node);
    }
    return Obj::newNull();
}
long long flagsOf(Obj field) { auto f = inherited(field, "/Ff"); return f.isInteger() ? f.getIntValue() : 0; }
// The field a widget belongs to: the nearest node that carries a name.
Obj fieldOf(Obj widget) {
    Obj node = widget;
    for (int depth = 0; depth < maxDepth; ++depth) {
        if (node.hasKey("/T") || !node.getKey("/Parent").isDictionary()) return node;
        node = node.getKey("/Parent");
    }
    return widget;
}
std::string nameOf(Obj field) {
    std::vector<std::string> parts;
    for (int depth = 0; depth < maxDepth && field.isDictionary(); ++depth, field = parentOf(field)) {
        auto t = field.getKey("/T");
        if (t.isString()) parts.push_back(t.getUTF8Value());
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += (out.empty() ? "" : ".") + *it;
    return out;
}
void collectWidgets(Obj field, std::vector<Obj>& out, int depth = 0) {
    auto kids = field.getKey("/Kids");
    if (!kids.isArray() || depth > maxDepth) { out.push_back(field); return; }
    for (int i = 0; i < kids.getArrayNItems(); ++i) {
        auto kid = kids.getArrayItem(i);
        if (!kid.isDictionary()) continue;
        if (kid.hasKey("/T") && kid.hasKey("/Kids")) collectWidgets(kid, out, depth + 1);
        else out.push_back(kid);
    }
}
bool isWidget(Obj annotation) { return annotation.isDictionary() && annotation.getKey("/Subtype").isName() && annotation.getKey("/Subtype").getName() == "/Widget"; }
std::string stringValue(Obj v) {
    if (v.isString()) return v.getUTF8Value();
    if (v.isName()) return v.getName().substr(1);
    if (v.isStream()) { auto d = v.getStreamData(); return std::string(reinterpret_cast<const char*>(d->getBuffer()), d->getSize()); }
    if (v.isArray() && v.getArrayNItems() > 0) return stringValue(v.getArrayItem(0));
    return {};
}
// Normal-appearance state names other than /Off.
std::string onState(Obj widget) {
    auto normal = sub(sub(widget, "/AP"), "/N");
    if (normal.isDictionary())
        for (auto& key : normal.getKeys()) if (key != "/Off") return key.substr(1);
    return {};
}
struct Options { std::vector<std::string> display, value; };
Options optionsOf(Obj field) {
    Options out; auto opt = inherited(field, "/Opt");
    if (!opt.isArray()) return out;
    for (int i = 0; i < opt.getArrayNItems() && i < 5000; ++i) {
        auto item = opt.getArrayItem(i);
        if (item.isArray() && item.getArrayNItems() >= 2) { out.value.push_back(stringValue(item.getArrayItem(0))); out.display.push_back(stringValue(item.getArrayItem(1))); }
        else { out.value.push_back(stringValue(item)); out.display.push_back(out.value.back()); }
    }
    return out;
}
bool rect(Obj widget, double& x0, double& y0, double& x1, double& y1) {
    auto r = widget.getKey("/Rect");
    if (!r.isArray() || r.getArrayNItems() != 4) return false;
    double v[4];
    for (int i = 0; i < 4; ++i) { auto n = r.getArrayItem(i); if (!n.isNumber()) return false; v[i] = n.getNumericValue(); }
    x0 = std::min(v[0], v[2]); x1 = std::max(v[0], v[2]); y0 = std::min(v[1], v[3]); y1 = std::max(v[1], v[3]);
    return std::isfinite(x0 + x1 + y0 + y1);
}
int rotationOf(Obj widget) { auto r = sub(sub(widget, "/MK"), "/R"); return r.isInteger() ? r.getIntValueAsInt() : 0; }
std::string colorOps(Obj array, bool stroke) {
    if (!array.isArray()) return {};
    std::vector<double> c;
    for (int i = 0; i < array.getArrayNItems(); ++i) { auto n = array.getArrayItem(i); if (!n.isNumber()) return {}; c.push_back(n.getNumericValue()); }
    std::string out; for (double v : c) out += num(std::clamp(v, 0.0, 1.0)) + " ";
    if (c.size() == 1) return out + (stroke ? "G " : "g ");
    if (c.size() == 3) return out + (stroke ? "RG " : "rg ");
    if (c.size() == 4) return out + (stroke ? "K " : "k ");
    return {};
}
std::vector<std::string> tokens(const std::string& s) { std::istringstream in(s); std::vector<std::string> out; std::string t; while (in >> t) out.push_back(t); return out; }

struct Appearance { std::string fontName{"/Helv"}; double size{}; std::string color{"0 g "}; };
Appearance parseDA(Obj field, Obj acroForm) {
    Appearance a;
    auto da = inherited(field, "/DA");
    if (!da.isString()) da = acroForm.isDictionary() ? acroForm.getKey("/DA") : Obj::newNull();
    if (!da.isString()) return a;
    auto t = tokens(da.getUTF8Value());
    for (std::size_t i = 2; i < t.size(); ++i) {
        if (t[i] != "Tf") continue;
        a.fontName = t[i - 2]; try { a.size = std::stod(t[i - 1]); } catch (...) { a.size = 0; }
        std::string rest;
        for (std::size_t k = i + 1; k < t.size(); ++k) rest += t[k] + " ";
        auto r = tokens(rest);
        if (!r.empty() && (r.back() == "g" || r.back() == "rg" || r.back() == "k")) a.color = rest;
        break;
    }
    if (a.fontName.empty() || a.fontName[0] != '/') a.fontName = "/Helv";
    return a;
}
Obj fontFor(QPDF& pdf, Obj acroForm, const std::string& name) {
    if (acroForm.isDictionary()) {
        auto font = sub(sub(sub(acroForm, "/DR"), "/Font"), name.c_str());
        if (font.isDictionary()) return font;
    }
    auto font = Obj::newDictionary();
    font.replaceKey("/Type", Obj::newName("/Font")); font.replaceKey("/Subtype", Obj::newName("/Type1"));
    font.replaceKey("/BaseFont", Obj::newName("/Helvetica")); font.replaceKey("/Encoding", Obj::newName("/WinAnsiEncoding"));
    return pdf.makeIndirectObject(font);
}
std::string hexOf(const std::string& s) {
    static const char* d = "0123456789ABCDEF"; std::string out = "<";
    for (unsigned char c : s) { out += d[c >> 4]; out += d[c & 15]; }
    return out + ">";
}
// Encodes UTF-8 with a single-byte font; throws when a character has no glyph.
std::string encode(const textedit::GlyphFont& font, const std::string& text) {
    if (!textedit::validUtf8(text)) throw Error(ErrorCode::InvalidSelection, "The text is not valid UTF-8.");
    std::string out;
    for (std::size_t at = 0; at < text.size();) {
        std::uint32_t code{}; std::size_t used{};
        if (text[at] == '\n') { out += '\n'; ++at; continue; }
        if (text[at] == '\r') { ++at; continue; }
        if (!font.encodeAt(text, at, code, used) || code > 255)
            throw Error(ErrorCode::Unsupported, "The field's font cannot display one of these characters.");
        out += static_cast<char>(code); at += used;
    }
    return out;
}
double widthOf(const textedit::GlyphFont& font, const std::string& bytes, double size) {
    double w = 0; for (unsigned char c : bytes) w += font.width(c); return w * size / 1000;
}
std::string fieldBackground(Obj widget, double w, double h, double& border) {
    std::string out; auto mk = widget.getKey("/MK");
    border = 0;
    if (!mk.isDictionary()) return out;
    out += colorOps(mk.getKey("/BG"), false).empty() ? "" : "q " + colorOps(mk.getKey("/BG"), false) + "0 0 " + num(w) + " " + num(h) + " re f Q ";
    auto stroke = colorOps(mk.getKey("/BC"), true);
    if (!stroke.empty()) {
        auto bs = sub(sub(widget, "/BS"), "/W");
        border = bs.isNumber() ? std::clamp(bs.getNumericValue(), 0.0, 10.0) : 1;
        if (border > 0) out += "q " + stroke + num(border) + " w " + num(border / 2) + " " + num(border / 2) + " " + num(w - border) + " " + num(h - border) + " re S Q ";
    }
    return out;
}
Obj makeForm(QPDF& pdf, const std::string& content, double w, double h, Obj resources) {
    auto stream = pdf.newStream(content); auto dict = stream.getDict();
    dict.replaceKey("/Type", Obj::newName("/XObject")); dict.replaceKey("/Subtype", Obj::newName("/Form"));
    dict.replaceKey("/BBox", numbers({0, 0, w, h})); dict.replaceKey("/Resources", resources);
    return stream;
}
void textAppearance(QPDF& pdf, Obj acroForm, Obj field, Obj widget, const std::string& shown, bool listbox, const Options& options, const std::string& selected) {
    double x0, y0, x1, y1;
    if (!rect(widget, x0, y0, x1, y1)) throw Error(ErrorCode::InvalidDocument, "The field has no valid rectangle.");
    const double w = x1 - x0, h = y1 - y0;
    if (w < 2 || h < 2) throw Error(ErrorCode::InvalidDocument, "The field is too small.");
    const long long flags = flagsOf(field);
    auto da = parseDA(field, acroForm);
    auto fontObj = fontFor(pdf, acroForm, da.fontName);
    auto font = textedit::loadFont(fontObj);
    if (!font || font->twoByte) throw Error(ErrorCode::Unsupported, "The field uses a font that cannot be edited.");
    double border; std::string content = fieldBackground(widget, w, h, border);
    const double pad = border + 2;
    const bool multiline = (flags & Multiline) && !listbox;
    double size = da.size;
    if (size <= 0) size = multiline || listbox ? 12 : std::clamp((h - 2 * border) * 0.75, 4.0, 12.0);
    std::string body = encode(*font, shown);
    auto q = inherited(field, "/Q"); const int align = q.isInteger() ? q.getIntValueAsInt() : 0;
    std::string text = "/Tx BMC q " + num(border) + " " + num(border) + " " + num(w - 2 * border) + " " + num(h - 2 * border) + " re W n ";
    if (listbox) {
        const double row = size * 1.2; double top = h - border - 1; std::string rows;
        for (std::size_t i = 0; i < options.display.size() && top > 0; ++i, top -= row) {
            const bool hit = options.value[i] == selected;
            if (hit) rows += "q 0.6 0.75 0.85 rg " + num(border) + " " + num(top - row) + " " + num(w - 2 * border) + " " + num(row) + " re f Q ";
            std::string line = encode(*font, options.display[i]);
            rows += "BT " + da.color + da.fontName + " " + num(size) + " Tf " + num(pad) + " " + num(top - row + size * 0.25) + " Td " + hexOf(line) + " Tj ET ";
        }
        text += rows;
    } else if (!body.empty() && multiline) {
        std::string plain; for (char c : body) if (c != '\r') plain += c;
        auto lines = annot::wrapText(plain, size, w - 2 * pad, *font);
        if (lines.size() > 2000) throw Error(ErrorCode::ResourceLimit, "The field text is too long.");
        const double leading = size * 1.15;
        text += "BT " + da.color + da.fontName + " " + num(size) + " Tf " + num(leading) + " TL " + num(pad) + " " + num(h - pad - size) + " Td ";
        for (std::size_t i = 0; i < lines.size(); ++i) text += hexOf(lines[i]) + " Tj " + (i + 1 < lines.size() ? "T* " : "");
        text += "ET ";
    } else if (!body.empty()) {
        for (char& c : body) if (c == '\n') c = ' ';
        const auto maxLen = inherited(field, "/MaxLen");
        if ((flags & Comb) && maxLen.isInteger() && maxLen.getIntValue() > 0 && !(flags & Password)) {
            const double cell = (w - 2 * border) / maxLen.getIntValue();
            text += "BT " + da.color + da.fontName + " " + num(size) + " Tf ";
            double previous = 0;
            for (std::size_t i = 0; i < body.size(); ++i) {
                const double cw = widthOf(*font, std::string(1, body[i]), size);
                const double x = border + cell * i + (cell - cw) / 2;
                text += num(x - previous) + " " + (i == 0 ? num((h - 0.72 * size) / 2) : "0") + " Td " + hexOf(std::string(1, body[i])) + " Tj ";
                previous = x;
            }
            text += "ET ";
        } else {
            double tw = widthOf(*font, body, size);
            if (da.size <= 0 && tw > w - 2 * pad && tw > 0) { size = std::max(4.0, size * (w - 2 * pad) / tw); tw = widthOf(*font, body, size); }
            const double x = align == 1 ? (w - tw) / 2 : align == 2 ? w - pad - tw : pad;
            text += "BT " + da.color + da.fontName + " " + num(size) + " Tf " + num(x) + " " + num((h - 0.72 * size) / 2) + " Td " + hexOf(body) + " Tj ET ";
        }
    }
    text += "Q EMC";
    auto fonts = Obj::newDictionary(); fonts.replaceKey(da.fontName, fontObj);
    auto resources = Obj::newDictionary(); resources.replaceKey("/Font", fonts);
    auto appearance = Obj::newDictionary(); appearance.replaceKey("/N", makeForm(pdf, content + text, w, h, resources));
    widget.replaceKey("/AP", appearance);
}
// Check marks and radio dots need an appearance when the file did not bring one.
void ensureButtonAppearance(QPDF& pdf, Obj widget, bool radio) {
    if (!onState(widget).empty()) return;
    double x0, y0, x1, y1;
    if (!rect(widget, x0, y0, x1, y1)) throw Error(ErrorCode::InvalidDocument, "The field has no valid rectangle.");
    const double w = x1 - x0, h = y1 - y0;
    double border; std::string back = fieldBackground(widget, w, h, border);
    std::string mark = radio ? "q 0 g " + num(w / 2 + w * 0.2) + " " + num(h / 2) + " m " : "q 0 0 0 RG " + num(std::max(1.0, w / 8)) + " w 1 J " + num(w * 0.2) + " " + num(h * 0.5) + " m " + num(w * 0.42) + " " + num(h * 0.25) + " l " + num(w * 0.8) + " " + num(h * 0.78) + " l S Q";
    if (radio) {
        const double cx = w / 2, cy = h / 2, r = std::min(w, h) * 0.25, k = 0.5522847498 * r;
        mark = "q 0 g " + num(cx + r) + " " + num(cy) + " m " + num(cx + r) + " " + num(cy + k) + " " + num(cx + k) + " " + num(cy + r) + " " + num(cx) + " " + num(cy + r) + " c " +
               num(cx - k) + " " + num(cy + r) + " " + num(cx - r) + " " + num(cy + k) + " " + num(cx - r) + " " + num(cy) + " c " +
               num(cx - r) + " " + num(cy - k) + " " + num(cx - k) + " " + num(cy - r) + " " + num(cx) + " " + num(cy - r) + " c " +
               num(cx + k) + " " + num(cy - r) + " " + num(cx + r) + " " + num(cy - k) + " " + num(cx + r) + " " + num(cy) + " c f Q";
    }
    auto normal = Obj::newDictionary();
    normal.replaceKey("/Yes", makeForm(pdf, back + mark, w, h, Obj::newDictionary()));
    normal.replaceKey("/Off", makeForm(pdf, back, w, h, Obj::newDictionary()));
    auto appearance = widget.getKey("/AP"); if (!appearance.isDictionary()) appearance = Obj::newDictionary();
    appearance.replaceKey("/N", normal); widget.replaceKey("/AP", appearance);
}
}

std::string restriction(Obj root) {
    auto af = root.getKey("/AcroForm");
    if (!af.isDictionary()) return af.isNull() ? "" : "The document's form definition is damaged.";
    if (af.hasKey("/XFA")) return "XFA dynamic forms are not supported. Read-only mode preserves the original.";
    auto flags = af.getKey("/SigFlags");
    if (flags.isInteger() && (flags.getIntValue() & 2)) return "This PDF is append-only because it is signed. Read-only mode preserves the signature.";
    std::vector<std::pair<Obj, int>> pending; std::set<QPDFObjGen> seen; std::size_t count = 0;
    auto fields = af.getKey("/Fields");
    if (fields.isArray()) for (int i = 0; i < fields.getArrayNItems(); ++i) pending.push_back({fields.getArrayItem(i), 0});
    while (!pending.empty()) {
        auto [node, depth] = pending.back(); pending.pop_back();
        if (!node.isDictionary()) continue;
        if (node.isIndirect() && !seen.insert(node.getObjGen()).second) continue;
        if (++count > maxFields || depth > maxDepth) return "This PDF's form is too large or deeply nested to edit safely.";
        auto type = inherited(node, "/FT");
        if (type.isName() && type.getName() == "/Sig" && node.getKey("/V").isDictionary()) return "This PDF contains a digital signature. Editing would invalidate it, so it is read-only.";
        auto kids = node.getKey("/Kids");
        if (kids.isArray()) for (int i = 0; i < kids.getArrayNItems(); ++i) pending.push_back({kids.getArrayItem(i), depth + 1});
    }
    return {};
}

bool hasWidgets(QPDFPageObjectHelper page) {
    auto annots = page.getObjectHandle().getKey("/Annots");
    if (!annots.isArray()) return false;
    for (int i = 0; i < annots.getArrayNItems(); ++i) if (isWidget(annots.getArrayItem(i))) return true;
    return false;
}

std::vector<FormField> list(QPDFPageObjectHelper page) {
    std::vector<FormField> out;
    auto annots = page.getObjectHandle().getKey("/Annots");
    if (!annots.isArray()) return out;
    for (int i = 0; i < annots.getArrayNItems(); ++i) {
        auto widget = annots.getArrayItem(i);
        if (!isWidget(widget)) continue;
        FormField f; f.widget = static_cast<std::uint32_t>(i);
        if (!rect(widget, f.x0, f.y0, f.x1, f.y1)) continue;
        auto field = fieldOf(widget);
        f.name = nameOf(field);
        auto type = inherited(field, "/FT"); const std::string ft = type.isName() ? type.getName() : "";
        const long long flags = flagsOf(field);
        f.readOnly = (flags & ReadOnly) || rotationOf(widget) != 0 || (widget.getKey("/F").isInteger() && (widget.getKey("/F").getIntValue() & 2));
        if (ft == "/Tx") {
            f.kind = FormFieldKind::Text; f.value = stringValue(inherited(field, "/V"));
            f.multiline = flags & Multiline; f.password = flags & Password;
            auto max = inherited(field, "/MaxLen"); f.maxLength = max.isInteger() ? std::max(0, max.getIntValueAsInt()) : 0;
            if (flags & (RichText | FileSelect)) f.readOnly = true;
        } else if (ft == "/Btn") {
            if (flags & Pushbutton) { f.kind = FormFieldKind::Button; f.readOnly = true; }
            else {
                f.kind = (flags & Radio) ? FormFieldKind::Radio : FormFieldKind::Checkbox;
                f.value = onState(widget);
                auto as = widget.getKey("/AS");
                f.checked = as.isName() && as.getName() != "/Off" && (f.value.empty() || as.getName() == "/" + f.value);
            }
        } else if (ft == "/Ch") {
            f.kind = FormFieldKind::Choice; f.value = stringValue(inherited(field, "/V"));
            auto opts = optionsOf(field); f.options = opts.display; f.optionValues = opts.value;
            f.combo = flags & Combo; f.editable = (flags & Combo) && (flags & Edit);
            if (flags & MultiSelect) f.readOnly = true;
        } else if (ft == "/Sig") { f.kind = FormFieldKind::Signature; f.readOnly = true; }
        else continue;
        out.push_back(std::move(f));
    }
    return out;
}

void set(QPDF& pdf, QPDFPageObjectHelper page, const SetFormValue& request) {
    auto annots = page.getObjectHandle().getKey("/Annots");
    if (!annots.isArray() || request.widget >= static_cast<std::uint32_t>(annots.getArrayNItems()))
        throw Error(ErrorCode::InvalidSelection, "The form field no longer exists.");
    auto widget = annots.getArrayItem(static_cast<int>(request.widget));
    if (!isWidget(widget)) throw Error(ErrorCode::InvalidSelection, "The form field no longer exists.");
    auto field = fieldOf(widget);
    auto acroForm = pdf.getRoot().getKey("/AcroForm");
    auto type = inherited(field, "/FT"); const std::string ft = type.isName() ? type.getName() : "";
    const long long flags = flagsOf(field);
    if ((flags & ReadOnly) || rotationOf(widget) != 0) throw Error(ErrorCode::Unsupported, "This field is read-only.");
    std::vector<Obj> widgets; collectWidgets(field, widgets);
    if (request.text.size() > maxText) throw Error(ErrorCode::ResourceLimit, "The field text is too long.");

    if (ft == "/Tx") {
        if (flags & (RichText | FileSelect)) throw Error(ErrorCode::Unsupported, "Rich-text and file-selection fields cannot be edited.");
        std::string text = request.text; bool lineBreaks = text.find_first_of("\r\n") != std::string::npos;
        if (!(flags & Multiline) && lineBreaks) throw Error(ErrorCode::InvalidSelection, "This field holds a single line.");
        auto max = inherited(field, "/MaxLen");
        if (max.isInteger() && max.getIntValue() > 0) {
            std::size_t chars = 0; for (std::size_t i = 0; i < text.size(); i += textedit::utf8Length(static_cast<unsigned char>(text[i]))) ++chars;
            if (chars > static_cast<std::size_t>(max.getIntValue())) throw Error(ErrorCode::InvalidSelection, "The text is longer than this field allows.");
        }
        // Appearances first: a character the font cannot show rejects the edit before anything changes.
        std::string shown = (flags & Password) ? std::string(text.size(), '*') : text;
        if (flags & Password) { shown.clear(); for (std::size_t i = 0; i < text.size(); i += textedit::utf8Length(static_cast<unsigned char>(text[i]))) shown += '*'; }
        for (auto& w : widgets) textAppearance(pdf, acroForm, field, w, shown, false, {}, {});
        if (text.empty()) field.removeKey("/V"); else field.replaceKey("/V", Obj::newUnicodeString(text));
    } else if (ft == "/Ch") {
        if (flags & MultiSelect) throw Error(ErrorCode::Unsupported, "Multiple-selection lists cannot be edited.");
        auto opts = optionsOf(field); const bool combo = flags & Combo, editable = combo && (flags & Edit);
        std::size_t hit = 0; bool known = false;
        for (std::size_t i = 0; i < opts.value.size(); ++i) if (opts.value[i] == request.text) { hit = i; known = true; break; }
        if (!known && !(editable && !request.text.empty()) && !request.text.empty()) throw Error(ErrorCode::InvalidSelection, "That choice is not one of the field's options.");
        const std::string shown = known ? opts.display[hit] : request.text;
        for (auto& w : widgets) textAppearance(pdf, acroForm, field, w, shown, !combo, opts, request.text);
        if (request.text.empty()) field.removeKey("/V"); else field.replaceKey("/V", Obj::newUnicodeString(request.text));
        field.removeKey("/I");
    } else if (ft == "/Btn" && !(flags & Pushbutton)) {
        const bool radio = flags & Radio;
        for (auto& w : widgets) ensureButtonAppearance(pdf, w, radio);
        auto chosen = onState(widget);
        if (chosen.empty()) throw Error(ErrorCode::InvalidDocument, "The button has no 'on' appearance.");
        if (radio) {
            if (!request.checked) { // A radio group with no toggle-off keeps its selection.
                if (!(flags & (1 << 14))) { for (auto& w : widgets) w.replaceKey("/AS", Obj::newName("/Off")); field.replaceKey("/V", Obj::newName("/Off")); }
                return;
            }
            for (auto& w : widgets) w.replaceKey("/AS", Obj::newName(onState(w) == chosen && w.getObjGen() == widget.getObjGen() ? "/" + chosen : "/Off"));
            field.replaceKey("/V", Obj::newName("/" + chosen));
        } else {
            for (auto& w : widgets) { auto s = onState(w); w.replaceKey("/AS", Obj::newName(request.checked && !s.empty() ? "/" + s : "/Off")); }
            field.replaceKey("/V", Obj::newName(request.checked ? "/" + chosen : "/Off"));
        }
    } else throw Error(ErrorCode::Unsupported, "This kind of field cannot be edited.");
}
}
