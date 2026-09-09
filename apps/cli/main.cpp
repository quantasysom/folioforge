#include "pdfengine/document.h"
#include "pdfengine/renderer.h"
#include <iostream>
#include <cstring>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace pdfengine;
namespace {
std::filesystem::path utf8Path(const char* text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text), std::strlen(text)));
}
}
int runCli(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cout << "FolioForge 0.1 preview\n"
                         "  pdfeditor-cli new OUTPUT.pdf\n"
                         "  pdfeditor-cli inspect INPUT.pdf\n"
                         "  pdfeditor-cli text INPUT.pdf\n"
                         "  pdfeditor-cli rotate INPUT.pdf PAGE OUTPUT.pdf\n"
                         "  pdfeditor-cli merge FIRST.pdf SECOND.pdf OUTPUT.pdf\n"
                         "Outputs must not already exist. PAGE is one-based.\n";
            return 0;
        }
        std::string operation = argv[1];
        if (operation == "new" && argc == 3) {
            auto doc = Document::create(); Renderer::validate(doc->snapshot()); doc->save(utf8Path(argv[2]));
        } else if ((operation == "inspect" || operation == "text") && argc == 3) {
            auto doc = Document::open(utf8Path(argv[2]));
            auto info = doc->info();
            if (operation == "inspect") {
                std::cout << info.pages.size() << " pages; " << (info.editable ? "page editing available" : info.restriction) << '\n';
                for (std::size_t i = 0; i < info.pages.size(); ++i)
                    std::cout << i + 1 << ": " << info.pages[i].width << " x " << info.pages[i].height << " pt, rotation " << info.pages[i].rotation << '\n';
            } else {
                // UTF-8 output without Qt; preserve Unicode supplementary characters.
                for (std::size_t page = 0; page < info.pages.size(); ++page) {
                    auto text = Renderer::text(doc->snapshot(), page);
                    for (std::size_t i = 0; i < text.size(); ++i) {
                        std::uint32_t c = text[i];
                        if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
                            c = 0x10000 + ((c - 0xd800) << 10) + (text[++i] - 0xdc00);
                        else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
                        if (c < 0x80) std::cout.put(static_cast<char>(c));
                        else if (c < 0x800) { std::cout.put(static_cast<char>(0xc0 | (c >> 6))); std::cout.put(static_cast<char>(0x80 | (c & 63))); }
                        else if (c < 0x10000) { std::cout.put(static_cast<char>(0xe0 | (c >> 12))); std::cout.put(static_cast<char>(0x80 | ((c >> 6) & 63))); std::cout.put(static_cast<char>(0x80 | (c & 63))); }
                        else { std::cout.put(static_cast<char>(0xf0 | (c >> 18))); std::cout.put(static_cast<char>(0x80 | ((c >> 12) & 63))); std::cout.put(static_cast<char>(0x80 | ((c >> 6) & 63))); std::cout.put(static_cast<char>(0x80 | (c & 63))); }
                    }
                    std::cout << "\n\f\n";
                }
            }
        } else if (operation == "rotate" && argc == 5) {
            auto doc = Document::open(utf8Path(argv[2]));
            std::size_t consumed = 0; int page = std::stoi(argv[3], &consumed);
            if (consumed != std::string(argv[3]).size() || page < 1 || page > static_cast<int>(doc->info().pages.size()))
                throw std::runtime_error("PAGE must be a valid one-based page number.");
            doc->execute({CommandKind::RotateRight, doc->info().pages[page - 1].id, doc->info().revision});
            Renderer::validate(doc->snapshot());
            if (std::filesystem::exists(utf8Path(argv[4]))) throw std::runtime_error("Output already exists.");
            doc->save(utf8Path(argv[4]));
        } else if (operation == "merge" && argc == 5) {
            auto doc = Document::open(utf8Path(argv[2]));
            doc->insertDocument(utf8Path(argv[3]), doc->info().pages.back().id, doc->info().revision);
            Renderer::validate(doc->snapshot());
            if (std::filesystem::exists(utf8Path(argv[4]))) throw std::runtime_error("Output already exists.");
            doc->save(utf8Path(argv[4]));
        } else throw std::runtime_error("Invalid arguments. Run pdfeditor-cli without arguments for usage.");
        return 0;
    } catch (const std::exception& e) { std::cerr << "FolioForge: " << e.what() << '\n'; return 1; }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wideArgs) {
    std::vector<std::string> args; args.reserve(argc);
    std::vector<char*> pointers; pointers.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, 0, wideArgs[i], -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 1;
        std::string arg(size, '\0');
        if (!WideCharToMultiByte(CP_UTF8, 0, wideArgs[i], -1, arg.data(), size, nullptr, nullptr)) return 1;
        arg.pop_back(); args.push_back(std::move(arg)); pointers.push_back(args.back().data());
    }
    return runCli(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return runCli(argc, argv); }
#endif
