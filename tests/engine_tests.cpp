#include "pdfengine/document.h"
#include "pdfengine/renderer.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <chrono>
#include <iostream>
#include <random>

using namespace pdfengine;
namespace {
int checks = 0;
void require(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
template <typename Fn> void rejected(Fn fn, ErrorCode code) {
    try { fn(); } catch (const Error& e) { require(e.code == code, "Unexpected rejection code"); return; }
    throw std::runtime_error("Expected operation rejection");
}
void execute(const std::shared_ptr<Document>& doc, CommandKind kind, std::size_t page = 0) {
    doc->execute({kind, doc->info().pages.at(page).id, doc->info().revision});
}
Bytes fixture(bool structures = false, bool encrypted = false) {
    QPDF pdf; pdf.emptyPDF();
    using O = QPDFObjectHandle;
    auto page = O::newDictionary(); page.replaceKey("/Type", O::newName("/Page"));
    page.replaceKey("/MediaBox", O::newArray(O::Rectangle(0, 0, 320, 480)));
    auto font = O::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    auto fonts = O::newDictionary(); fonts.replaceKey("/F1", pdf.makeIndirectObject(font));
    auto resources = O::newDictionary(); resources.replaceKey("/Font", fonts); page.replaceKey("/Resources", resources);
    page.replaceKey("/Contents", pdf.newStream("0.1 0.4 0.8 rg 30 320 260 80 re f\nBT /F1 22 Tf 30 280 Td (FolioForge test) Tj ET\n"));
    QPDFPageDocumentHelper(pdf).addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
    if (structures) pdf.getRoot().replaceKey("/AcroForm", O::parse("<< /Fields [] >>"));
    QPDFWriter writer(pdf); writer.setOutputMemory();
    if (encrypted) writer.setR6EncryptionParameters("secret", "owner", true, true, true, true, true, true, qpdf_r3p_full, true);
    writer.write(); auto buffer = writer.getBufferSharedPointer(); return Bytes(buffer->getBuffer(), buffer->getBuffer() + buffer->getSize());
}
}
int main() {
    auto root = std::filesystem::temp_directory_path() / ("folio-tests-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
    try {
        auto start = std::chrono::steady_clock::now();
        auto doc = Document::create();
        require(doc->info().dirty, "New document should be dirty");
        auto originalId = doc->info().pages[0].id;
        auto snapshot = doc->snapshot();
        rejected([&] { execute(doc, CommandKind::Delete); }, ErrorCode::InvalidSelection);
        require(doc->snapshot().bytes == snapshot.bytes && doc->info().revision == snapshot.revision, "Rejected delete changed state");
        execute(doc, CommandKind::RotateRight);
        require(doc->info().pages[0].rotation == 90, "Rotation incorrect");
        rejected([&] { doc->execute({CommandKind::RotateRight, originalId, snapshot.revision}); }, ErrorCode::StaleRevision);
        execute(doc, CommandKind::InsertBlank);
        auto secondId = doc->info().pages[1].id;
        require(secondId != originalId, "Page IDs are not unique");
        execute(doc, CommandKind::MoveEarlier, 1);
        require(doc->info().pages[0].id == secondId, "Moving page changed identity");
        doc->undo(doc->info().revision);
        require(doc->info().pages[0].id == originalId, "Undo did not restore order");
        execute(doc, CommandKind::Duplicate);
        require(doc->info().pages[1].id != originalId, "Duplicate reused original ID");
        execute(doc, CommandKind::RotateRight, 1);
        require(doc->info().pages[0].rotation == 90 && doc->info().pages[1].rotation == 180, "Duplicate rotation altered original");
        auto output = root / "roundtrip.pdf"; Renderer::validate(doc->snapshot()); doc->save(output);
        require(!doc->info().dirty, "Save did not clear dirty state");
        execute(doc, CommandKind::RotateRight);
        require(doc->info().dirty, "Mutation did not set dirty state");
        doc->undo(doc->info().revision);
        require(!doc->info().dirty, "Undo to saved state should clear dirty");
        doc->redo(doc->info().revision);
        require(doc->info().dirty, "Redo should restore dirty");
        auto reopened = Document::open(output);
        require(reopened->info().pages.size() == 3 && reopened->info().pages[0].rotation == 90, "Save/reopen mismatch");
        auto originalBytes = readFile(output);
        rejected([&] { atomicWrite(output, fixture(), false); }, ErrorCode::SaveFailed);
        require(readFile(output) == originalBytes, "Collision protection changed an existing file");
        rejected([&] { doc->save(root / "missing" / "out.pdf"); }, ErrorCode::SaveFailed);
        require(doc->info().dirty && readFile(output) == originalBytes, "Failed save damaged original or dirty flag");
        atomicWrite(output, fixture(), true);
        auto external = readFile(output);
        rejected([&] { doc->save(output); }, ErrorCode::ExternalModification);
        require(readFile(output) == external && doc->info().dirty, "Conflict overwrote external changes");
        auto input = root / "fixture.pdf"; auto inputBytes = fixture(); atomicWrite(input, inputBytes, false);
        auto content = Document::open(input); auto before = Renderer::render(content->snapshot(), 0, 1);
        require(Renderer::text(content->snapshot(), 0).find(u"FolioForge test") != std::u16string::npos, "Text extraction failed");
        auto old = content->snapshot(); execute(content, CommandKind::RotateRight);
        auto after = Renderer::render(content->snapshot(), 0, 1);
        require(before.width == after.height && before.height == after.width, "Rendered rotation has incorrect dimensions");
        require(Renderer::render(old, 0, 1).bgra == before.bgra, "Immutable snapshot changed");
        content->insertDocument(input, content->info().pages[0].id, content->info().revision);
        require(Renderer::render(content->snapshot(), 1, 1).bgra == before.bgra, "Imported resources did not preserve appearance");
        require(readFile(input) == inputBytes, "Import modified the source file");
        auto importOut = root / "merged.pdf"; content->save(importOut);
        auto merged = Document::open(importOut);
        require(Renderer::text(merged->snapshot(), 1).find(u"FolioForge test") != std::u16string::npos, "Imported text lost after reopen");
        auto structured = root / "forms.pdf"; atomicWrite(structured, fixture(true), false);
        auto restricted = Document::open(structured);
        require(!restricted->info().editable, "Structured PDF should be read-only");
        rejected([&] { execute(restricted, CommandKind::Delete); }, ErrorCode::Unsupported);
        rejected([&] { restricted->save(root / "forbidden.pdf"); }, ErrorCode::Unsupported);
        auto currentRevision = content->info().revision;
        rejected([&] { content->insertDocument(structured, content->info().pages[0].id, currentRevision); }, ErrorCode::Unsupported);
        require(content->info().revision == currentRevision, "Failed import changed revision");
        auto malformed = root / "malformed.pdf";
        atomicWrite(malformed, Bytes{'n','o','t',' ','p','d','f'}, false);
        rejected([&] { Document::open(malformed); }, ErrorCode::InvalidDocument);
        auto savedSnap = content->snapshot();
        rejected([&] { content->execute({CommandKind::RotateRight, 999999, savedSnap.revision}); }, ErrorCode::InvalidSelection);
        require(content->snapshot().bytes == savedSnap.bytes && content->info().revision == savedSnap.revision, "Invalid selection mutated the session");
        auto locked = root / "encrypted.pdf"; atomicWrite(locked, fixture(false, true), false);
        rejected([&] { Document::open(locked); }, ErrorCode::PasswordRequired);
        auto unlocked = Document::open(locked, "secret");
        require(!unlocked->info().editable && !Renderer::text(unlocked->snapshot(), 0).empty(), "Password PDF viewing failed");
        for (int i = 0; i < 40; ++i) execute(content, CommandKind::RotateRight);
        for (int i = 0; i < 40; ++i) content->undo(content->info().revision);
        for (int i = 0; i < 40; ++i) content->redo(content->info().revision);
        require(content->info().pages.size() == 2, "Mixed history lost pages");
        content->undo(content->info().revision);
        auto undoneRevision = content->info().revision;
        execute(content, CommandKind::RotateLeft);
        require(!content->info().canRedo && content->info().revision > undoneRevision, "Branching must clear redo and advance revision");
        auto unicodePath = root / std::filesystem::path(u8"roundtrip-తెలుగు.pdf");
        content->save(unicodePath);
        require(Document::open(unicodePath)->info().pages.size() == 2, "Unicode path round trip failed");
        rejected([&] { Renderer::render(content->snapshot(), 0, 0); }, ErrorCode::ResourceLimit);
        require(Renderer::render(snapshot, 0, 0.5).width > 0, "Old snapshot lost lifetime");
        atomicWrite(std::filesystem::current_path() / "smoke-input.pdf", inputBytes, true);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::cout << checks << " checks passed; generated fixtures, round trips, rendering, conflict/failure/history tests in " << elapsed << " ms\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
