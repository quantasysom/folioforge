#include "pdfengine/render_service.h"
#include "process.h"
#include "render_protocol.h"
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>

namespace pdfengine {
namespace {
class InProcessService : public RenderService {
public:
    Bitmap render(const Snapshot& s, std::size_t page, double scale) override { return Renderer::render(s, page, scale); }
    std::u16string text(const Snapshot& s, std::size_t page) override { return Renderer::text(s, page); }
    void validate(const Snapshot& s) override { Renderer::validate(s); }
};

// Kills the helper if a call outlives its deadline; this unblocks the pending pipe operation.
class Watchdog {
public:
    Watchdog(detail::Process& process, std::chrono::milliseconds timeout) : thread_([this, &process, timeout] {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return done_; })) { expired_ = true; process.kill(); }
    }) {}
    ~Watchdog() { stop(); }
    bool stop() {
        { std::lock_guard lock(mutex_); done_ = true; }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        return expired_;
    }
private:
    std::mutex mutex_; std::condition_variable cv_; bool done_{}, expired_{};
    std::thread thread_;
};

class IsolatedService : public RenderService {
public:
    IsolatedService(std::filesystem::path helper, Options options) : helper_(std::move(helper)), options_(options) {
        if (!std::filesystem::exists(helper_)) throw Error(ErrorCode::RenderWorkerFailed, "The rendering worker is missing: " + helper_.string());
    }
    ~IsolatedService() override {
        if (process_) { protocol::Header quit{protocol::requestMagic, protocol::Quit, 0}; process_->write(&quit, sizeof quit); }
    }
    Bitmap render(const Snapshot& snapshot, std::size_t page, double scale) override {
        std::lock_guard lock(mutex_);
        protocol::Writer request; request.put<std::uint64_t>(page); request.put<double>(scale);
        auto reply = call(snapshot, protocol::Render, request.bytes, options_.renderTimeout);
        protocol::Reader in(reply.data(), reply.size());
        Bitmap bitmap;
        bitmap.width = in.get<std::int32_t>(); bitmap.height = in.get<std::int32_t>(); bitmap.stride = in.get<std::int32_t>();
        bitmap.revision = in.get<std::uint64_t>(); bitmap.page = in.get<std::uint64_t>();
        auto size = in.get<std::uint64_t>();
        if (bitmap.width < 1 || bitmap.height < 1 || bitmap.stride != bitmap.width * 4 || size != static_cast<std::uint64_t>(bitmap.stride) * bitmap.height || size != in.remaining())
            fail("The rendering worker returned an invalid bitmap.");
        auto pixels = in.take(static_cast<std::size_t>(size));
        bitmap.bgra.assign(pixels, pixels + size);
        return bitmap;
    }
    std::u16string text(const Snapshot& snapshot, std::size_t page) override {
        std::lock_guard lock(mutex_);
        protocol::Writer request; request.put<std::uint64_t>(page);
        auto reply = call(snapshot, protocol::Text, request.bytes, options_.renderTimeout);
        if (reply.size() % sizeof(char16_t)) fail("The rendering worker returned invalid text.");
        std::u16string result(reply.size() / sizeof(char16_t), u'\0');
        std::memcpy(result.data(), reply.data(), reply.size());
        return result;
    }
    void validate(const Snapshot& snapshot) override {
        std::lock_guard lock(mutex_);
        call(snapshot, protocol::Validate, {}, options_.validateTimeout);
    }
    // Test-only: exercises timeout and crash handling.
    void testOperation(protocol::Op op, const std::string& argument, std::chrono::milliseconds timeout) {
        std::lock_guard lock(mutex_);
        Snapshot empty; call(empty, op, std::vector<unsigned char>(argument.begin(), argument.end()), timeout, false);
    }
private:
    std::filesystem::path helper_; Options options_;
    std::mutex mutex_;
    std::unique_ptr<detail::Process> process_;
    std::shared_ptr<const Bytes> loaded_; RevisionId loadedRevision_{}; std::vector<PageId> loadedPages_;

    [[noreturn]] void fail(const std::string& message) { reset(); throw Error(ErrorCode::RenderWorkerFailed, message); }
    void reset() { process_.reset(); loaded_.reset(); }
    bool send(const protocol::Header& header, const void* a, std::size_t an, const void* b = nullptr, std::size_t bn = 0) {
        return process_->write(&header, sizeof header) && (!an || process_->write(a, an)) && (!bn || process_->write(b, bn));
    }
    std::optional<std::vector<unsigned char>> exchange(protocol::Op op, const std::vector<unsigned char>& payload, const Snapshot* load) {
        if (load) {
            protocol::Writer header; header.put<std::uint64_t>(load->revision); header.put<std::uint64_t>(load->pages.size());
            for (auto id : load->pages) header.put<std::uint64_t>(id);
            header.put<std::uint64_t>(load->bytes->size());
            protocol::Header frame{protocol::requestMagic, protocol::Load, header.bytes.size() + load->bytes->size()};
            if (!send(frame, header.bytes.data(), header.bytes.size(), load->bytes->data(), load->bytes->size())) return std::nullopt;
            if (!readResponse()) return std::nullopt;
        }
        protocol::Header frame{protocol::requestMagic, op, payload.size()};
        if (!send(frame, payload.data(), payload.size())) return std::nullopt;
        return readResponse();
    }
    std::optional<std::vector<unsigned char>> readResponse() {
        protocol::Header header;
        if (!process_->read(&header, sizeof header) || header.magic != protocol::responseMagic || header.length > protocol::maxPayload) return std::nullopt;
        std::vector<unsigned char> payload(static_cast<std::size_t>(header.length));
        if (!payload.empty() && !process_->read(payload.data(), payload.size())) return std::nullopt;
        if (header.code != 0) {
            // A reported failure (bad page, resource limit) is a normal answer; the helper stays alive.
            throw WorkerError{static_cast<ErrorCode>(header.code - 1), std::string(payload.begin(), payload.end())};
        }
        return payload;
    }
    struct WorkerError { ErrorCode code; std::string message; };
    std::vector<unsigned char> call(const Snapshot& snapshot, protocol::Op op, const std::vector<unsigned char>& payload,
                                    std::chrono::milliseconds timeout, bool needsSnapshot = true) {
        if (needsSnapshot && (!snapshot.bytes || snapshot.bytes->empty())) throw Error(ErrorCode::InvalidDocument, "Empty rendering snapshot.");
        if (!process_) process_ = detail::Process::spawn(helper_, {options_.memoryLimitBytes});
        const Snapshot* load = nullptr;
        if (needsSnapshot && (loaded_ != snapshot.bytes || loadedRevision_ != snapshot.revision || loadedPages_ != snapshot.pages)) load = &snapshot;
        std::optional<std::vector<unsigned char>> reply; bool expired = false;
        try {
            Watchdog watchdog(*process_, load ? std::max(timeout, options_.validateTimeout) : timeout);
            try { reply = exchange(op, payload, load); }
            catch (...) { watchdog.stop(); throw; }
            expired = watchdog.stop();
        } catch (const WorkerError& error) {
            loaded_.reset(); // The helper's copy may be stale after a rejected load.
            throw Error(error.code, error.message);
        }
        if (expired) { reset(); throw Error(ErrorCode::RenderWorkerFailed, "The PDF took too long to process. The rendering worker was stopped."); }
        if (!reply) {
            reset();
            throw Error(ErrorCode::RenderWorkerFailed, "The rendering worker stopped unexpectedly. The document is unchanged; try the operation again.");
        }
        if (load) { loaded_ = snapshot.bytes; loadedRevision_ = snapshot.revision; loadedPages_ = snapshot.pages; }
        return std::move(*reply);
    }
};
}
std::filesystem::path RenderService::helperName() {
#ifdef _WIN32
    return "pdfeditor-render.exe";
#else
    return "pdfeditor-render";
#endif
}
std::shared_ptr<RenderService> RenderService::isolated(const std::filesystem::path& helper, Options options) { return std::make_shared<IsolatedService>(helper, options); }
std::shared_ptr<RenderService> RenderService::inProcess() { return std::make_shared<InProcessService>(); }
std::shared_ptr<RenderService> RenderService::forApplication(const std::filesystem::path& directory) {
    if (std::getenv("FOLIOFORGE_RENDER_INPROCESS")) return inProcess();
    return isolated(directory / helperName());
}
namespace testing {
void runWorkerOperation(RenderService& service, unsigned op, const std::string& argument, std::chrono::milliseconds timeout) {
    static_cast<IsolatedService&>(service).testOperation(static_cast<protocol::Op>(op), argument, timeout);
}
}
}
