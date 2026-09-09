#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace pdfengine {
using PageId = std::uint64_t;
using RevisionId = std::uint64_t;
using Bytes = std::vector<unsigned char>;
enum class ErrorCode { InvalidDocument, PasswordRequired, Unsupported, StaleRevision,
                       InvalidSelection, ResourceLimit, SaveFailed, ExternalModification, TextOverflow };
class Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code(code) {}
    ErrorCode code;
};
struct PageInfo { PageId id; double width; double height; int rotation; };
struct Snapshot {
    std::shared_ptr<const Bytes> bytes;
    RevisionId revision{};
    std::vector<PageId> pages;
};
struct DocumentInfo {
    std::vector<PageInfo> pages;
    RevisionId revision{};
    bool dirty{}, canUndo{}, canRedo{}, editable{};
    std::string restriction;
    std::filesystem::path path;
    bool historyPruned{};
};
enum class CommandKind { RotateLeft, RotateRight, InsertBlank, Delete, Duplicate, MoveEarlier, MoveLater };
struct Command { CommandKind kind; PageId page; RevisionId expectedRevision; };
// Source-bound text ID, valid only for this page/revision. Coordinates are PDF points.
struct TextRun {
    std::uint64_t id{};
    PageId page{};
    RevisionId revision{};
    std::string text, font;
    double x{}, baseline{}, width{}, fontSize{};
};
struct TextInventory { std::vector<TextRun> runs; std::string explanation; };
struct ReplaceText { PageId page; std::uint64_t run; RevisionId expectedRevision; std::string text; };

// A session has one owner. Callers serialize access; immutable snapshots may cross threads.
class Document {
public:
    static std::shared_ptr<Document> create();
    static std::shared_ptr<Document> open(const std::filesystem::path&, const std::string& password = {});
    DocumentInfo info() const;
    Snapshot snapshot() const;
    void execute(const Command&);
    void undo(RevisionId expected);
    void redo(RevisionId expected);
    void insertDocument(const std::filesystem::path&, PageId after, RevisionId expected);
    TextInventory textRuns(PageId) const;
    void replaceText(const ReplaceText&);
    // Call renderer validation on snapshot() before saving. Saving revalidates QPDF structure.
    void save(const std::filesystem::path&, bool overwrite = false);
private:
    struct State { std::shared_ptr<const Bytes> bytes; std::vector<PageInfo> pages; std::uint64_t identity; };
    State current_;
    std::vector<State> undo_, redo_;
    RevisionId revision_{1};
    PageId nextPage_{1};
    std::uint64_t nextIdentity_{1}, savedIdentity_{};
    bool editable_{true}, historyPruned_{};
    std::string restriction_;
    std::filesystem::path path_;
    std::shared_ptr<const Bytes> sourceBytes_;
    void checkRevision(RevisionId) const;
    void commit(State);
};

Bytes readFile(const std::filesystem::path&);
// Unique sibling temp file, durable write, atomic replacement. expected is an exact conflict check.
void atomicWrite(const std::filesystem::path&, const Bytes&, bool overwrite,
                 const Bytes* expected = nullptr);
}
