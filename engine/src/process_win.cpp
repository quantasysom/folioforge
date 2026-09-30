#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "process.h"
#include "pdfengine/document.h"
#include <windows.h>
#include <atomic>
#include <string>

namespace pdfengine::detail {
namespace {
struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default; Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
    HANDLE release() { auto h = value; value = nullptr; return h; }
};
class WinProcess : public Process {
public:
    WinProcess(HANDLE process, HANDLE job, HANDLE in, HANDLE out) : process_(process), job_(job), in_(in), out_(out) {}
    ~WinProcess() override {
        kill();
        CloseHandle(in_); CloseHandle(out_);
        WaitForSingleObject(process_, 5000);
        CloseHandle(process_); CloseHandle(job_); // The job also kills the process if it is still alive.
    }
    bool write(const void* data, std::size_t size) override {
        auto p = static_cast<const char*>(data);
        while (size) {
            DWORD written = 0;
            if (!WriteFile(in_, p, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20)), &written, nullptr) || !written) return false;
            p += written; size -= written;
        }
        return true;
    }
    bool read(void* data, std::size_t size) override {
        auto p = static_cast<char*>(data);
        while (size) {
            DWORD count = 0;
            if (!ReadFile(out_, p, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20)), &count, nullptr) || !count) return false;
            p += count; size -= count;
        }
        return true;
    }
    void kill() override { if (!killed_.exchange(true)) TerminateProcess(process_, 1); }
private:
    HANDLE process_, job_, in_, out_; std::atomic<bool> killed_{};
};
}
std::unique_ptr<Process> Process::spawn(const std::filesystem::path& executable, const Limits& limits) {
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    Handle childIn, parentIn, parentOut, childOut;
    if (!CreatePipe(&childIn.value, &parentIn.value, &inheritable, 0) || !CreatePipe(&parentOut.value, &childOut.value, &inheritable, 0))
        throw Error(ErrorCode::RenderWorkerFailed, "Could not create the rendering worker pipes.");
    SetHandleInformation(parentIn.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentOut.value, HANDLE_FLAG_INHERIT, 0);
    Handle nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childIn.value; startup.hStdOutput = childOut.value; startup.hStdError = nul.value;
    std::wstring command = L"\"" + executable.wstring() + L"\" --memory-limit=" + std::to_wstring(limits.memoryBytes);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        throw Error(ErrorCode::RenderWorkerFailed, "The rendering worker could not be started: " + executable.string());
    Handle process = info.hProcess, thread = info.hThread;
    Handle job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limit.BasicLimitInformation.ActiveProcessLimit = 1;
    if (limits.memoryBytes) { limit.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY; limit.ProcessMemoryLimit = static_cast<SIZE_T>(limits.memoryBytes); }
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{}; ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_ALL;
    bool restricted = job.value && SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limit, sizeof(limit)) &&
        SetInformationJobObject(job.value, JobObjectBasicUIRestrictions, &ui, sizeof(ui)) && AssignProcessToJobObject(job.value, process.value);
    if (!restricted) { TerminateProcess(process.value, 1); throw Error(ErrorCode::RenderWorkerFailed, "The rendering worker could not be restricted."); }
    ResumeThread(thread.value);
    return std::make_unique<WinProcess>(process.release(), job.release(), parentIn.release(), parentOut.release());
}
}
