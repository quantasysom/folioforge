#include "pdfengine/document.h"
#include "pdfengine/renderer.h"
#include "sfnt.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>

using namespace pdfengine;
namespace {
using Obj = QPDFObjectHandle;
int checks{};
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class Fn> void rejected(Fn fn, ErrorCode code) {
    try { fn(); } catch (const Error& error) { require(error.code == code, "Wrong error category"); return; }
    throw std::runtime_error("Expected rejection");
}
std::filesystem::path findFont() {
    std::vector<std::filesystem::path> candidates;
    if (const char* env = std::getenv("FOLIOFORGE_TEST_FONT")) candidates.push_back(env);
    for (const char* path : {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/Library/Fonts/Arial Unicode.ttf", "C:/Windows/Fonts/arial.ttf",
                             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"})
        candidates.push_back(path);
    for (const auto& path : candidates) if (std::filesystem::exists(path) && font::Sfnt::load(path)) return path;
    return {};
}
Bytes build(const std::string& commands, const std::function<Obj(QPDF&)>& makeFont) {
    QPDF pdf; pdf.emptyPDF();
    auto page = Obj::parse("<< /Type /Page /MediaBox [0 0 600 800] >>");
    auto fonts = Obj::newDictionary(); fonts.replaceKey("/F1", makeFont(pdf));
    auto resources = Obj::newDictionary(); resources.replaceKey("/Font", fonts); page.replaceKey("/Resources", resources);
    page.replaceKey("/Contents", pdf.newStream(commands));
    QPDFPageDocumentHelper(pdf).addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
    QPDFWriter writer(pdf); writer.setOutputMemory(); writer.write(); auto bytes = writer.getBufferSharedPointer();
    return Bytes(bytes->getBuffer(), bytes->getBuffer() + bytes->getSize());
}
std::string toUnicodeMap(const std::vector<std::pair<unsigned, std::u16string>>& entries, bool twoByte) {
    std::string map = "/CIDInit /ProcSet findresource begin 12 dict begin begincmap /CMapName /T def /CMapType 2 def 1 begincodespacerange " +
        std::string(twoByte ? "<0000> <FFFF>" : "<00> <FF>") + " endcodespacerange\n" + std::to_string(entries.size()) + " beginbfchar\n";
    for (const auto& [code, text] : entries) {
        char b[16]; std::snprintf(b, sizeof b, twoByte ? "<%04X> <" : "<%02X> <", code); map += b;
        for (auto unit : text) { std::snprintf(b, sizeof b, "%04X", unsigned(unit)); map += b; }
        map += ">\n";
    }
    return map + "endbfchar endcmap end end";
}
std::string hexGlyphs(const font::Sfnt& face, const std::u32string& text) {
    std::string out = "<";
    for (auto cp : text) { char b[8]; std::snprintf(b, sizeof b, "%04X", face.glyph(cp)); out += b; }
    return out + ">";
}
std::u16string joinText(Document& doc, PageId page) {
    std::string all;
    for (const auto& run : doc.textRuns(page).runs) all += run.text;
    std::u16string out;
    for (std::size_t i = 0; i < all.size();) {
        unsigned char c = all[i]; char32_t cp; std::size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        cp = n == 1 ? c : c & (0xFF >> (n + 1));
        for (std::size_t k = 1; k < n; ++k) cp = cp << 6 | (all[i + k] & 0x3F);
        i += n;
        if (cp >= 0x10000) { cp -= 0x10000; out += char16_t(0xD800 + (cp >> 10)); out += char16_t(0xDC00 + (cp & 0x3FF)); } else out += char16_t(cp);
    }
    return out;
}
std::size_t inkIn(const Bitmap& bitmap, double x0, double x1, double baseline, double size) {
    std::size_t count = 0;
    for (int y = std::max(0, int(bitmap.height - baseline - size)); y < std::min<int>(bitmap.height, int(bitmap.height - baseline + size * 0.3)); ++y)
        for (int x = std::max(0, int(x0)); x < std::min<int>(bitmap.width, int(x1)); ++x) {
            auto* px = &bitmap.bgra[std::size_t(y) * bitmap.stride + std::size_t(x) * 4];
            if (px[0] < 128 && px[1] < 128 && px[2] < 128) ++count;
        }
    return count;
}
std::size_t fontCount(const Snapshot& snapshot) {
    QPDF pdf; pdf.processMemoryFile("f", reinterpret_cast<const char*>(snapshot.bytes->data()), snapshot.bytes->size());
    return QPDFPageDocumentHelper(pdf).getAllPages()[0].getAttribute("/Resources", false).getKey("/Font").getKeys().size();
}
}
int main() {
    auto fontPath = findFont();
    if (fontPath.empty()) { std::cout << "SKIP: no TrueType font available (set FOLIOFORGE_TEST_FONT)\n"; return 77; }
    auto root = std::filesystem::temp_directory_path() / ("folio-font-tests-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(p, ec); } } cleanup{root};
    try {
        auto face = font::Sfnt::load(fontPath);
        int index = 0;
        auto open = [&](const std::string& commands, const std::function<Obj(QPDF&)>& makeFont) {
            auto path = root / (std::to_string(++index) + ".pdf"); atomicWrite(path, build(commands, makeFont), false);
            auto doc = Document::open(path); doc->setFallbackFonts({fontPath}); return doc;
        };
        auto helvetica = [](QPDF& pdf) { return pdf.makeIndirectObject(Obj::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")); };

        // Standard font, text the font cannot encode: characters come from an embedded fallback font.
        const std::string commands = "BT /F1 24 Tf 1 0 0 1 40 500 Tm (Hello) Tj ( NEXT) Tj ET";
        auto doc = open(commands, helvetica); auto page = doc->info().pages[0].id;
        auto before = Renderer::render(doc->snapshot(), 0, 1); auto original = doc->snapshot();
        auto run = doc->textRuns(page).runs.front();
        doc->replaceText({page, run.id, run.revision, "Gr\xC3\xBC\xC3\x9F" "e \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xCE\x95\xCE\xBB\xCE\xBB"});
        require(fontCount(doc->snapshot()) == 2, "Fallback font was not embedded");
        require(joinText(*doc, page).find(u"Gr\u00FC\u00DFe \u041F\u0440\u0438\u0432\u0435\u0442 \u0395\u03BB\u03BB") != std::u16string::npos, "Mixed-script text did not round-trip through the inventory");
        auto extracted = Renderer::text(doc->snapshot(), 0);
        require(extracted.find(u"\u041F\u0440\u0438\u0432\u0435\u0442") != std::u16string::npos, "Extracted text lost Cyrillic");
        require(extracted.find(u"NEXT") != std::u16string::npos, "Text after the run was lost");
        auto after = Renderer::render(doc->snapshot(), 0, 1);
        require(inkIn(after, 40, 250, 500, 24) > inkIn(before, 40, 250, 500, 24) / 2, "Fallback glyphs did not draw");
        auto edited = doc->textRuns(page).runs;
        require(edited.back().text == " NEXT" && edited.back().x > run.x + 100, "Following run did not reflow after the fallback text");
        auto saved = root / "saved.pdf"; Renderer::validate(doc->snapshot()); doc->save(saved);
        auto reopened = Document::open(saved);
        require(joinText(*reopened, reopened->info().pages[0].id).find(u"\u041F\u0440\u0438\u0432\u0435\u0442") != std::u16string::npos, "Saved file lost fallback text");
        auto again = doc->textRuns(page).runs.front();
        doc->replaceText({page, again.id, again.revision, "Hello"});
        require(joinText(*doc, page).find(u"Hello") != std::u16string::npos, "Editing fallback text back failed");
        doc->undo(doc->info().revision); doc->undo(doc->info().revision);
        require(Renderer::render(doc->snapshot(), 0, 1).bgra == before.bgra && doc->snapshot().bytes == original.bytes, "Undo did not restore the original");

        if (face->glyph(0x65E5) && face->glyph(0x672C) && face->glyph(0x3042)) {
            auto cjk = open(commands, helvetica); auto cjkRun = cjk->textRuns(cjk->info().pages[0].id).runs.front();
            cjk->replaceText({cjkRun.page, cjkRun.id, cjkRun.revision, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\x82"});
            require(joinText(*cjk, cjk->info().pages[0].id).find(u"\u65E5\u672C\u8A9E\u3042") != std::u16string::npos, "CJK text did not round-trip");
            require(inkIn(Renderer::render(cjk->snapshot(), 0, 1), 40, 200, 500, 24) > 50, "CJK glyphs did not draw");
        }

        // Embedded CID font (Identity-H, ToUnicode): reuse its own glyphs, fall back only for missing ones.
        auto cid = [&](QPDF& pdf) {
            std::ifstream in(fontPath, std::ios::binary); std::string data((std::istreambuf_iterator<char>(in)), {});
            auto file = pdf.newStream(data); file.replaceDict(Obj::parse("<< /Length1 " + std::to_string(data.size()) + " >>"));
            auto descriptor = Obj::parse("<< /Type /FontDescriptor /FontName /ABCDEF+Embedded /Flags 4 /FontBBox [-500 -300 1500 1000] /ItalicAngle 0 /Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 >>");
            descriptor.replaceKey("/FontFile2", file);
            auto widths = Obj::newArray(); std::vector<std::pair<unsigned, std::u16string>> entries;
            for (char32_t cp : std::u32string(U"Helo Wrd")) {
                if (!face->glyph(cp) || (!entries.empty() && std::any_of(entries.begin(), entries.end(), [&](auto& e) { return e.first == face->glyph(cp); }))) continue;
                widths.appendItem(Obj::newInteger(face->glyph(cp)));
                widths.appendItem(Obj::parse("[" + std::to_string(std::llround(face->advance(face->glyph(cp)) * 1000.0 / face->unitsPerEm)) + "]"));
                entries.push_back({face->glyph(cp), std::u16string(1, char16_t(cp))});
            }
            auto descendant = Obj::parse("<< /Type /Font /Subtype /CIDFontType2 /BaseFont /ABCDEF+Embedded /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /CIDToGIDMap /Identity /DW 1000 >>");
            descendant.replaceKey("/FontDescriptor", pdf.makeIndirectObject(descriptor)); descendant.replaceKey("/W", widths);
            auto type0 = Obj::parse("<< /Type /Font /Subtype /Type0 /BaseFont /ABCDEF+Embedded /Encoding /Identity-H >>");
            type0.replaceKey("/DescendantFonts", Obj::newArray(std::vector<Obj>{pdf.makeIndirectObject(descendant)}));
            type0.replaceKey("/ToUnicode", pdf.newStream(toUnicodeMap(entries, true)));
            return pdf.makeIndirectObject(type0);
        };
        auto embedded = open("BT /F1 24 Tf 1 0 0 1 40 500 Tm " + hexGlyphs(*face, U"Hello World") + " Tj ET", cid);
        auto embeddedPage = embedded->info().pages[0].id; auto embeddedRun = embedded->textRuns(embeddedPage).runs.front();
        require(embeddedRun.text == "Hello World", "Embedded CID text was not decoded through ToUnicode");
        auto embeddedBefore = Renderer::render(embedded->snapshot(), 0, 1);
        require(inkIn(embeddedBefore, 40, 300, 500, 24) > 100, "Fixture CID font did not draw");
        embedded->replaceText({embeddedPage, embeddedRun.id, embeddedRun.revision, "World Hello"});
        require(fontCount(embedded->snapshot()) == 1, "Reusing the embedded font's glyphs must not embed another font");
        require(joinText(*embedded, embeddedPage) == u"World Hello", "Embedded font edit failed");
        require(inkIn(Renderer::render(embedded->snapshot(), 0, 1), 40, 300, 500, 24) > 100, "Reused embedded glyphs did not draw");
        embeddedRun = embedded->textRuns(embeddedPage).runs.front();
        embedded->replaceText({embeddedPage, embeddedRun.id, embeddedRun.revision, "Hello Wor\xD0\x96"});
        require(fontCount(embedded->snapshot()) == 2 && joinText(*embedded, embeddedPage) == u"Hello Wor\u0416", "Missing glyph did not fall back");

        // Simple TrueType font with WinAnsi + Widths: accented Latin-1 needs no fallback.
        auto simple = open("BT /F1 24 Tf 1 0 0 1 40 500 Tm (Cafe) Tj ET", [&](QPDF& pdf) {
            std::string widths = "[";
            for (unsigned c = 32; c < 256; ++c) widths += (c < 127 || c >= 160 ? std::to_string(std::llround(face->advance(face->glyph(c)) * 1000.0 / face->unitsPerEm)) : "0") + " ";
            return pdf.makeIndirectObject(Obj::parse("<< /Type /Font /Subtype /TrueType /BaseFont /ArialUnicodeMS /Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 255 /Widths " + widths + "] " +
                "/FontDescriptor << /Type /FontDescriptor /FontName /ArialUnicodeMS /Flags 32 /FontBBox [-500 -300 1500 1000] /ItalicAngle 0 /Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 >> >>"));
        });
        auto simpleRun = simple->textRuns(simple->info().pages[0].id).runs.front();
        simple->replaceText({simpleRun.page, simpleRun.id, simpleRun.revision, "Caf\xC3\xA9"});
        require(fontCount(simple->snapshot()) == 1 && joinText(*simple, simple->info().pages[0].id) == u"Caf\u00E9", "WinAnsi Latin-1 edit failed");
        simpleRun = simple->textRuns(simple->info().pages[0].id).runs.front();
        simple->replaceText({simpleRun.page, simpleRun.id, simpleRun.revision, "Caf\xC3\xA9 \xE2\x82\xAC"});
        require(fontCount(simple->snapshot()) == 2 && joinText(*simple, simple->info().pages[0].id) == u"Caf\u00E9 \u20AC", "Glyph without a width did not fall back");

        // Subset simple font with a ToUnicode map: only mapped glyphs may be reused.
        auto subset = open("BT /F1 24 Tf 1 0 0 1 40 500 Tm (ABC) Tj ET", [&](QPDF& pdf) {
            auto font = Obj::parse("<< /Type /Font /Subtype /TrueType /BaseFont /ABCDEF+Sub /FirstChar 65 /LastChar 67 /Widths [700 650 720] /FontDescriptor << /Type /FontDescriptor /FontName /ABCDEF+Sub /Flags 4 /FontBBox [0 0 1000 1000] /ItalicAngle 0 /Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 >> >>");
            font.replaceKey("/ToUnicode", pdf.newStream(toUnicodeMap({{65, u"A"}, {66, u"B"}, {67, u"C"}}, false)));
            return pdf.makeIndirectObject(font);
        });
        auto subsetRun = subset->textRuns(subset->info().pages[0].id).runs.front();
        require(subsetRun.text == "ABC", "Subset font text was not decoded");
        subset->replaceText({subsetRun.page, subsetRun.id, subsetRun.revision, "CAB"});
        require(fontCount(subset->snapshot()) == 1, "Subset glyph reuse embedded a font");
        subsetRun = subset->textRuns(subset->info().pages[0].id).runs.front();
        subset->replaceText({subsetRun.page, subsetRun.id, subsetRun.revision, "ABZ"});
        require(fontCount(subset->snapshot()) == 2 && joinText(*subset, subset->info().pages[0].id) == u"ABZ", "Subset font missing glyph did not fall back");

        // Unsupported or unsafe input is rejected without changing the document.
        auto guarded = open(commands, helvetica); auto guard = guarded->textRuns(guarded->info().pages[0].id).runs.front(); auto checkpoint = guarded->snapshot();
        for (const char* bad : {"\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7", "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", "\xE0\xA4\xA8\xE0\xA4\xAE", "a\nb", "\xFF\xFE", "\xF4\x8F\xBF\xBD"})
            rejected([&] { guarded->replaceText({guard.page, guard.id, guard.revision, bad}); }, ErrorCode::Unsupported);
        rejected([&] { guarded->replaceText({guard.page, guard.id, guard.revision, std::string(60, 'W') + "\xD0\x96" + std::string(60, 'W')}); }, ErrorCode::TextOverflow);
        require(guarded->snapshot().bytes == checkpoint.bytes, "Rejected edit changed the document");
        std::ofstream(root / "notafont.ttf") << "not a font";
        rejected([&] { guarded->setFallbackFonts({root / "notafont.ttf"}); }, ErrorCode::Unsupported);
        std::cout << checks << " font checks passed: fallback embedding, CJK, embedded CID reuse, WinAnsi Latin-1, subset fonts, shaping/RTL rejection\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
