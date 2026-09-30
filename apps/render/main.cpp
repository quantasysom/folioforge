// pdfeditor-render: restricted helper that runs PDFium for the application (see ADR-002).
// Speaks the protocol in engine/src/render_protocol.h over stdin/stdout.
#include "pdfengine/renderer.h"
#include "render_protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sandbox.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

using namespace pdfengine;
namespace {
struct Channel {
#ifdef _WIN32
    HANDLE in{GetStdHandle(STD_INPUT_HANDLE)}, out{GetStdHandle(STD_OUTPUT_HANDLE)};
    bool read(void* data, std::size_t size) {
        auto p = static_cast<char*>(data);
        while (size) { DWORD n = 0; if (!ReadFile(in, p, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20)), &n, nullptr) || !n) return false; p += n; size -= n; }
        return true;
    }
    bool write(const void* data, std::size_t size) {
        auto p = static_cast<const char*>(data);
        while (size) { DWORD n = 0; if (!WriteFile(out, p, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20)), &n, nullptr) || !n) return false; p += n; size -= n; }
        return true;
    }
#else
    int in{STDIN_FILENO}, out{-1};
    Channel() {
        // Keep the protocol on a private descriptor so stray library output cannot corrupt it.
        out = dup(STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
    }
    bool read(void* data, std::size_t size) {
        auto p = static_cast<char*>(data);
        while (size) { auto n = ::read(in, p, size); if (n < 0 && errno == EINTR) continue; if (n <= 0) return false; p += n; size -= static_cast<std::size_t>(n); }
        return true;
    }
    bool write(const void* data, std::size_t size) {
        auto p = static_cast<const char*>(data);
        while (size) { auto n = ::write(out, p, size); if (n < 0 && errno == EINTR) continue; if (n <= 0) return false; p += n; size -= static_cast<std::size_t>(n); }
        return true;
    }
#endif
    bool respond(std::uint32_t code, const void* data, std::size_t size) {
        protocol::Header header{protocol::responseMagic, code, size};
        return write(&header, sizeof header) && (!size || write(data, size));
    }
};

// Drops what a renderer never needs: writes, new processes, network, core dumps, runaway memory.
void restrictProcess(std::uint64_t memoryLimit) {
#ifndef _WIN32
    rlimit none{0, 0}; setrlimit(RLIMIT_CORE, &none);
    rlimit files{256, 256}; setrlimit(RLIMIT_NOFILE, &files);
    rlimit cpu{3600, 3600}; setrlimit(RLIMIT_CPU, &cpu);
#ifdef __linux__
    if (memoryLimit) { rlimit memory{memoryLimit, memoryLimit}; setrlimit(RLIMIT_AS, &memory); }
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
#ifdef __APPLE__
    (void)memoryLimit;
    const char* profile =
        "(version 1)(deny default)"
        "(allow file-read*)(allow sysctl-read)(allow mach-lookup)(allow ipc-posix-shm-read*)"
        "(allow signal (target self))(allow process-info* (target self))";
    char* error = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const int sandboxed = sandbox_init(profile, 0, &error);
#pragma clang diagnostic pop
    if (sandboxed != 0) { std::cerr << "sandbox: " << (error ? error : "failed") << '\n'; std::exit(3); }
#endif
#else
    (void)memoryLimit; // The parent's job object enforces memory and process limits.
#endif
}
}

int main(int argc, char** argv) {
    std::uint64_t memoryLimit = 0;
    for (int i = 1; i < argc; ++i) if (std::strncmp(argv[i], "--memory-limit=", 15) == 0) memoryLimit = std::strtoull(argv[i] + 15, nullptr, 10);
    const bool hooks = std::getenv("FOLIOFORGE_RENDER_TEST_HOOKS") != nullptr;
    Channel channel;
    restrictProcess(memoryLimit);
    Snapshot snapshot;
    for (;;) {
        protocol::Header header;
        if (!channel.read(&header, sizeof header) || header.magic != protocol::requestMagic || header.length > protocol::maxPayload) return 1;
        std::vector<unsigned char> payload;
        try { payload.resize(static_cast<std::size_t>(header.length)); }
        catch (const std::bad_alloc&) { return 1; }
        if (!payload.empty() && !channel.read(payload.data(), payload.size())) return 1;
        try {
            protocol::Reader in(payload.data(), payload.size());
            protocol::Writer out;
            switch (header.code) {
            case protocol::Load: {
                Snapshot loaded; loaded.revision = in.get<std::uint64_t>();
                auto pages = in.get<std::uint64_t>();
                if (pages > 100000) throw Error(ErrorCode::InvalidDocument, "Too many pages.");
                for (std::uint64_t i = 0; i < pages; ++i) loaded.pages.push_back(in.get<std::uint64_t>());
                auto size = in.get<std::uint64_t>();
                if (size != in.remaining()) throw Error(ErrorCode::InvalidDocument, "Snapshot size mismatch.");
                auto data = in.take(static_cast<std::size_t>(size));
                loaded.bytes = std::make_shared<const Bytes>(data, data + size);
                snapshot = std::move(loaded);
                break;
            }
            case protocol::Render: {
                auto page = in.get<std::uint64_t>(); auto scale = in.get<double>();
                auto bitmap = Renderer::render(snapshot, static_cast<std::size_t>(page), scale);
                out.put<std::int32_t>(bitmap.width); out.put<std::int32_t>(bitmap.height); out.put<std::int32_t>(bitmap.stride);
                out.put<std::uint64_t>(bitmap.revision); out.put<std::uint64_t>(bitmap.page);
                out.put<std::uint64_t>(bitmap.bgra.size()); out.put(bitmap.bgra.data(), bitmap.bgra.size());
                break;
            }
            case protocol::Text: {
                auto text = Renderer::text(snapshot, static_cast<std::size_t>(in.get<std::uint64_t>()));
                out.put(text.data(), text.size() * sizeof(char16_t));
                break;
            }
            case protocol::Validate: Renderer::validate(snapshot); break;
            case protocol::Quit: return 0;
            case protocol::TestHang:
                if (!hooks) return 1;
                for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
            case protocol::TestWrite: {
                if (!hooks) return 1;
                std::string path(reinterpret_cast<const char*>(payload.data()), payload.size());
                if (FILE* file = std::fopen(path.c_str(), "w")) { std::fclose(file); std::remove(path.c_str()); }
                else throw Error(ErrorCode::Unsupported, "write denied");
                break;
            }
            case protocol::TestCrash:
                if (!hooks) return 1;
                std::abort();
            default: return 1;
            }
            if (!channel.respond(0, out.bytes.data(), out.bytes.size())) return 1;
        } catch (const Error& e) {
            std::string message = e.what();
            if (!channel.respond(static_cast<std::uint32_t>(e.code) + 1, message.data(), message.size())) return 1;
        } catch (const std::bad_alloc&) {
            std::string message = "The rendering worker ran out of memory.";
            if (!channel.respond(static_cast<std::uint32_t>(ErrorCode::ResourceLimit) + 1, message.data(), message.size())) return 1;
        } catch (const std::exception&) { return 1; }
    }
}
