#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace pdfengine::detail {
// A child process with blocking pipes on its stdin/stdout. kill() may be called from another
// thread to unblock a pending read/write, after which both fail.
class Process {
public:
    struct Limits { std::uint64_t memoryBytes{}; };
    static std::unique_ptr<Process> spawn(const std::filesystem::path& executable, const Limits& limits);
    virtual ~Process() = default;
    virtual bool write(const void* data, std::size_t size) = 0;
    virtual bool read(void* data, std::size_t size) = 0;
    virtual void kill() = 0;
};
}
