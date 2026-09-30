#pragma once
#include "renderer.h"
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace pdfengine {
// Renders, extracts text and validates in a restricted helper process (pdfeditor-render), so a
// crashing, hanging or memory-hungry PDF can only take down the helper. Calls are serialized.
// On timeout, crash or protocol error the helper is killed, an Error is thrown, and a fresh
// helper is started by the next call. Snapshots are sent once and cached by the helper.
class RenderService {
public:
    struct Options {
        std::chrono::milliseconds renderTimeout{30000};
        std::chrono::milliseconds validateTimeout{120000};
        std::uint64_t memoryLimitBytes{4ull << 30};
    };
    virtual ~RenderService() = default;
    virtual Bitmap render(const Snapshot&, std::size_t page, double scale) = 0;
    virtual std::u16string text(const Snapshot&, std::size_t page) = 0;
    virtual void validate(const Snapshot&) = 0;

    // helper: path to the pdfeditor-render executable.
    static std::shared_ptr<RenderService> isolated(const std::filesystem::path& helper, Options options);
    static std::shared_ptr<RenderService> isolated(const std::filesystem::path& helper) { return isolated(helper, Options{}); }
    // Runs PDFium in this process. For tests and explicit opt-out only; provides no isolation.
    static std::shared_ptr<RenderService> inProcess();
    // Isolated service using the helper beside `executableDirectory`, or in-process when the
    // FOLIOFORGE_RENDER_INPROCESS environment variable is set. Throws if the helper is missing.
    static std::shared_ptr<RenderService> forApplication(const std::filesystem::path& executableDirectory);
    static std::filesystem::path helperName();
};
namespace testing {
// Sends a fault-injection request (hang/crash) to an isolated service; needs FOLIOFORGE_RENDER_TEST_HOOKS=1.
enum WorkerOp : unsigned { Hang = 100, Crash = 101, WriteFile = 102 };
void runWorkerOperation(RenderService&, unsigned op, const std::string& argument, std::chrono::milliseconds timeout);
}
}
