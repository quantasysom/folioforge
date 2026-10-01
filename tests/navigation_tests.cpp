#include "pdfengine/document.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <filesystem>
#include <iostream>

using namespace pdfengine;
using Obj = QPDFObjectHandle;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void fixture(const std::filesystem::path& path) {
    QPDF pdf; pdf.emptyPDF();
    auto layer = pdf.makeIndirectObject(Obj::parse("<< /Type /OCG /Name (Notes) >>"));
    auto layer2 = pdf.makeIndirectObject(Obj::parse("<< /Type /OCG /Name (Grid) >>"));
    auto props = Obj::newDictionary();
    props.replaceKey("/OCGs", Obj::newArray(std::vector<Obj>{layer, layer2}));
    auto config = Obj::newDictionary(); config.replaceKey("/OFF", Obj::newArray(std::vector<Obj>{layer2}));
    props.replaceKey("/D", config);
    pdf.getRoot().replaceKey("/OCProperties", props);
    for (int i = 0; i < 3; ++i) {
        auto page = pdf.makeIndirectObject(Obj::parse("<< /Type /Page /MediaBox [0 0 200 200] /Resources << >> >>"));
        if (i == 0) page.replaceKey("/Annots", Obj::newArray(std::vector<Obj>{pdf.makeIndirectObject(Obj::parse("<< /Type /Annot /Subtype /Square /Rect [10 10 50 50] /Contents (first) /T (Ann) >>"))}));
        QPDFPageDocumentHelper(pdf).addPage(QPDFPageObjectHelper(page), false);
    }
    QPDFWriter writer(pdf); writer.setOutputFilename(path.string().c_str()); writer.write();
}
}
int main() {
    try {
        auto root = std::filesystem::temp_directory_path() / "folioforge-navigation-tests"; std::filesystem::remove_all(root); std::filesystem::create_directories(root);
        auto path = root / "in.pdf"; fixture(path);
        auto doc = Document::open(path);
        require(doc->info().editable, "Layers must not make a file read-only");
        auto id = [&](int i) { return doc->info().pages[i].id; };
        auto rev = [&] { return doc->info().revision; };

        // Comments: edit a foreign annotation, reply, remove with replies.
        doc->setAnnotationText(id(0), 0, "changed", rev());
        require(doc->annotations(id(0))[0].contents == "changed", "Comment text not updated");
        doc->replyToAnnotation(id(0), 0, "reply", rev());
        auto list = doc->annotations(id(0));
        require(list.size() == 2 && list[1].parent == 0 && list[1].contents == "reply", "Reply not linked");
        doc->undo(rev()); require(doc->annotations(id(0)).size() == 1, "Undo reply failed");
        doc->redo(rev());
        doc->removeAnnotation(id(0), 0, rev());
        require(doc->annotations(id(0)).empty(), "Removing the parent must remove its replies");

        // Bookmarks.
        require(doc->bookmarks().empty(), "No bookmarks expected");
        doc->addBookmark("First", id(0), rev());
        doc->addBookmark("Third", id(2), rev());
        auto marks = doc->bookmarks();
        require(marks.size() == 2 && marks[0].title == "First" && marks[0].page == 0 && marks[1].page == 2, "Bookmarks wrong");
        doc->renameBookmark(1, "Last", rev());
        require(doc->bookmarks()[1].title == "Last", "Rename failed");
        doc->execute({CommandKind::Delete, id(2), rev()});
        marks = doc->bookmarks();
        require(marks.size() == 1 && marks[0].title == "First", "Bookmark of a deleted page must be pruned");
        doc->undo(rev()); require(doc->bookmarks().size() == 2, "Undo must restore the bookmark");
        doc->removeBookmark(0, rev());
        require(doc->bookmarks().size() == 1 && doc->bookmarks()[0].title == "Last", "Remove failed");

        // Layers.
        auto layers = doc->layers();
        require(layers.size() == 2 && layers[0].visible && !layers[1].visible, "Layer state wrong");
        doc->setLayerVisible(1, true, rev()); doc->setLayerVisible(0, false, rev());
        layers = doc->layers();
        require(!layers[0].visible && layers[1].visible, "Layer toggle failed");
        doc->renameLayer(0, "Renamed", rev());
        require(doc->layers()[0].name == "Renamed", "Layer rename failed");
        doc->save(root / "out.pdf", true);
        auto reopened = Document::open(root / "out.pdf");
        require(reopened->layers().size() == 2 && reopened->bookmarks().size() == 1, "Saved file lost data");
        std::cout << "navigation ok\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
}
