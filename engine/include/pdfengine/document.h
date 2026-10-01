#pragma once
#include <array>
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

enum class AnnotationKind { Highlight, Underline, StrikeOut, Note, FreeText, Ink, Rectangle, Ellipse, Other };
struct Point { double x{}, y{}; };
// Coordinates are PDF user-space points (origin bottom-left, y up) of the unrotated page.
struct Annotation {
    std::uint32_t index{};      // Position in the page's /Annots; valid for the revision it was listed at.
    AnnotationKind kind{AnnotationKind::Other};
    double x0{}, y0{}, x1{}, y1{};
    std::array<double, 3> color{};
    std::string contents;
    bool removable{};           // True for annotations created by FolioForge; others are preserved untouched (and cannot be edited).
    std::vector<std::vector<Point>> strokes;  // Ink only.
    double lineWidth{1.5}, fontSize{12};
};
struct AddAnnotation {
    PageId page; RevisionId expectedRevision;
    AnnotationKind kind{AnnotationKind::Highlight};
    double x0{}, y0{}, x1{}, y1{};                    // Bounding rectangle (any corner order).
    std::array<double, 3> color{1, 0.9, 0};           // RGB 0..1.
    std::string contents;                             // Note text or FreeText body (printable ASCII/Latin-1, \n allowed).
    std::vector<std::vector<Point>> strokes;          // Ink only.
    double lineWidth{1.5}, fontSize{12};
};

struct SignOptions {
    std::filesystem::path certificate;   // PKCS#12 (.p12/.pfx) containing the private key and certificate chain.
    std::string password, reason, location, signerName;
};
struct SignatureCheck { bool intact{}; bool coversWholeFile{}; std::string signer; };
// Checks every signature in a PDF: intact means the signed bytes are unmodified (trust in the certificate is not evaluated).
std::vector<SignatureCheck> verifySignatures(const Bytes& pdf);

enum class FormFieldKind { Text, Checkbox, Radio, Choice, Button, Signature };
// One widget of an AcroForm field on a page. Coordinates are PDF points like Annotation.
struct FormField {
    std::uint32_t widget{};          // Position in the page's /Annots; valid for the revision it was listed at.
    FormFieldKind kind{FormFieldKind::Text};
    std::string name;                // Fully qualified field name.
    std::string value;               // Text/choice value (export value for choices).
    bool checked{};                  // Checkbox/radio: this widget is on.
    std::vector<std::string> options, optionValues; // Choice display strings and export values.
    bool readOnly{};                 // Not editable (read-only flag, signature, push button, or unsupported variant).
    bool multiline{}, password{}, combo{}, editable{};
    int maxLength{};
    double x0{}, y0{}, x1{}, y1{};
};
struct SetFormValue {
    PageId page; RevisionId expectedRevision; std::uint32_t widget{};
    std::string text;                // Text fields and choices (UTF-8).
    bool checked{};                  // Checkboxes and radio buttons.
};

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

// Parses "1-3,7,10-" (one-based, "-N" and "N-" open ends) into zero-based indexes; throws InvalidSelection.
std::vector<std::size_t> parsePageRanges(const std::string& text, std::size_t pageCount);

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
    // `ranges` selects source pages in the order written ("1-3,7,10-"; empty means every page).
    void insertDocument(const std::filesystem::path&, PageId after, RevisionId expected, const std::string& ranges = {});
    void insertImage(const ImagePage&, PageId after, RevisionId expected);
    TextInventory textRuns(PageId) const;
    void replaceText(const ReplaceText&);
    std::vector<Annotation> annotations(PageId) const;
    std::vector<FormField> formFields(PageId) const;
    // Fills one field. Appearance streams are regenerated so every viewer shows the value. Scripts and
    // calculations are never run.
    void setFormValue(const SetFormValue&);
    void addAnnotation(const AddAnnotation&);
    void removeAnnotation(PageId, std::uint32_t index, RevisionId expected);
    // Rewrites a FolioForge-created annotation in place (move, resize, recolor, new text) keeping its identity and z-order.
    void updateAnnotation(std::uint32_t index, const AddAnnotation& replacement);
    // TrueType (.ttf) fonts tried, in order, before installed system fonts when an edit needs characters the
    // run's own font lacks. Throws Error(Unsupported) if a file cannot be embedded.
    void setFallbackFonts(const std::vector<std::filesystem::path>&);
    // Writes a signed copy (invisible signature, incremental update) to a new file. The open document is unchanged.
    void signTo(const std::filesystem::path& output, const SignOptions&, bool overwrite = false);
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
