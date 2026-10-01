#include "pdfengine/document.h"
#include "pdfengine/renderer.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <iostream>
#include <random>

using namespace pdfengine;
using Obj = QPDFObjectHandle;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Fn> void rejected(Fn fn, ErrorCode code) {
    try { fn(); } catch (const Error& error) { require(error.code == code, "Wrong error category"); return; }
    throw std::runtime_error("Expected rejection");
}
Bytes fixture(bool signedDoc, bool xfa) {
    QPDF pdf; pdf.emptyPDF();
    auto font = pdf.makeIndirectObject(Obj::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
    auto page = pdf.makeIndirectObject(Obj::parse("<< /Type /Page /MediaBox [0 0 600 800] /Resources << >> >>"));
    page.replaceKey("/Contents", pdf.newStream(""));
    auto widget = [&](const char* dict) { auto w = pdf.makeIndirectObject(Obj::parse(dict)); w.replaceKey("/P", page); return w; };
    auto text = widget("<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /Rect [50 700 250 725] /DA (/Helv 12 Tf 0 g) /MaxLen 12 /MK << /BC [0 0 0] >> >>");
    auto check = widget("<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /Rect [50 650 70 670] /V /Off /AS /Off >>");
    auto radioParent = pdf.makeIndirectObject(Obj::parse("<< /FT /Btn /Ff 32768 /T (pick) /V /Off >>"));
    auto r1 = widget("<< /Type /Annot /Subtype /Widget /Rect [50 600 70 620] /AS /Off >>");
    auto r2 = widget("<< /Type /Annot /Subtype /Widget /Rect [100 600 120 620] /AS /Off >>");
    for (auto r : {r1, r2}) r.replaceKey("/Parent", radioParent);
    radioParent.replaceKey("/Kids", Obj::newArray(std::vector<Obj>{r1, r2}));
    auto combo = widget("<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /T (color) /Rect [50 550 200 575] /Opt [(red) [(g) (Green)] (blue)] /DA (/Helv 12 Tf 0 g) >>");
    auto sig = widget(signedDoc ? "<< /Type /Annot /Subtype /Widget /FT /Sig /T (sig) /Rect [300 700 400 740] /V << /Type /Sig /Filter /Adobe.PPKLite >> >>"
                                : "<< /Type /Annot /Subtype /Widget /FT /Sig /T (sig) /Rect [300 700 400 740] >>");
    std::vector<Obj> annots{text, check, r1, r2, combo, sig};
    page.replaceKey("/Annots", Obj::newArray(annots));
    auto helv = Obj::newDictionary(); helv.replaceKey("/Helv", font);
    auto dr = Obj::newDictionary(); dr.replaceKey("/Font", helv);
    auto form = Obj::newDictionary();
    form.replaceKey("/Fields", Obj::newArray(std::vector<Obj>{text, check, radioParent, combo, sig}));
    form.replaceKey("/DR", dr);
    if (xfa) form.replaceKey("/XFA", Obj::newArray());
    pdf.getRoot().replaceKey("/AcroForm", form);
    QPDFPageDocumentHelper(pdf).addPage(QPDFPageObjectHelper(page), false);
    QPDFWriter writer(pdf); writer.setOutputMemory(); writer.write(); auto bytes = writer.getBufferSharedPointer();
    return Bytes(bytes->getBuffer(), bytes->getBuffer() + bytes->getSize());
}
}
int main() {
    auto root = std::filesystem::temp_directory_path() / ("folio-form-tests-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(p, ec); } } cleanup{root};
    try {
        int n = 0;
        auto open = [&](bool sig, bool xfa) { auto p = root / (std::to_string(++n) + ".pdf"); atomicWrite(p, fixture(sig, xfa), false); return Document::open(p); };
        auto doc = open(false, false);
        require(doc->info().editable, "Unsigned AcroForm must be editable");
        auto page = doc->info().pages[0].id;
        auto fields = doc->formFields(page);
        require(fields.size() == 6, "Expected six widgets");
        require(fields[0].kind == FormFieldKind::Text && fields[0].name == "name" && fields[0].maxLength == 12, "Text field wrong");
        require(fields[1].kind == FormFieldKind::Checkbox && !fields[1].checked, "Checkbox wrong");
        require(fields[2].kind == FormFieldKind::Radio && fields[2].name == "pick", "Radio wrong");
        require(fields[4].kind == FormFieldKind::Choice && fields[4].combo && fields[4].options.size() == 3 && fields[4].optionValues[1] == "g" && fields[4].options[1] == "Green", "Combo wrong");
        require(fields[5].kind == FormFieldKind::Signature && fields[5].readOnly, "Signature must be read-only");

        auto before = Renderer::render(doc->snapshot(), 0, 1);
        auto set = [&](std::uint32_t widget, const std::string& text, bool checked = false) {
            doc->setFormValue({page, doc->info().revision, widget, text, checked});
        };
        set(0, "Jane Q Pub");
        require(doc->formFields(page)[0].value == "Jane Q Pub", "Text value not stored");
        auto after = Renderer::render(doc->snapshot(), 0, 1);
        require(after.bgra != before.bgra, "Text field did not render");
        set(1, "", true);
        require(doc->formFields(page)[1].checked, "Checkbox not checked");
        set(2, "", true);
        require(doc->formFields(page)[2].checked && !doc->formFields(page)[3].checked, "Radio selection wrong");
        set(3, "", true);
        require(!doc->formFields(page)[2].checked && doc->formFields(page)[3].checked, "Radio must be exclusive");
        set(4, "g");
        require(doc->formFields(page)[4].value == "g", "Combo value not stored");

        auto checkpoint = doc->snapshot();
        rejected([&] { set(0, "this is longer than twelve"); }, ErrorCode::InvalidSelection);
        rejected([&] { set(0, "\xE0\xB0\x85"); }, ErrorCode::Unsupported);
        rejected([&] { set(0, "a\nb"); }, ErrorCode::InvalidSelection);
        rejected([&] { set(4, "purple"); }, ErrorCode::InvalidSelection);
        rejected([&] { set(5, "x"); }, ErrorCode::Unsupported);
        rejected([&] { doc->setFormValue({page, checkpoint.revision - 1, 0, "x", false}); }, ErrorCode::StaleRevision);
        require(doc->snapshot().bytes == checkpoint.bytes, "Rejected form edit changed state");

        auto out = root / "filled.pdf"; Renderer::validate(doc->snapshot()); doc->save(out);
        auto reopened = Document::open(out);
        auto again = reopened->formFields(reopened->info().pages[0].id);
        require(again[0].value == "Jane Q Pub" && again[1].checked && again[3].checked && again[4].value == "g", "Values lost on save/reopen");

        doc->undo(doc->info().revision);
        require(doc->formFields(page)[4].value.empty(), "Undo did not revert the form edit");

        rejected([&] { doc->execute({CommandKind::Delete, page, doc->info().revision}); }, ErrorCode::Unsupported);
        rejected([&] { doc->execute({CommandKind::Duplicate, page, doc->info().revision}); }, ErrorCode::Unsupported);
        doc->execute({CommandKind::RotateRight, page, doc->info().revision});

        require(!open(true, false)->info().editable, "Signed document must be read-only");
        require(!open(false, true)->info().editable, "XFA document must be read-only");
        std::cout << "form tests passed\n";
    } catch (const std::exception& error) { std::cerr << "form tests failed: " << error.what() << "\n"; return 1; }
}
