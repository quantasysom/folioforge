#include "process.h"
#include "pdfengine/document.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace pdfengine::detail {
namespace {
class PosixProcess : public Process {
public:
    PosixProcess(pid_t pid, int toChild, int fromChild) : pid_(pid), in_(toChild), out_(fromChild) {}
    ~PosixProcess() override {
        kill();
        close(in_); close(out_);
        int status; while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    }
    bool write(const void* data, std::size_t size) override {
        auto p = static_cast<const char*>(data);
        while (size) {
            auto n = ::write(in_, p, size);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            p += n; size -= static_cast<std::size_t>(n);
        }
        return true;
    }
    bool read(void* data, std::size_t size) override {
        auto p = static_cast<char*>(data);
        while (size) {
            auto n = ::read(out_, p, size);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            p += n; size -= static_cast<std::size_t>(n);
        }
        return true;
    }
    void kill() override { if (!killed_.exchange(true)) ::kill(pid_, SIGKILL); }
private:
    pid_t pid_; int in_, out_; std::atomic<bool> killed_{};
};
void cloexec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }
}
std::unique_ptr<Process> Process::spawn(const std::filesystem::path& executable, const Limits& limits) {
    static std::once_flag once;
    std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });
    int toChild[2], fromChild[2];
    if (pipe(toChild) != 0) throw Error(ErrorCode::RenderWorkerFailed, "Could not create the rendering worker pipes.");
    if (pipe(fromChild) != 0) { close(toChild[0]); close(toChild[1]); throw Error(ErrorCode::RenderWorkerFailed, "Could not create the rendering worker pipes."); }
    for (int fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]}) cloexec(fd);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawnattr_t attributes; posix_spawnattr_init(&attributes);
#ifdef __APPLE__
    // Children inherit only the descriptors named below, never stray application files.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
#endif
    posix_spawn_file_actions_adddup2(&actions, toChild[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fromChild[1], STDOUT_FILENO);
#if defined(__linux__) && defined(__GLIBC__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 34)
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
    std::string path = executable.string();
    std::string memory = "--memory-limit=" + std::to_string(limits.memoryBytes);
    char* argv[] = {path.data(), memory.data(), nullptr};
    pid_t pid = 0;
    int result = posix_spawn(&pid, path.c_str(), &actions, &attributes, argv, environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    close(toChild[0]); close(fromChild[1]);
    if (result != 0) {
        close(toChild[1]); close(fromChild[0]);
        throw Error(ErrorCode::RenderWorkerFailed, "The rendering worker could not be started: " + path);
    }
    return std::make_unique<PosixProcess>(pid, toChild[1], fromChild[0]);
}
}
