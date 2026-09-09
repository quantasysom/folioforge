#include "pdfengine/document.h"
#include <fstream>
#include <random>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pdfengine {
Bytes readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw Error(ErrorCode::InvalidDocument, "The file could not be opened.");
    auto size = stream.tellg();
    if (size < 0 || size > 256ll * 1024 * 1024) throw Error(ErrorCode::ResourceLimit, "This preview supports files up to 256 MiB.");
    Bytes bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) throw Error(ErrorCode::InvalidDocument, "The complete file could not be read.");
    return bytes;
}
void atomicWrite(const std::filesystem::path& path, const Bytes& bytes, bool overwrite, const Bytes* expected) {
    if (expected && (!std::filesystem::exists(path) || readFile(path) != *expected))
        throw Error(ErrorCode::ExternalModification, "The file changed outside FolioForge. Use Save As to preserve both versions.");
    if (!overwrite && std::filesystem::exists(path)) throw Error(ErrorCode::SaveFailed, "The output already exists. Choose another name.");
    auto temp = path;
    temp += ".folio-" + std::to_string(std::random_device{}()) + ".tmp";
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } };
#ifdef _WIN32
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw Error(ErrorCode::SaveFailed, "Cannot create a temporary file beside the destination.");
    Cleanup cleanup{temp};
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    ok = FlushFileBuffers(file) && ok;
    ok = CloseHandle(file) && ok;
#else
    int file = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) throw Error(ErrorCode::SaveFailed, "Cannot create a temporary file beside the destination.");
    Cleanup cleanup{temp};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto written = ::write(file, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
    bool ok = offset == bytes.size();
    ok = (::fsync(file) == 0) && ok;
    ok = (::close(file) == 0) && ok;
#endif
    if (!ok || readFile(temp) != bytes) throw Error(ErrorCode::SaveFailed, "Writing or verifying the temporary PDF failed. The destination was not changed.");
    if (expected && (!std::filesystem::exists(path) || readFile(path) != *expected))
        throw Error(ErrorCode::ExternalModification, "The destination changed during saving. Use Save As.");
#ifdef _WIN32
    DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temp.c_str(), path.c_str(), flags))
        throw Error(ErrorCode::SaveFailed, "The destination could not be replaced. It may be open in another application.");
#else
    if (overwrite) {
        if (::rename(temp.c_str(), path.c_str()) != 0) throw Error(ErrorCode::SaveFailed, "The destination could not be replaced.");
    } else {
        if (::link(temp.c_str(), path.c_str()) != 0) throw Error(ErrorCode::SaveFailed, "The destination could not be created.");
    }
    int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0) { ::fsync(directory); ::close(directory); }
#endif
}
}
