#pragma once
// Wire format between the application and the pdfeditor-render helper (same machine, same
// endianness). Every message is a header {magic, op-or-status, payload length} plus payload.
#include "pdfengine/document.h"
#include <cstring>

namespace pdfengine::protocol {
constexpr std::uint32_t requestMagic = 0x51524646, responseMagic = 0x53524646; // "FFRQ" / "FFRS"
constexpr std::uint64_t maxPayload = 2ull << 30;
enum Op : std::uint32_t { Load = 1, Render = 2, Text = 3, Validate = 4, Quit = 5,
                          TestHang = 100, TestCrash = 101, TestWrite = 102 }; // Test ops need FOLIOFORGE_RENDER_TEST_HOOKS=1.
struct Header { std::uint32_t magic{}, code{}; std::uint64_t length{}; };

class Writer {
public:
    std::vector<unsigned char> bytes;
    template<class T> void put(T value) { auto p = reinterpret_cast<const unsigned char*>(&value); bytes.insert(bytes.end(), p, p + sizeof(T)); }
    void put(const void* data, std::size_t size) { auto p = static_cast<const unsigned char*>(data); bytes.insert(bytes.end(), p, p + size); }
};
class Reader {
public:
    Reader(const unsigned char* data, std::size_t size) : data_(data), size_(size) {}
    template<class T> T get() { T value; std::memcpy(&value, take(sizeof(T)), sizeof(T)); return value; }
    const unsigned char* take(std::size_t count) {
        if (count > size_ - pos_) throw Error(ErrorCode::RenderWorkerFailed, "Malformed message from the rendering worker.");
        auto result = data_ + pos_; pos_ += count; return result;
    }
    std::size_t remaining() const { return size_ - pos_; }
private:
    const unsigned char* data_; std::size_t size_, pos_{};
};
}
