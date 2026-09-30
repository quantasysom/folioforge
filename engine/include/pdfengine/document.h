#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace pdfengine {
namespace font { class FontSource; }
using PageId = std::uint64_t;
using RevisionId = std::uint64_t;
using Bytes = std::vector<unsigned char>;
inline constexpr std::uint64_t maxDocumentBytes = 1ull << 30; // Largest PDF opened or produced.
enum class ErrorCode { InvalidDocument, PasswordRequired, Unsupported, StaleRevision,
                       InvalidSelection, ResourceLimit, SaveFailed, ExternalModification, TextOverflow, RenderWorkerFailed };
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

// One raster image that becomes a page. `data` is either a complete JPEG file (jpeg == true) or
// width*height*components interleaved 8-bit samples (1 = gray, 3 = RGB). `alpha` is optional
// width*height 8-bit opacity. `dpi` <= 0 selects the 96 DPI default.
struct ImagePage {
    std::uint32_t width{}, height{};
    int components{3};
    bool jpeg{};
    Bytes data, alpha;
    double dpi{};
};
// Validates a baseline/progressive 8-bit gray or RGB JPEG and wraps it without recompression.
ImagePage jpegImage(const Bytes& file, double dpi = 0);

// Undo/redo history keeps recent snapshots in memory and moves older ones to temporary files, so
// history depth is bounded by disk rather than RAM. Entries beyond either budget are pruned.
struct HistoryLimits {
    std::size_t memoryBytes = 256ull << 20;
    std::uint64_t diskBytes = 4ull << 30;
    std::size_t entries = 200;
};

// A session has one owner. Callers serialize access; immutable snapshots may cross threads.
class Document {
public:
    ~Document();
    void setHistoryLimits(const HistoryLimits&);
    static std::shared_ptr<Document> create();
    static std::shared_ptr<Document> createFromImage(const ImagePage&);
    static std::shared_ptr<Document> open(const std::filesystem::path&, const std::string& password = {});
    DocumentInfo info() const;
    Snapshot snapshot() const;
    void execute(const Command&);
    void undo(RevisionId expected);
    void redo(RevisionId expected);
    void insertDocument(const std::filesystem::path&, PageId after, RevisionId expected);
    void insertImage(const ImagePage&, PageId after, RevisionId expected);
    TextInventory textRuns(PageId) const;
    void replaceText(const ReplaceText&);
    // TrueType (.ttf) fonts tried, in order, before installed system fonts when an edit needs characters the
    // run's own font lacks. Throws Error(Unsupported) if a file cannot be embedded.
    void setFallbackFonts(const std::vector<std::filesystem::path>&);
    // Call renderer validation on snapshot() before saving. Saving revalidates QPDF structure.
    void save(const std::filesystem::path&, bool overwrite = false);
private:
    struct SpillFile;
    struct State {
        std::shared_ptr<const Bytes> bytes; // Null while the snapshot lives only in its spill file.
        std::vector<PageInfo> pages;
        std::uint64_t identity;
        std::shared_ptr<SpillFile> spill;
        std::uint64_t spillSize{};
    };
    std::shared_ptr<font::FontSource> fonts_;
    HistoryLimits limits_;
    std::filesystem::path spillDirectory_;
    std::uint64_t spillCounter_{};
    bool spill(State&);
    void materialize(State&) const;
    void enforceHistoryBudget();
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
