#include "pdfengine/document.h"
#include <iostream>

using namespace pdfengine;
namespace {
int checks{};
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
std::size_t spillFiles() {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::temp_directory_path()))
        if (entry.path().filename().string().starts_with("folioforge-history-")) ++count;
    return count;
}
}
int main() {
    try {
        const auto before = spillFiles();
        {
            auto doc = Document::create();
            doc->setHistoryLimits({1, 1ull << 30, 200}); // Force every older snapshot onto disk.
            std::vector<std::shared_ptr<const Bytes>> states{doc->snapshot().bytes};
            for (int i = 0; i < 12; ++i) {
                doc->execute({CommandKind::Duplicate, doc->info().pages.front().id, doc->info().revision});
                states.push_back(doc->snapshot().bytes);
            }
            require(doc->info().pages.size() == 13 && spillFiles() == before + 1, "History should spill to one temporary directory");
            for (int i = 12; i > 0; --i) {
                doc->undo(doc->info().revision);
                require(*doc->snapshot().bytes == *states[i - 1], "Undo from disk restored different bytes");
            }
            require(!doc->info().canUndo && !doc->info().historyPruned && doc->info().pages.size() == 1, "Fully undone document should have no history left and nothing pruned");
            for (int i = 0; i < 12; ++i) doc->redo(doc->info().revision);
            require(*doc->snapshot().bytes == *states.back() && doc->info().pages.size() == 13, "Redo from disk failed");

            doc->setHistoryLimits({1, 1ull << 30, 3});
            require(doc->info().historyPruned, "Entry limit should prune and report it");
            int undone = 0; while (doc->info().canUndo) { doc->undo(doc->info().revision); ++undone; }
            require(undone == 3, "Entry limit not enforced");

            doc->setHistoryLimits({1, 1, 200}); // Disk budget below one snapshot prunes everything spilled.
            doc->execute({CommandKind::Duplicate, doc->info().pages.front().id, doc->info().revision});
            doc->execute({CommandKind::Duplicate, doc->info().pages.front().id, doc->info().revision});
            require(!doc->info().canUndo || doc->info().historyPruned, "Disk budget should prune history");
        }
        require(spillFiles() == before, "Temporary history was not cleaned up");
        std::cout << checks << " history checks passed: disk spill, exact restore, entry/disk limits, cleanup\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
