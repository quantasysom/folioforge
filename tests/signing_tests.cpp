#include "pdfengine/document.h"
#include "pdfengine/renderer.h"
#include <filesystem>
#include <iostream>

using namespace pdfengine;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Fn> void rejected(Fn fn, ErrorCode code) {
    try { fn(); } catch (const Error& error) { require(error.code == code, "Wrong error category"); return; }
    throw std::runtime_error("Expected rejection");
}
}
int main() {
#ifndef __APPLE__
    std::cout << "signing is macOS-only for now; skipped\n"; return 77;
#else
    try {
        auto root = std::filesystem::temp_directory_path() / "folioforge-signing-tests"; std::filesystem::remove_all(root); std::filesystem::create_directories(root);
        SignOptions options; options.certificate = std::filesystem::path(FOLIOFORGE_TEST_DATA) / "test-signer.p12"; options.password = "folioforge";
        options.reason = "Approved"; options.location = "Test lab"; options.signerName = "Test Signer";

        auto doc = Document::create();
        auto info = doc->info();
        doc->execute({CommandKind::InsertBlank, info.pages[0].id, info.revision});   // Exercise a document that has been edited.
        auto out = root / "signed.pdf";
        doc->signTo(out, options);
        require(std::filesystem::exists(out), "Signed file not written");

        auto bytes = readFile(out);
        auto checks = verifySignatures(bytes);
        require(checks.size() == 1 && checks[0].intact && checks[0].coversWholeFile, "Signature must verify and cover the file");
        require(checks[0].signer == "FolioForge Test Signer", "Signer name not read from the certificate");

        auto reopened = Document::open(out);
        require(reopened->info().pages.size() == 2, "Signed file must keep its pages");
        require(!reopened->info().editable, "A signed document must be read-only");
        require(Renderer::render(reopened->snapshot(), 0, 1).width > 0, "Signed file must render");

        // Any change to the signed bytes breaks the signature.
        auto tampered = bytes; tampered[20] ^= 0x01;
        auto bad = verifySignatures(tampered);
        require(bad.size() == 1 && !bad[0].intact, "Tampering must be detected");
        auto appended = bytes; for (char c : std::string("\n% appended")) appended.push_back(static_cast<unsigned char>(c));
        auto later = verifySignatures(appended);
        require(later.size() == 1 && later[0].intact && !later[0].coversWholeFile, "Appended data must be reported as not covered");

        // Failure cases leave nothing behind.
        auto wrong = options; wrong.password = "nope";
        rejected([&] { doc->signTo(root / "x.pdf", wrong); }, ErrorCode::InvalidSelection);
        require(!std::filesystem::exists(root / "x.pdf"), "A failed signing must not write output");
        rejected([&] { doc->signTo(out, options); }, ErrorCode::SaveFailed);
        auto missing = options; missing.certificate = root / "missing.p12";
        rejected([&] { doc->signTo(root / "y.pdf", missing); }, ErrorCode::InvalidSelection);
        rejected([&] { reopened->signTo(root / "z.pdf", options); }, ErrorCode::Unsupported);
        std::cout << "signing workflows passed\n";
    } catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
#endif
}
