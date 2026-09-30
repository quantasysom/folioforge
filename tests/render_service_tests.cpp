#include "pdfengine/document.h"
#include "pdfengine/render_service.h"
#include <cstdlib>
#include <iostream>

using namespace pdfengine;
namespace {
int checks{};
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class Fn> void rejected(Fn fn, ErrorCode code, const char* what) {
    try { fn(); } catch (const Error& error) { require(error.code == code, what); return; }
    throw std::runtime_error(std::string("Expected rejection: ") + what);
}
std::shared_ptr<Document> gradient(int width, int height) {
    ImagePage image; image.width = width; image.height = height; image.components = 3; image.dpi = 72;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        image.data.push_back(static_cast<unsigned char>(x * 255 / width)); image.data.push_back(static_cast<unsigned char>(y * 255 / height)); image.data.push_back(90);
    }
    return Document::createFromImage(image);
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: render-service-tests HELPER\n"; return 2; }
#ifdef _WIN32
    _putenv_s("FOLIOFORGE_RENDER_TEST_HOOKS", "1");
#else
    setenv("FOLIOFORGE_RENDER_TEST_HOOKS", "1", 1);
#endif
    try {
        auto direct = RenderService::inProcess();
        RenderService::Options options; options.renderTimeout = std::chrono::milliseconds(20000);
        auto isolated = RenderService::isolated(argv[1], options);
        auto doc = gradient(64, 48); auto snapshot = doc->snapshot();

        auto expected = direct->render(snapshot, 0, 2), actual = isolated->render(snapshot, 0, 2);
        require(expected.width == actual.width && expected.height == actual.height && expected.bgra == actual.bgra, "Isolated raster differs from in-process raster");
        require(actual.page == snapshot.pages[0] && actual.revision == snapshot.revision, "Bitmap identity lost across the process boundary");
        isolated->validate(snapshot);
        require(isolated->text(snapshot, 0) == direct->text(snapshot, 0), "Extracted text differs");

        doc->execute({CommandKind::InsertBlank, snapshot.pages[0], doc->info().revision});
        auto changed = doc->snapshot();
        require(isolated->render(changed, 1, 1).page == changed.pages[1], "Worker did not pick up the new snapshot");
        require(isolated->render(snapshot, 0, 1).page == snapshot.pages[0], "Worker did not switch back to the older snapshot");

        rejected([&] { isolated->render(changed, 99, 1); }, ErrorCode::InvalidSelection, "Out-of-range page must be a normal error");
        rejected([&] { isolated->render(changed, 0, 0); }, ErrorCode::ResourceLimit, "Bad zoom must be a normal error");
        require(isolated->render(changed, 0, 1).width > 0, "Worker must survive reported errors");

        rejected([&] { testing::runWorkerOperation(*isolated, testing::Hang, {}, std::chrono::milliseconds(300)); }, ErrorCode::RenderWorkerFailed, "Hung worker must time out");
        require(isolated->render(changed, 0, 1).width > 0, "Service did not restart after a timeout");
        rejected([&] { testing::runWorkerOperation(*isolated, testing::Crash, {}, std::chrono::milliseconds(20000)); }, ErrorCode::RenderWorkerFailed, "Crashed worker must be reported");
        require(isolated->render(changed, 0, 1).width > 0, "Service did not restart after a crash");

#if defined(__APPLE__)
        auto target = (std::filesystem::temp_directory_path() / "folioforge-sandbox-probe.txt").string();
        rejected([&] { testing::runWorkerOperation(*isolated, testing::WriteFile, target, std::chrono::milliseconds(20000)); }, ErrorCode::Unsupported, "Sandboxed worker must not write files");
        require(!std::filesystem::exists(target), "Sandboxed worker created a file");
#endif
        rejected([&] { RenderService::isolated("/nonexistent/pdfeditor-render"); }, ErrorCode::RenderWorkerFailed, "Missing helper must be reported");
        std::cout << checks << " render-service checks passed: isolation parity, snapshot caching, error propagation, timeout, crash recovery, sandbox\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
