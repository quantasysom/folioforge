#include "window.h"
#include "text_canvas.h"
#include "merge_dialog.h"
#include "office_convert.h"
#include "pdfengine/render_service.h"
#include <QtWidgets>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace pdfengine;
namespace {
std::filesystem::path localPath(const QString& value) {
#ifdef _WIN32
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::u8path(value.toUtf8().constData());
#endif
}
QString displayPath(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromUtf8(path.string().c_str());
#endif
}
bool isImagePath(const QString& path) {
    for (const char* suffix : {".png", ".jpg", ".jpeg"}) if (path.endsWith(suffix, Qt::CaseInsensitive)) return true;
    return false;
}
// Keeps JPEGs byte-for-byte when possible; everything else is decoded to gray/RGB (+ alpha) samples.
ImagePage loadImage(const QString& path) {
    QImageReader reader(path); reader.setAutoTransform(true);
    if (!reader.canRead()) throw Error(ErrorCode::InvalidDocument, QString("%1 is not a readable image.").arg(QFileInfo(path).fileName()).toStdString());
    const auto size = reader.size();
    if (size.isValid() && static_cast<qint64>(size.width()) * size.height() > 100'000'000)
        throw Error(ErrorCode::ResourceLimit, "Images larger than 100 megapixels are not supported.");
    if (reader.format().toLower() == "jpeg" && reader.transformation() == QImageIOHandler::TransformationNone) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && static_cast<std::uint64_t>(file.size()) <= maxDocumentBytes) {
            auto data = file.readAll();
            try { return jpegImage(Bytes(data.begin(), data.end())); }
            catch (const Error& e) { if (e.code != ErrorCode::Unsupported) throw; }
        }
    }
    QImage image = reader.read();
    if (image.isNull()) throw Error(ErrorCode::InvalidDocument, "The image could not be decoded.");
    ImagePage page; page.width = image.width(); page.height = image.height();
    double dpi = image.dotsPerMeterX() * 0.0254;
    page.dpi = dpi > 72.5 ? dpi : 0;
    const bool alpha = image.hasAlphaChannel();
    const bool gray = !alpha && image.isGrayscale();
    image = image.convertToFormat(alpha ? QImage::Format_RGBA8888 : gray ? QImage::Format_Grayscale8 : QImage::Format_RGB888);
    const int channels = alpha ? 4 : gray ? 1 : 3, components = gray ? 1 : 3;
    page.components = components;
    page.data.resize(static_cast<std::size_t>(page.width) * page.height * components);
    if (alpha) page.alpha.resize(static_cast<std::size_t>(page.width) * page.height);
    bool translucent = false;
    for (int y = 0; y < image.height(); ++y) {
        const uchar* row = image.constScanLine(y);
        auto out = page.data.data() + static_cast<std::size_t>(y) * page.width * components;
        if (!alpha) { std::memcpy(out, row, static_cast<std::size_t>(page.width) * components); continue; }
        auto mask = page.alpha.data() + static_cast<std::size_t>(y) * page.width;
        for (std::uint32_t x = 0; x < page.width; ++x) {
            std::memcpy(out + x * 3, row + x * channels, 3); mask[x] = row[x * channels + 3]; translucent |= mask[x] != 255;
        }
    }
    if (!translucent) page.alpha.clear();
    return page;
}
QImage imageOf(const Bitmap& bitmap) {
    return QImage(bitmap.bgra.data(), bitmap.width, bitmap.height, bitmap.stride, QImage::Format_ARGB32).copy();
}
struct JobResult { QString error; bool password{}; };
struct PageRender { QImage image; QString text; TextInventory inventory; std::vector<Annotation> annotations; std::vector<FormField> fields; };
// Builds a new document from PDFs (with page ranges), images and Office files (via LibreOffice), in order.
std::shared_ptr<Document> assemble(const std::vector<MergeItem>& items) {
    auto document = Document::create();
    const auto blank = document->info().pages[0].id;
    QTemporaryDir work;
    if (!work.isValid()) throw std::runtime_error("A temporary folder could not be created.");
    int counter = 0;
    for (const auto& item : items) {
        auto info = document->info();
        if (isImagePath(item.path)) { document->insertImage(loadImage(item.path), info.pages.back().id, info.revision); continue; }
        QString pdf = item.path;
        if (office::isOfficePath(item.path)) pdf = office::convertToPdf(item.path, work.filePath(QString::number(++counter)));
        else if (!item.path.endsWith(".pdf", Qt::CaseInsensitive)) throw std::runtime_error(QFileInfo(item.path).fileName().toStdString() + " is not a supported file type.");
        try { document->insertDocument(localPath(pdf), info.pages.back().id, info.revision, item.ranges.toStdString()); }
        catch (const Error& e) { throw Error(e.code, QFileInfo(item.path).fileName().toStdString() + ": " + e.what()); }
    }
    auto info = document->info();
    document->execute({CommandKind::Delete, blank, info.revision});
    return document;
}
QString exportPath(QWidget* parent, const QString& title, const QString& suffix, const QString& filter) {
    auto path = QFileDialog::getSaveFileName(parent, title, "export." + suffix, filter, nullptr, QFileDialog::DontConfirmOverwrite);
    if (path.isEmpty()) return {};
    if (!path.endsWith("." + suffix, Qt::CaseInsensitive)) path += "." + suffix;
    if (QFileInfo::exists(path) && QMessageBox::question(parent, "Replace output?", "Replace the existing output file?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return {};
    return path;
}
}

// Paints comment cards (title + wrapped body) and per-page group headers.
class CommentDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (index.data(Qt::UserRole + 4).isValid()) return {0, 38};
        auto doc = body(index, width(option) - index.data(Qt::UserRole + 5).toInt() * 22); auto fm = QFontMetrics(boldFont(option));
        return {0, static_cast<int>(18 + fm.height() + 6 + doc->size().height() + 14)};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save(); painter->setRenderHint(QPainter::Antialiasing);
        auto r = option.rect;
        if (index.data(Qt::UserRole + 4).isValid()) {
            painter->setPen(QColor("#2B2F36")); painter->setFont(boldFont(option));
            painter->drawText(r.adjusted(14, 0, -14, 0), Qt::AlignVCenter | Qt::AlignLeft, "⌄  " + index.data(Qt::DisplayRole).toString());
            painter->drawText(r.adjusted(14, 0, -14, 0), Qt::AlignVCenter | Qt::AlignRight, index.data(Qt::UserRole + 4).toString());
            painter->restore(); return;
        }
        const int indent = index.data(Qt::UserRole + 5).toInt() * 22;
        if (option.state & QStyle::State_Selected) painter->fillRect(r, QColor("#E8F0FE"));
        else if (option.state & QStyle::State_MouseOver) painter->fillRect(r, QColor("#F5F7FA"));
        r.adjust(indent, 0, 0, 0);
        painter->setPen(Qt::NoPen); painter->setBrush(QColor("#C9CDD3")); painter->drawEllipse(QRect(r.left() + 14, r.top() + 12, 30, 30));
        painter->setBrush(Qt::NoBrush); painter->setPen(QPen(QColor("#4A4F57"), 1.5)); painter->drawRoundedRect(QRect(r.left() + 22, r.top() + 21, 14, 12), 2, 2);
        painter->setPen(QColor("#1B1D21")); painter->setFont(boldFont(option));
        QFontMetrics fm(boldFont(option));
        painter->drawText(QRect(r.left() + 56, r.top() + 14, r.width() - 70, fm.height()), Qt::AlignVCenter | Qt::AlignLeft,
            fm.elidedText(index.data(Qt::UserRole + 2).toString(), Qt::ElideRight, r.width() - 70));
        auto doc = body(index, r.width());
        painter->translate(r.left() + 56, r.top() + 14 + fm.height() + 6); doc->drawContents(painter); painter->restore();
    }
private:
    static int width(const QStyleOptionViewItem& option) {
        auto list = qobject_cast<const QListView*>(option.widget);
        return list ? list->viewport()->width() : 300;
    }
    static QFont boldFont(const QStyleOptionViewItem& option) { QFont f = option.font; f.setBold(true); return f; }
    static std::unique_ptr<QTextDocument> body(const QModelIndex& index, int rowWidth) {
        auto doc = std::make_unique<QTextDocument>(); doc->setDocumentMargin(0); doc->setTextWidth(std::max(80, rowWidth - 70));
        doc->setPlainText(index.data(Qt::UserRole + 3).toString()); return doc;
    }
};
DocumentPane::DocumentPane(QWidget* parent) : QWidget(parent) {
    auto outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0); outer->setSpacing(0);
    notice = new QLabel; notice->setWordWrap(true); notice->setMargin(6); notice->hide();
    notice->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    notice->setStyleSheet("background:#FFF5D8;color:#664500;"); outer->addWidget(notice);
    auto split = new QSplitter; outer->addWidget(split);
    searchPane = new QWidget; searchPane->setMinimumWidth(200); searchPane->setMaximumWidth(320); auto searchLayout = new QVBoxLayout(searchPane);
    query = new QLineEdit; query->setPlaceholderText("Search text, then press Enter"); query->setAccessibleName("Find in document");
    matches = new QListWidget; matches->setWordWrap(true); matches->setAccessibleName("Search results");
    searchLayout->addWidget(query); searchLayout->addWidget(matches); searchPane->hide();
    split->addWidget(searchPane);
    scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setAlignment(Qt::AlignCenter);
    scroll->setStyleSheet("QScrollArea {background:#E8ECF2;border:0;} QScrollArea > QWidget > QWidget {background:#E8ECF2;}");
    auto stage = new QWidget; auto stageLayout = new QVBoxLayout(stage); stageLayout->setContentsMargins(28, 28, 28, 28);
    canvas = new TextCanvas("Opening document…"); canvas->setAlignment(Qt::AlignCenter); canvas->setAccessibleName("Rendered PDF page");
    canvas->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed); stageLayout->addWidget(canvas, 0, Qt::AlignCenter);
    scroll->setWidget(stage); split->addWidget(scroll);
    auto tabsPane = new QTabWidget; tabsPane->setDocumentMode(true); tabsPane->setMinimumWidth(280); tabsPane->tabBar()->hide(); inspector = tabsPane;
    auto commentsPane = new QWidget; auto commentsLayout = new QVBoxLayout(commentsPane); commentsLayout->setContentsMargins(0, 10, 0, 0); commentsLayout->setSpacing(8);
    commentsTitle = new QLabel("Comments"); QFont heading = commentsTitle->font(); heading.setPointSize(14); heading.setBold(true); commentsTitle->setFont(heading); commentsTitle->setContentsMargins(14, 0, 14, 0);
    addComment = new QLineEdit; addComment->setPlaceholderText("Add a comment"); addComment->setAccessibleName("Add a comment");
    addComment->setStyleSheet("QLineEdit{border:1px solid #9AA0A8;border-radius:5px;padding:8px 10px;margin:0 14px;background:#fff;} QLineEdit:focus{border-color:#1769E8;}");
    comments = new QListWidget; comments->setAccessibleName("Comments and annotations"); comments->setResizeMode(QListView::Adjust); comments->setFrameShape(QFrame::NoFrame);
    comments->setItemDelegate(new CommentDelegate(comments)); comments->setMouseTracking(true); comments->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    auto commentsBarHost = new QWidget; commentsBar = new QHBoxLayout(commentsBarHost); commentsBar->setContentsMargins(14, 0, 14, 6);
    commentsLayout->addWidget(commentsTitle); commentsLayout->addWidget(addComment); commentsLayout->addWidget(commentsBarHost); commentsLayout->addWidget(comments, 1); tabsPane->addTab(commentsPane, "Comments");
    auto bookmarksPane = new QWidget; auto bookmarksLayout = new QVBoxLayout(bookmarksPane); bookmarksLayout->setContentsMargins(0, 10, 0, 0);
    auto bookmarksTitle = new QLabel("Bookmarks"); bookmarksTitle->setFont(heading); bookmarksTitle->setContentsMargins(14, 0, 14, 0);
    auto bookmarksBarHost = new QWidget; bookmarksBar = new QHBoxLayout(bookmarksBarHost); bookmarksBar->setContentsMargins(14, 0, 14, 6);
    bookmarks = new QListWidget; bookmarks->setAccessibleName("Bookmarks"); bookmarks->setFrameShape(QFrame::NoFrame);
    bookmarksLayout->addWidget(bookmarksTitle); bookmarksLayout->addWidget(bookmarksBarHost); bookmarksLayout->addWidget(bookmarks, 1); tabsPane->addTab(bookmarksPane, "Bookmarks");
    auto pagesPane = new QWidget; auto pagesLayout = new QVBoxLayout(pagesPane); pagesLayout->setContentsMargins(0, 10, 0, 0);
    auto pagesTitle = new QLabel("Pages"); pagesTitle->setFont(heading); pagesTitle->setContentsMargins(14, 0, 14, 0);
    auto pagesBarHost = new QWidget; pagesBar = new QHBoxLayout(pagesBarHost); pagesBar->setContentsMargins(14, 0, 14, 6);
    pages = new QListWidget; pages->setAccessibleName("Document pages"); pages->setViewMode(QListView::IconMode); pages->setIconSize(QSize(120, 156));
    pages->setResizeMode(QListView::Adjust); pages->setMovement(QListView::Static); pages->setSpacing(0); pages->setGridSize(QSize(144, 200)); pages->setUniformItemSizes(true); pages->setWordWrap(true); pages->setFrameShape(QFrame::NoFrame);
    pagesLayout->addWidget(pagesTitle); pagesLayout->addWidget(pagesBarHost); pagesLayout->addWidget(pages, 1); tabsPane->addTab(pagesPane, "Pages");
    auto layersPane = new QWidget; auto layersLayout = new QVBoxLayout(layersPane); layersLayout->setContentsMargins(0, 10, 0, 0);
    auto layersTitle = new QLabel("Layers"); layersTitle->setFont(heading); layersTitle->setContentsMargins(14, 0, 14, 0);
    layers = new QListWidget; layers->setAccessibleName("Layers"); layers->setFrameShape(QFrame::NoFrame);
    layersLayout->addWidget(layersTitle); layersLayout->addWidget(layers, 1); tabsPane->addTab(layersPane, "Layers");
    auto inspectorPane = new QWidget; auto propertiesLayout = new QVBoxLayout(inspectorPane);
    auto title = new QLabel("Page properties"); title->setFont(heading);
    properties = new QLabel; properties->setWordWrap(true); properties->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text = new QPlainTextEdit; text->setReadOnly(true); text->setPlaceholderText("Extracted page text appears here."); text->setAccessibleName("Accessible extracted page text");
    propertiesLayout->addWidget(title); propertiesLayout->addWidget(properties); propertiesLayout->addSpacing(16);
    propertiesLayout->addWidget(new QLabel("Page text")); propertiesLayout->addWidget(text, 1);
    auto scope = new QLabel("Preview · Local files only\n\nClick-to-type text editing (including embedded fonts and non-Latin text) and Annotate tools (highlight, underline, strike-out, shapes, pen, notes, text boxes) are available. Fill form fills text, check box, radio and drop-down fields (scripts are not run; signature fields are untouched). Redaction flattens the page. Signing is available on macOS.");
    scope->setWordWrap(true); scope->setStyleSheet("color:#596579;font-size:11px;"); propertiesLayout->addWidget(scope);
    tabsPane->addTab(inspectorPane, "Page");
    split->addWidget(inspector); split->setStretchFactor(1, 1); split->setSizes({220, 830, 340});
}
Window::Window(std::shared_ptr<RenderService> renderer, bool smoke) : renderer_(std::move(renderer)), smoke_(smoke) {
    worker_.setMaxThreadCount(1); thumbs_.setMaxThreadCount(1);
    setWindowTitle("FolioForge · PDF editor preview"); resize(1440, 900); setMinimumSize(1024, 700); setAcceptDrops(true);
    tabs_ = new QTabWidget; tabs_->setTabsClosable(true); tabs_->setMovable(true); tabs_->setDocumentMode(true); setCentralWidget(tabs_);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &Window::closeTab);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] { updateActions(); if (auto p = active(); p && pageBox_) pageBox_->setText(QString::number(p->currentPage + 1)); });
    if (!smoke_) { auto timer = new QTimer(this); connect(timer, &QTimer::timeout, this, [this] { saveRecovery(); }); timer->start(20000); }
    auto file = menuBar()->addMenu("&File"); auto edit = menuBar()->addMenu("&Edit"); auto page = menuBar()->addMenu("&Pages");
    auto view = menuBar()->addMenu("&View"); auto help = menuBar()->addMenu("&Help");
    auto toolbar = addToolBar("Document"); toolbar->setMovable(false); toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    setStyleSheet("QToolBar{background:#fff;border:0;border-bottom:1px solid #E3E6EB;spacing:4px;padding:3px 8px;} QToolButton{padding:5px 9px;border:0;border-radius:5px;color:#2B2F36;} QToolButton:hover{background:#EEF1F6;} QToolButton:checked{background:#DCE8FF;color:#0B4FC4;} QToolButton:disabled{color:#A9AEB6;} QToolButton::menu-indicator{image:none;} QTabBar::tab{padding:6px 14px;} QStatusBar{background:#fff;border-top:1px solid #E3E6EB;}");
    auto action = [this](QMenu* menu, const QString& label, const QKeySequence& shortcut, auto callback) {
        auto a = menu->addAction(label); a->setShortcut(shortcut); connect(a, &QAction::triggered, this, callback); return a;
    };
    auto create = action(file, "&New PDF", QKeySequence::New, [this] { newDocument(); });
    create->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    auto open = action(file, "&Open PDF…", QKeySequence::Open, [this] {
        auto paths = QFileDialog::getOpenFileNames(this, "Open PDF or image", {}, "PDF, images and Office files (*.pdf *.png *.jpg *.jpeg *.docx *.doc *.odt *.rtf *.txt *.xlsx *.xls *.ods *.pptx *.ppt *.odp);;PDF files (*.pdf);;Images (*.png *.jpg *.jpeg)");
        for (auto& path : paths) openPath(path);
    });
    toolbar->addAction(open);
    action(file, "&Merge files into new PDF…", QKeySequence("Ctrl+Shift+M"), [this] {
        MergeDialog dialog(this); if (dialog.exec() == QDialog::Accepted) mergeFiles(dialog.items());
    });
    action(file, "Images to new PDF…", QKeySequence("Ctrl+Shift+I"), [this] {
        auto paths = QFileDialog::getOpenFileNames(this, "Convert images to one PDF (one image per page)", {}, "Images (*.png *.jpg *.jpeg)"); openImages(paths);
    });
    file->addSeparator();
    save_ = action(file, "&Save", QKeySequence::Save, [this] { save(active(), false); });
    toolbar->addAction(save_);
    saveAs_ = action(file, "Save &As…", QKeySequence::SaveAs, [this] { save(active(), true); });
    save_->setToolTip("Apply any active text edit and save the PDF (Ctrl+S)");
    signAction_ = action(file, "Sign and save copy…", {}, [this] { signDocument(active()); });
    file->addSeparator(); exportImage_ = action(file, "Export current page as PNG…", {}, [this] { exportImage(); });
    exportText_ = action(file, "Export document text…", {}, [this] { exportText(); });
    action(file, "Close tab", QKeySequence::Close, [this] { closeTab(tabs_->currentIndex()); });
    file->addSeparator(); action(file, "E&xit", QKeySequence::Quit, [this] { close(); });
    undo_ = action(edit, "&Undo", QKeySequence::Undo, [this] { auto p = active(); if (!p || p->busy) return; auto r = p->info.revision; run(p, "Undoing", [p, r] { p->document->undo(r); }, [this, p] { render(p); }); });
    redo_ = action(edit, "&Redo", QKeySequence::Redo, [this] { auto p = active(); if (!p || p->busy) return; auto r = p->info.revision; run(p, "Redoing", [p, r] { p->document->redo(r); }, [this, p] { render(p); }); });
    toolbar->addSeparator(); toolbar->addAction(undo_); toolbar->addAction(redo_);
    editText_ = action(edit, "Edit text", QKeySequence("Ctrl+E"), [this] {
        auto p = active(); if (!p || p->busy || p->canvas->editing()) return;
        p->canvas->editMode = editText_->isChecked(); p->canvas->update();
        if (p->canvas->editMode) { p->canvas->tool = TextCanvas::Tool::None; for (auto a : tools_) a->setChecked(false); }
        if (p->canvas->editMode) {
            p->canvas->setFocus();
            statusBar()->showMessage(p->textInventory.runs.empty() ? QString::fromStdString(p->textInventory.explanation) :
                "Click text on the page to place a cursor and type. Enter or clicking elsewhere applies, Escape cancels. Left/Right select a run; Enter or F2 edits it.");
        }
    });
    editText_->setCheckable(true); toolbar->addAction(editText_);
    applyText_ = action(edit, "Apply text", QKeySequence("Ctrl+Return"), [this] {
        auto p = active(); if (p && !p->busy && p->canvas->editing()) commitTextEdit(p, p->canvas->editor->property("runIndex").toInt());
    });
    cancelText_ = action(edit, "Cancel edit", {}, [this] { auto p = active(); if (p && !p->busy) cancelTextEdit(p); });
    find_ = action(edit, "&Find…", QKeySequence::Find, [this] { if (auto p = active()) { p->searchPane->show(); p->query->setFocus(); p->query->selectAll(); } });
    auto annotate = menuBar()->addMenu("&Annotate");
    auto annotationBar = new QToolBar("Tools"); annotationBar->setMovable(false); annotationBar->setOrientation(Qt::Vertical); annotationBar->setToolButtonStyle(Qt::ToolButtonIconOnly); annotationBar->setIconSize(QSize(22, 22));
    addToolBar(Qt::LeftToolBarArea, annotationBar);
    auto glyph = [](const QString& text, const QColor& bg = Qt::transparent) {
        QPixmap pm(44, 44); pm.setDevicePixelRatio(2); pm.fill(Qt::transparent); QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing);
        if (bg.alpha()) { g.setPen(Qt::NoPen); g.setBrush(bg); g.drawRoundedRect(QRectF(3, 6, 16, 10), 2, 2); }
        QFont f; f.setPointSize(15); f.setBold(true); g.setFont(f); g.setPen(QColor("#2B2F36")); g.drawText(QRectF(0, 0, 22, 22), Qt::AlignCenter, text); return QIcon(pm);
    };
    const QHash<QString, QIcon> icons{{"Select", glyph("➤")}, {"Highlight", glyph("A", QColor(255, 224, 0))}, {"Underline", glyph("U")}, {"Strike-out", glyph("S")}, {"Rectangle", glyph("▭")},
        {"Ellipse", glyph("◯")}, {"Pen", glyph("✎")}, {"Note", glyph("✉")}, {"Text box", glyph("T")}, {"Fill form", glyph("☑")}, {"Redact", glyph("■")}};
    auto group = new QActionGroup(this); group->setExclusive(true);
    struct ToolSpec { const char* label; AnnotationKind kind; TextCanvas::Tool tool; const char* tip; };
    const ToolSpec specs[] = {
        {"Highlight", AnnotationKind::Highlight, TextCanvas::Tool::Area, "Drag over text to highlight it"},
        {"Underline", AnnotationKind::Underline, TextCanvas::Tool::Area, "Drag over text to underline it"},
        {"Strike-out", AnnotationKind::StrikeOut, TextCanvas::Tool::Area, "Drag over text to strike it out"},
        {"Rectangle", AnnotationKind::Rectangle, TextCanvas::Tool::Area, "Drag to draw a rectangle"},
        {"Ellipse", AnnotationKind::Ellipse, TextCanvas::Tool::Area, "Drag to draw an ellipse"},
        {"Pen", AnnotationKind::Ink, TextCanvas::Tool::Pen, "Draw freehand"},
        {"Note", AnnotationKind::Note, TextCanvas::Tool::Point, "Click to place a sticky note"},
        {"Text box", AnnotationKind::FreeText, TextCanvas::Tool::Area, "Drag a box, then type the text to place"},
        {"Select", AnnotationKind::Other, TextCanvas::Tool::Select, "Click an annotation to select it"},
        {"Fill form", AnnotationKind::Other, TextCanvas::Tool::Form, "Click a form field to fill it. Scripts are not run; signature fields are untouched."},
        {"Redact", AnnotationKind::Other, TextCanvas::Tool::Redact, "Drag boxes over content to remove, then choose Apply redactions"}};
    for (const auto& spec : specs) {
        auto a = new QAction(spec.label, this); a->setCheckable(true); a->setIcon(icons.value(spec.label)); a->setToolTip(QString("%1 — %2").arg(spec.label, spec.tip)); a->setStatusTip(spec.tip);
        a->setData(QVariant::fromValue<int>(static_cast<int>(spec.tool) * 100 + static_cast<int>(spec.kind)));
        group->addAction(a); annotate->addAction(a); annotationBar->addAction(a); tools_.push_back(a);
        connect(a, &QAction::triggered, this, [this, a] { selectTool(active(), a); });
    }
    annotate->addSeparator();
    annotationColor_ = action(annotate, "Color…", {}, [this] {
        auto chosen = QColorDialog::getColor(customColor_.value_or(QColor(255, 224, 0)), this, "Annotation color");
        if (chosen.isValid()) customColor_ = chosen;
    });
    action(annotate, "Recolor selected annotation…", {}, [this] {
        auto p = active(); if (!p) return;
        auto chosen = QColorDialog::getColor(customColor_.value_or(QColor(255, 224, 0)), this, "Annotation color");
        if (chosen.isValid()) editAnnotation(p, [chosen](AddAnnotation& r) { r.color = {chosen.redF(), chosen.greenF(), chosen.blueF()}; });
    });
    applyRedactions_ = action(annotate, "Apply redactions…", {}, [this] { applyRedactions(active()); });
    action(annotate, "Clear pending redactions", {}, [this] { if (auto p = active()) { p->canvas->redactions.clear(); p->canvas->update(); } });
    deleteAnnotation_ = action(annotate, "Delete selected annotation", {}, [this] { removeSelectedAnnotation(active()); });
    annotationColor_->setIcon(glyph("◐")); deleteAnnotation_->setIcon(glyph("⌫")); annotationColor_->setToolTip("Annotation color"); deleteAnnotation_->setToolTip("Delete selected annotation");
    annotationBar->addSeparator(); annotationBar->addAction(annotationColor_); annotationBar->addAction(deleteAnnotation_);
    auto organize = new QMenu(this);
    insert_ = action(page, "Insert blank", {}, [this] { command(CommandKind::InsertBlank); });
    merge_ = action(page, "Insert PDF…", {}, [this] {
        auto p = active(); if (!p || p->busy || !p->document) return;
        auto path = QFileDialog::getOpenFileName(this, "Insert pages from a PDF after the current page", {}, "PDF files (*.pdf)"); if (path.isEmpty()) return;
        bool ok = true; QString ranges;
        if (!smoke_) ranges = QInputDialog::getText(this, "Pages to insert", "Pages (for example 1-3,5,8-; blank = all):", QLineEdit::Normal, {}, &ok);
        if (!ok) return;
        auto id = p->info.pages[p->currentPage].id; auto rev = p->info.revision;
        run(p, "Inserting PDF", [p, path, id, rev, ranges] { p->document->insertDocument(localPath(path), id, rev, ranges.toStdString()); }, [this, p] { render(p); });
    });
    insertImage_ = action(page, "Insert image…", {}, [this] {
        auto p = active(); if (!p || p->busy || !p->document) return;
        auto paths = QFileDialog::getOpenFileNames(this, "Insert images after the current page", {}, "Images (*.png *.jpg *.jpeg)"); if (paths.isEmpty()) return;
        auto id = p->info.pages[p->currentPage].id; auto rev = p->info.revision;
        run(p, "Inserting images", [p, paths, id, rev] {
            auto after = id; auto revision = rev;
            for (const auto& path : paths) {
                p->document->insertImage(loadImage(path), after, revision);
                auto info = p->document->info(); revision = info.revision;
                for (std::size_t i = 0; i < info.pages.size(); ++i) if (info.pages[i].id == after) { after = info.pages[i + 1].id; break; }
            }
        }, [this, p] { render(p); });
    });
    duplicate_ = action(page, "Duplicate", {}, [this] { command(CommandKind::Duplicate); });
    left_ = action(page, "Rotate left", {}, [this] { command(CommandKind::RotateLeft); });
    right_ = action(page, "Rotate right", {}, [this] { command(CommandKind::RotateRight); });
    earlier_ = action(page, "Move earlier", {}, [this] { command(CommandKind::MoveEarlier); });
    later_ = action(page, "Move later", {}, [this] { command(CommandKind::MoveLater); });
    delete_ = action(page, "Delete page", {}, [this] { command(CommandKind::Delete); });
    for (auto a : {insert_, merge_, insertImage_, duplicate_, left_, right_, earlier_, later_, delete_}) organize->addAction(a);
    toolbar->addSeparator(); toolbar->addAction(find_);
    auto prev = action(view, "Previous page", QKeySequence("Alt+Up"), [this] { auto p = active(); if (p && !p->busy && !p->canvas->editing()) p->pages->setCurrentRow(std::max(0, p->currentPage - 1)); });
    auto next = action(view, "Next page", QKeySequence("Alt+Down"), [this] { auto p = active(); if (p && !p->busy && !p->canvas->editing()) p->pages->setCurrentRow(std::min(p->pages->count() - 1, p->currentPage + 1)); });
    zoom_ = new QComboBox; zoom_->addItems({"50%", "75%", "100%", "125%", "150%", "200%", "Fit width", "Fit page"}); zoom_->setCurrentText("100%");
    zoom_->setAccessibleName("Page zoom"); zoom_->setEditable(true); zoom_->lineEdit()->setReadOnly(true); zoom_->lineEdit()->setAlignment(Qt::AlignCenter); zoom_->setInsertPolicy(QComboBox::NoInsert);
    zoomOutButton_ = new QToolButton; zoomOutButton_->setText("−"); zoomOutButton_->setToolTip("Zoom out (Ctrl+−)"); zoomOutButton_->setAutoRaise(true);
    zoomInButton_ = new QToolButton; zoomInButton_->setText("+"); zoomInButton_->setToolTip("Zoom in (Ctrl++)"); zoomInButton_->setAutoRaise(true);
    zoom_->hide(); zoom_->setParent(this);
    connect(zoom_, &QComboBox::textActivated, this, [this](const QString& value) {
        auto p = active(); if (!p || p->busy || p->info.pages.empty()) return;
        auto size = p->info.pages[p->currentPage]; double w = size.width, h = size.height; if (size.rotation % 180) std::swap(w, h);
        if (value.startsWith("Fit")) {
            p->scale = (p->scroll->viewport()->width() - 64) / w;
            if (value == "Fit page") p->scale = std::min(p->scale, (p->scroll->viewport()->height() - 64) / h);
            p->scale = std::clamp(p->scale, 0.05, 4.0);
        } else p->scale = value.chopped(1).toDouble() / 100.0;
        render(p);
    });
    propertiesToggle_ = action(view, "Side panel", QKeySequence("Ctrl+Alt+I"), [this] {
        const bool show = propertiesToggle_->isChecked(); QSettings().setValue("showProperties", show);
        showPanel(show ? lastPanel_ : -1);
    });
    propertiesToggle_->setCheckable(true); panel_ = (smoke_ || QSettings().value("showProperties", true).toBool()) ? 0 : -1; propertiesToggle_->setChecked(panel_ >= 0);
    auto stepZoom = [this](double factor) {
        auto p = active(); if (!p || p->busy || p->info.pages.empty()) return;
        p->scale = std::clamp(p->scale * factor, 0.05, 4.0); render(p);
    };
    zoomOut_ = action(view, "Zoom out", QKeySequence::ZoomOut, [stepZoom] { stepZoom(1 / 1.25); });
    zoomIn_ = action(view, "Zoom in", QKeySequence::ZoomIn, [stepZoom] { stepZoom(1.25); });
    connect(zoomOutButton_, &QToolButton::clicked, zoomOut_, &QAction::trigger); connect(zoomInButton_, &QToolButton::clicked, zoomIn_, &QAction::trigger);
    {
        auto rail = new QToolBar("Panels"); rail->setMovable(false); rail->setOrientation(Qt::Vertical); rail->setToolButtonStyle(Qt::ToolButtonIconOnly); rail->setIconSize(QSize(22, 22));
        rail->setStyleSheet("QToolBar{border:0;border-left:1px solid #E3E6EB;background:#fff;padding:6px 4px;spacing:6px;} QToolButton{padding:7px;border-radius:6px;font-size:16px;} QToolButton:checked{background:#DCE8FD;} QLineEdit{border:1px solid #C9CDD3;border-radius:4px;padding:3px 2px;}");
        addToolBar(Qt::RightToolBarArea, rail);
        auto panelIcon = [](const QString& text) {
            QPixmap pm(44, 44); pm.setDevicePixelRatio(2); pm.fill(Qt::transparent); QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing);
            QFont f; f.setPointSize(15); g.setFont(f); g.setPen(QColor("#2B2F36")); g.drawText(QRectF(0, 0, 22, 22), Qt::AlignCenter, text); return QIcon(pm);
        };
        struct PanelSpec { const char* label; const char* glyph; };
        const PanelSpec panels[] = {{"Comments", "💬"}, {"Bookmarks", "🔖"}, {"Pages", "▤"}, {"Layers", "◫"}, {"Page properties", "ⓘ"}};
        int index = 0;
        for (const auto& spec : panels) {
            auto a = rail->addAction(panelIcon(spec.glyph), spec.label); a->setCheckable(true); a->setToolTip(spec.label); panelActions_.push_back(a);
            const int target = index++;
            connect(a, &QAction::triggered, this, [this, target] { showPanel(panel_ == target ? -1 : target); });
        }
        auto spacer = new QWidget; spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding); rail->addWidget(spacer);
        pageBox_ = new QLineEdit("1"); pageBox_->setFixedWidth(40); pageBox_->setAlignment(Qt::AlignCenter); pageBox_->setValidator(new QIntValidator(1, 10000, pageBox_));
        pageBox_->setToolTip("Go to page"); pageBox_->setAccessibleName("Page number"); rail->addWidget(pageBox_);
        auto chevron = [&](const QString& text, QAction* target, const QString& tip) { auto a = rail->addAction(text); a->setToolTip(tip); connect(a, &QAction::triggered, target, &QAction::trigger); return a; };
        chevron("︿", prev, "Previous page (Alt+Up)"); chevron("﹀", next, "Next page (Alt+Down)");
        rail->addSeparator();
        auto turn = rail->addAction("⟳"); turn->setToolTip("Rotate page right"); connect(turn, &QAction::triggered, right_, &QAction::trigger);
        auto fit = rail->addAction("⤢"); fit->setToolTip("Fit page"); connect(fit, &QAction::triggered, this, [this] {
            auto p = active(); if (!p || p->busy || p->info.pages.empty()) return;
            auto size = p->info.pages[p->currentPage]; double w = size.width, h = size.height; if (size.rotation % 180) std::swap(w, h);
            p->scale = std::clamp(std::min((p->scroll->viewport()->width() - 64) / w, (p->scroll->viewport()->height() - 64) / h), 0.05, 4.0); render(p);
        });
        rail->addWidget(zoomInButton_); rail->addWidget(zoomOutButton_);
        connect(pageBox_, &QLineEdit::returnPressed, this, [this] { if (auto p = active()) goToPage(p, pageBox_->text().toInt() - 1); });
    }
    showPanel(panel_);
    action(help, "About FolioForge", {}, [this] { QMessageBox::about(this, "FolioForge 0.1", "A local PDF reader and page-tool preview.\n\nC++20 · Qt 6 · QPDF · PDFium\n\nAdvanced editing and release hardening remain in development."); });
    auto welcome = new QWidget; auto layout = new QVBoxLayout(welcome); layout->setAlignment(Qt::AlignCenter);
    auto brand = new QLabel("FolioForge"); QFont font = brand->font(); font.setPointSize(30); font.setBold(true); brand->setFont(font); brand->setAlignment(Qt::AlignCenter);
    auto subtitle = new QLabel("A clear workspace for your PDFs."); subtitle->setAlignment(Qt::AlignCenter);
    auto openButton = new QPushButton("Open PDF or image"); openButton->setMinimumSize(220, 44); connect(openButton, &QPushButton::clicked, open, &QAction::trigger);
    auto newButton = new QPushButton("Create a blank PDF"); newButton->setMinimumSize(220, 40); connect(newButton, &QPushButton::clicked, create, &QAction::trigger);
    layout->addWidget(brand); layout->addWidget(subtitle); layout->addSpacing(24); layout->addWidget(openButton, 0, Qt::AlignCenter); layout->addWidget(newButton, 0, Qt::AlignCenter);
    auto hint = new QLabel("Open · Read · Search · Organize · Save\n\nDrop PDFs or JPG/PNG images here. PDFs open in separate tabs; dropped images become one PDF."); hint->setAlignment(Qt::AlignCenter); layout->addSpacing(24); layout->addWidget(hint);
    tabs_->addTab(welcome, "Start"); tabs_->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    statusBar()->showMessage("Ready · Files stay on this computer"); updateActions();
    if (!smoke_) { QSettings settings; restoreGeometry(settings.value("windowGeometry").toByteArray()); }
}
DocumentPane* Window::active() const { return dynamic_cast<DocumentPane*>(tabs_->currentWidget()); }
bool Window::busy() const { for (int i = 0; i < tabs_->count(); ++i) if (auto p = dynamic_cast<DocumentPane*>(tabs_->widget(i)); p && p->busy) return true; return false; }
bool Window::smokeReady() const { auto p = active(); return p && !p->busy && p->document && !p->image.isNull(); }
int Window::advanceSmokeTest() {
    auto p = active();
    if (!smoke_ || !smokeReady()) return 0;
    switch (smokeStep_++) {
    case 0:
        if (p->info.pages.size() != 1 || !p->text->toPlainText().contains("FolioForge test")) return -1;
        duplicate_->trigger(); break;
    case 1:
        if (p->info.pages.size() != 2 || p->currentPage != 1) return -1;
        right_->trigger(); break;
    case 2:
        if (p->info.pages[1].rotation != 90) return -1;
        undo_->trigger(); break;
    case 3:
        if (p->info.pages[1].rotation != 0) return -1;
        redo_->trigger(); break;
    case 4:
        if (p->info.pages[1].rotation != 90) return -1;
        insert_->trigger(); break;
    case 5:
        if (p->info.pages.size() != 3 || p->currentPage != 2) return -1;
        delete_->trigger(); break;
    case 6:
        if (p->info.pages.size() != 2) return -1;
        earlier_->trigger(); break;
    case 7:
        if (p->currentPage != 0 || p->info.pages[0].rotation != 90) return -1;
        later_->trigger(); break;
    case 8:
        if (p->currentPage != 1 || p->info.pages[1].rotation != 90) return -1;
        p->query->setText("FolioForge test"); search(p); break;
    case 9:
        if (p->matches->count() != 2) return -1;
        p->pages->setCurrentRow(0); break;
    case 10:
        if (p->currentPage != 0 || !p->info.dirty) return -1;
        run(p, "Smoke save and reopen", [p, renderer = renderer_] {
            auto path = std::filesystem::current_path() / "desktop-smoke-output.pdf";
            renderer->validate(p->document->snapshot()); p->document->save(path, true);
            p->document = Document::open(path);
        }, [this, p] { render(p); }); break;
    case 11:
        if (p->info.dirty || p->info.pages.size() != 2 || p->info.pages[1].rotation != 90) return -1;
        if (p->canvas->runs.size() != 1) return -1;
        editText_->trigger();
        {
            auto point = p->canvas->box(0).center();
            QMouseEvent click(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(p->canvas, &click);
        }
        if (!p->canvas->editing()) return -1;
        {
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(p->canvas->editor, &escape);
        }
        if (p->canvas->editing() || p->info.dirty) return -1;
        {
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(p->canvas, &enter);
        }
        if (!p->canvas->editing()) return -1;
        p->canvas->editor->setText("Folio edit");
        if (!grab().save("desktop-text-edit.png")) return -1;
        {
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(p->canvas->editor, &enter);
        }
        break;
    case 12:
        if (!p->info.dirty || !p->text->toPlainText().contains("Folio edit") || p->text->toPlainText().contains("FolioForge test")) return -1;
        undo_->trigger(); break;
    case 13:
        if (!p->text->toPlainText().contains("FolioForge test") || p->info.dirty) return -1;
        redo_->trigger(); break;
    case 14:
        if (!p->text->toPlainText().contains("Folio edit")) return -1;
        run(p, "Saving text edit", [p, renderer = renderer_] {
            auto path = std::filesystem::current_path() / "desktop-smoke-text-output.pdf";
            renderer->validate(p->document->snapshot()); p->document->save(path, true); p->document = Document::open(path);
        }, [this, p] { render(p); }); break;
    case 15:
        if (p->info.dirty || !p->text->toPlainText().contains("Folio edit") || p->canvas->runs.size() != 1) return -1;
        {
            // A click places a caret instead of selecting the whole run, and typing may outgrow the original slot.
            auto point = p->canvas->box(0).center();
            QMouseEvent click(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(p->canvas, &click);
        }
        if (!p->canvas->editing() || p->canvas->editor->hasSelectedText() || p->canvas->editor->cursorPosition() <= 0) return -1;
        {
            const int original = p->canvas->editor->width();
            p->canvas->editor->setText("Folio edit, now longer");
            if (p->canvas->editor->width() <= original) return -1;
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(p->canvas->editor, &enter);
        }
        break;
    case 16:
        if (!p->info.dirty || !p->text->toPlainText().contains("now longer")) return -1;
        {
            QImage picture(64, 32, QImage::Format_ARGB32); picture.fill(QColor(200, 30, 30, 255)); picture.setPixelColor(0, 0, QColor(0, 0, 0, 0));
            auto path = QDir::current().absoluteFilePath("smoke-image.png");
            if (!picture.save(path)) return -1;
            openPath(path);
        }
        break;
    case 17:
        if (!p->info.dirty || p->info.pages.size() != 1 || std::abs(p->info.pages[0].width - 48) > 0.01 || std::abs(p->info.pages[0].height - 24) > 0.01) return -1;
        if (tabs_->tabText(tabs_->currentIndex()) != "smoke-image.pdf *") return -1;
        // Annotation tools: drag a highlight over the page, then undo, redo, select and delete it.
        tools_[0]->trigger();
        if (p->canvas->tool != TextCanvas::Tool::Area || toolKind_ != AnnotationKind::Highlight) return -1;
        p->canvas->areaDrawn(p->canvas->toPdf({6, 6}), p->canvas->toPdf({40, 18}));
        break;
    case 18:
        if (p->canvas->annotations.size() != 1 || p->canvas->annotations[0].kind != AnnotationKind::Highlight) return -1;
        undo_->trigger(); break;
    case 19:
        if (!p->canvas->annotations.empty()) return -1;
        redo_->trigger(); break;
    case 20:
        if (p->canvas->annotations.size() != 1) return -1;
        tools_[8]->trigger();
        {
            auto point = p->canvas->annotationBox(p->canvas->annotations[0]).center();
            QMouseEvent click(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(p->canvas, &click);
        }
        if (p->canvas->selectedAnnotation != 0 || !deleteAnnotation_->isEnabled()) return -1;
        deleteAnnotation_->trigger(); break;
    case 21:
        if (!p->canvas->annotations.empty()) return -1;
        tools_[9]->trigger();
        if (p->canvas->tool != TextCanvas::Tool::Form || !p->canvas->fields.empty()) return -1;
#ifndef _WIN32
        {
            // A stand-in for LibreOffice: copies the smoke PDF to where the real converter would write its output.
            auto script = QDir::current().absoluteFilePath("fake-soffice.sh");
            QFile file(script);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return -1;
            file.write("#!/bin/sh\nout=\nfor a in \"$@\"; do if [ \"$prev\" = --outdir ]; then out=$a; fi; prev=$a; last=$a; done\nb=$(basename \"$last\")\ncp \"" + QDir::current().absoluteFilePath("smoke-input.pdf").toUtf8() + "\" \"$out/${b%.*}.pdf\"\n");
            file.close(); file.setPermissions(file.permissions() | QFileDevice::ExeUser);
            qputenv("FOLIOFORGE_SOFFICE", script.toUtf8());
            auto sample = QDir::current().absoluteFilePath("smoke-sample.docx"); QFile doc(sample); if (!doc.open(QIODevice::WriteOnly)) return -1; doc.write("x"); doc.close();
            mergeFiles({{QDir::current().absoluteFilePath("smoke-input.pdf"), "1"}, {QDir::current().absoluteFilePath("smoke-image.png"), {}}, {sample, {}}}, "Merged.pdf");
        }
        break;
    case 22:
        if (p->info.pages.size() != 3 || !tabs_->tabText(tabs_->currentIndex()).startsWith("Merged.pdf")) return -1;
        p->canvas->redactionPage = p->info.pages[p->currentPage].id; p->canvas->redactionRevision = p->info.revision;
        p->canvas->redactions.emplace_back(QPointF(0, 0), QPointF(100, 100));
        applyRedactions(p);
        break;
    case 23:
        if (p->info.pages.size() != 3 || !p->info.canUndo || !p->canvas->redactions.empty()) return -1;
        return 1;
#else
        return 1;
#endif
    default:
        return 1;
    }
    return 0;
}
void Window::applyPanel(DocumentPane* p) {
    p->inspector->setVisible(panel_ >= 0);
    if (panel_ >= 0) static_cast<QTabWidget*>(p->inspector)->setCurrentIndex(panel_);
}
void Window::showPanel(int index) {
    panel_ = index; if (index >= 0) lastPanel_ = index;
    for (int i = 0; i < static_cast<int>(panelActions_.size()); ++i) { QSignalBlocker b(panelActions_[i]); panelActions_[i]->setChecked(i == index); }
    if (propertiesToggle_) { QSignalBlocker b(propertiesToggle_); propertiesToggle_->setChecked(index >= 0); QSettings().setValue("showProperties", index >= 0); }
    for (int i = 0; i < tabs_->count(); ++i) if (auto p = dynamic_cast<DocumentPane*>(tabs_->widget(i))) applyPanel(p);
}
void Window::goToPage(DocumentPane* p, int page) {
    if (!p || p->busy || p->canvas->editing() || p->info.pages.empty()) return;
    p->pages->setCurrentRow(std::clamp(page, 0, static_cast<int>(p->info.pages.size()) - 1));
    if (pageBox_) pageBox_->setText(QString::number(p->currentPage + 1));
}
void Window::loadThumbnails(DocumentPane* p) {
    if (p->thumbToken) p->thumbToken->store(false);
    auto token = std::make_shared<std::atomic_bool>(true); p->thumbToken = token;
    auto snap = p->document->snapshot(); QPointer<DocumentPane> pane(p);
    for (std::size_t i = 0; i < snap.pages.size(); ++i) {
        const auto& info = p->info.pages[i]; double w = info.width, h = info.height; if (info.rotation % 180) std::swap(w, h);
        const double scale = std::clamp(120.0 / std::max(1.0, w), 0.02, 1.0), ratio = devicePixelRatioF();
        QtConcurrent::run(&thumbs_, [this, token, snap, i, scale, ratio, pane, renderer = renderer_] {
            if (!token->load()) return;
            QImage image;
            try { image = imageOf(renderer->render(snap, i, scale * ratio)); } catch (const std::exception&) { return; }
            image.setDevicePixelRatio(ratio);
            QMetaObject::invokeMethod(this, [pane, token, i, image] {
                if (!pane || !token->load() || static_cast<int>(i) >= pane->pages->count()) return;
                pane->pages->item(static_cast<int>(i))->setIcon(QPixmap::fromImage(image));
            }, Qt::QueuedConnection);
        });
    }
}
void Window::wirePanels(DocumentPane* p) {
    auto button = [](const QString& glyph, const QString& tip) {
        auto b = new QToolButton; b->setText(glyph); b->setToolTip(tip); b->setAccessibleName(tip); b->setAutoRaise(true); return b;
    };
    // Pages: insert blank, duplicate, rotate, delete.
    for (auto spec : {std::make_pair(QString("＋"), insert_), std::make_pair(QString("⧉"), duplicate_), std::make_pair(QString("↺"), left_), std::make_pair(QString("↻"), right_), std::make_pair(QString("🗑"), delete_)}) {
        auto b = button(spec.first, spec.second->text()); p->pagesBar->addWidget(b);
        connect(b, &QToolButton::clicked, spec.second, &QAction::trigger);
        connect(spec.second, &QAction::changed, b, [b, a = spec.second] { b->setEnabled(a->isEnabled()); }); b->setEnabled(spec.second->isEnabled());
    }
    p->pagesBar->addStretch();
    // Comments: reply, edit, delete the selected comment.
    auto selectedComment = [p]() -> QListWidgetItem* { auto item = p->comments->currentItem(); return item && item->data(Qt::UserRole + 1).isValid() ? item : nullptr; };
    auto replyButton = button("↩", "Reply to the selected comment"), editButton = button("✎", "Edit the selected comment"), deleteButton = button("🗑", "Delete the selected comment (and its replies)");
    for (auto b : {replyButton, editButton, deleteButton}) p->commentsBar->addWidget(b);
    p->commentsBar->addStretch();
    auto ask = [this](const QString& title, const QString& label, const QString& initial, QString& out) {
        if (smoke_) return false; bool ok = false; out = QInputDialog::getMultiLineText(this, title, label, initial, &ok); out = out.trimmed(); return ok && !out.isEmpty();
    };
    connect(replyButton, &QToolButton::clicked, this, [this, p, selectedComment, ask] {
        auto item = selectedComment(); if (!item || p->busy || !p->info.editable) { statusBar()->showMessage("Select a comment to reply to."); return; }
        QString text; if (!ask("Reply", "Your reply:", {}, text)) return;
        auto id = p->info.pages[item->data(Qt::UserRole).toInt()].id; auto index = static_cast<std::uint32_t>(item->data(Qt::UserRole + 1).toInt()); auto rev = p->info.revision;
        run(p, "Adding reply", [p, id, index, text, rev] { p->document->replyToAnnotation(id, index, text.toStdString(), rev); }, [this, p] { render(p); });
    });
    connect(editButton, &QToolButton::clicked, this, [this, p, selectedComment, ask] {
        auto item = selectedComment(); if (!item || p->busy || !p->info.editable) { statusBar()->showMessage("Select a comment to edit."); return; }
        QString text; if (!ask("Edit comment", "Comment text:", item->data(Qt::UserRole + 3).toString(), text)) return;
        const int page = item->data(Qt::UserRole).toInt(); auto index = static_cast<std::uint32_t>(item->data(Qt::UserRole + 1).toInt());
        if (item->data(Qt::UserRole + 6).toInt() == static_cast<int>(AnnotationKind::FreeText)) {
            // Text-box text is part of the drawing, so rebuild its appearance.
            if (page != p->currentPage) { statusBar()->showMessage("Open the page of this text box first, then edit it."); return; }
            selectAnnotationByIndex(p, static_cast<int>(index));
            editAnnotation(p, [text](AddAnnotation& r) { r.contents = text.toStdString(); }); return;
        }
        auto id = p->info.pages[page].id; auto rev = p->info.revision;
        run(p, "Editing comment", [p, id, index, text, rev] { p->document->setAnnotationText(id, index, text.toStdString(), rev); }, [this, p] { render(p); });
    });
    connect(deleteButton, &QToolButton::clicked, this, [this, p, selectedComment] {
        auto item = selectedComment(); if (!item || p->busy || !p->info.editable) { statusBar()->showMessage("Select a comment to delete."); return; }
        auto id = p->info.pages[item->data(Qt::UserRole).toInt()].id; auto index = static_cast<std::uint32_t>(item->data(Qt::UserRole + 1).toInt()); auto rev = p->info.revision;
        run(p, "Deleting comment", [p, id, index, rev] { p->document->removeAnnotation(id, index, rev); }, [this, p] { render(p); });
    });
    // Bookmarks: add for the current page, rename, delete, go to.
    auto addMark = button("＋", "Bookmark the current page"), renameMark = button("✎", "Rename the selected bookmark"), deleteMark = button("🗑", "Delete the selected bookmark");
    for (auto b : {addMark, renameMark, deleteMark}) p->bookmarksBar->addWidget(b);
    p->bookmarksBar->addStretch();
    auto selectedMark = [p]() -> QListWidgetItem* { auto item = p->bookmarks->currentItem(); return item && item->data(Qt::UserRole + 1).isValid() ? item : nullptr; };
    connect(addMark, &QToolButton::clicked, this, [this, p] {
        if (p->busy || !p->info.editable) return;
        QString title = QString("Page %1").arg(p->currentPage + 1);
        if (!smoke_) { bool ok = false; title = QInputDialog::getText(this, "Add bookmark", "Bookmark name:", QLineEdit::Normal, title, &ok).trimmed(); if (!ok || title.isEmpty()) return; }
        auto id = p->info.pages[p->currentPage].id; auto rev = p->info.revision;
        run(p, "Adding bookmark", [p, id, title, rev] { p->document->addBookmark(title.toStdString(), id, rev); });
    });
    connect(renameMark, &QToolButton::clicked, this, [this, p, selectedMark] {
        auto item = selectedMark(); if (!item || p->busy || !p->info.editable || smoke_) return;
        bool ok = false; auto title = QInputDialog::getText(this, "Rename bookmark", "Bookmark name:", QLineEdit::Normal, item->text().trimmed(), &ok).trimmed(); if (!ok || title.isEmpty()) return;
        auto index = static_cast<std::uint32_t>(item->data(Qt::UserRole + 1).toInt()); auto rev = p->info.revision;
        run(p, "Renaming bookmark", [p, index, title, rev] { p->document->renameBookmark(index, title.toStdString(), rev); });
    });
    connect(deleteMark, &QToolButton::clicked, this, [this, p, selectedMark] {
        auto item = selectedMark(); if (!item || p->busy || !p->info.editable) { statusBar()->showMessage("Select a bookmark to delete."); return; }
        auto index = static_cast<std::uint32_t>(item->data(Qt::UserRole + 1).toInt()); auto rev = p->info.revision;
        run(p, "Deleting bookmark", [p, index, rev] { p->document->removeBookmark(index, rev); });
    });
    connect(p->bookmarks, &QListWidget::itemClicked, this, [this, p](QListWidgetItem* item) { if (item->data(Qt::UserRole).isValid() && item->data(Qt::UserRole).toInt() >= 0) goToPage(p, item->data(Qt::UserRole).toInt()); });
    // Layers: the check box shows or hides the layer.
    connect(p->layers, &QListWidget::itemChanged, this, [this, p](QListWidgetItem* item) {
        if (p->busy || !item->data(Qt::UserRole).isValid()) return;
        if (!p->info.editable) { QSignalBlocker b(p->layers); item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked); return; }
        auto index = static_cast<std::uint32_t>(item->data(Qt::UserRole).toInt()); const bool on = item->checkState() == Qt::Checked; auto rev = p->info.revision;
        run(p, on ? "Showing layer" : "Hiding layer", [p, index, on, rev] { p->document->setLayerVisible(index, on, rev); }, [this, p] { render(p); });
    });
    connect(p->layers, &QListWidget::itemDoubleClicked, this, [this, p](QListWidgetItem* item) {
        if (p->busy || !p->info.editable || smoke_ || !item->data(Qt::UserRole).isValid()) return;
        bool ok = false; auto name = QInputDialog::getText(this, "Rename layer", "Layer name:", QLineEdit::Normal, item->text(), &ok).trimmed(); if (!ok || name.isEmpty()) return;
        auto index = static_cast<std::uint32_t>(item->data(Qt::UserRole).toInt()); auto rev = p->info.revision;
        run(p, "Renaming layer", [p, index, name, rev] { p->document->renameLayer(index, name.toStdString(), rev); });
    });
}
DocumentPane* Window::addPane(const QString& title) {
    auto p = new DocumentPane; tabs_->setCurrentIndex(tabs_->addTab(p, title));
    applyPanel(p); wirePanels(p);
    connect(p->pages, &QListWidget::currentRowChanged, this, [this, p](int row) { if (row >= 0 && !p->busy && !p->canvas->editing() && row != p->currentPage) { p->currentPage = row; render(p); } });
    auto showComment = [this, p](QListWidgetItem* item) {
        if (!item || !item->data(Qt::UserRole + 1).isValid() || p->busy) return;
        const int page = item->data(Qt::UserRole).toInt(), index = item->data(Qt::UserRole + 1).toInt();
        if (page == p->currentPage) { selectAnnotationByIndex(p, index); return; }
        p->setProperty("pendingAnnotation", index); p->pages->setCurrentRow(page);
    };
    connect(p->comments, &QListWidget::currentItemChanged, this, showComment);
    connect(p->comments, &QListWidget::itemClicked, this, showComment);
    connect(p->addComment, &QLineEdit::returnPressed, this, [this, p] {
        auto text = p->addComment->text().trimmed(); if (text.isEmpty()) return;
        for (auto a : tools_) if (a->text() == "Note" && a->isEnabled()) {
            p->setProperty("pendingCommentText", text); a->trigger(); p->addComment->clear();
            statusBar()->showMessage("Click on the page to place your comment"); return;
        }
        statusBar()->showMessage("This document is read-only, so comments can't be added.");
    });
    p->canvas->editRequested = [this, p](int index) { beginTextEdit(p, index); };
    p->canvas->areaDrawn = [this, p](QPointF a, QPointF b) {
        if (p->busy) return;
        AddAnnotation request; request.kind = toolKind_; request.x0 = a.x(); request.y0 = a.y(); request.x1 = b.x(); request.y1 = b.y();
        if (toolKind_ == AnnotationKind::FreeText) {
            if (smoke_) request.contents = "Smoke text";
            else {
                bool ok = false; auto text = QInputDialog::getMultiLineText(this, "Text box", "Text to place (plain ASCII):", {}, &ok);
                if (!ok || text.trimmed().isEmpty()) return;
                request.contents = text.toStdString();
            }
        }
        placeAnnotation(p, request);
    };
    p->canvas->redactionDrawn = [this, p](QPointF a, QPointF b) {
        if (p->busy || !p->info.editable) return;
        if (std::abs(a.x() - b.x()) < 2 || std::abs(a.y() - b.y()) < 2) return;
        p->canvas->redactionPage = p->info.pages[p->currentPage].id; p->canvas->redactionRevision = p->info.revision;
        p->canvas->redactions.emplace_back(a, b); p->canvas->update(); updateActions();
    };
    p->canvas->strokeDrawn = [this, p](std::vector<std::vector<QPointF>> strokes) {
        AddAnnotation request; request.kind = AnnotationKind::Ink; request.lineWidth = 2.5;
        for (const auto& stroke : strokes) { request.strokes.emplace_back(); for (const auto& q : stroke) request.strokes.back().push_back({q.x(), q.y()}); }
        placeAnnotation(p, request);
    };
    p->canvas->pointPicked = [this, p](QPointF at) {
        if (p->busy) return;
        AddAnnotation request; request.kind = AnnotationKind::Note; request.x0 = at.x(); request.y0 = at.y();
        if (smoke_) request.contents = "Smoke note";
        else if (auto pending = p->property("pendingCommentText").toString(); !pending.isEmpty()) { request.contents = pending.toStdString(); p->setProperty("pendingCommentText", QVariant()); }
        else {
            bool ok = false; auto text = QInputDialog::getMultiLineText(this, "Note", "Note text:", {}, &ok);
            if (!ok) return;
            request.contents = text.toStdString();
        }
        placeAnnotation(p, request);
    };
    p->canvas->annotationSelected = [this, p](int i) {
        updateActions();
        if (i < 0 || i >= static_cast<int>(p->canvas->annotations.size())) { p->canvas->setToolTip({}); return; }
        auto text = QString::fromStdString(p->canvas->annotations[i].contents).trimmed();
        p->canvas->setToolTip(text.toHtmlEscaped().replace("\n", "<br>")); if (!text.isEmpty()) statusBar()->showMessage(text.left(300).replace('\n', ' '));
        const int index = static_cast<int>(p->canvas->annotations[i].index);
        for (int row = 0; row < p->comments->count(); ++row) {
            auto item = p->comments->item(row);
            if (item->data(Qt::UserRole + 1).isValid() && item->data(Qt::UserRole).toInt() == p->currentPage && item->data(Qt::UserRole + 1).toInt() == index) {
                QSignalBlocker block(p->comments); p->comments->setCurrentRow(row); p->comments->scrollToItem(item); return;
            }
        }
    };
    p->canvas->annotationMoved = [this, p](int, QPointF d) {
        editAnnotation(p, [d](AddAnnotation& r) {
            r.x0 += d.x(); r.x1 += d.x(); r.y0 += d.y(); r.y1 += d.y();
            for (auto& stroke : r.strokes) for (auto& pt : stroke) { pt.x += d.x(); pt.y += d.y(); }
        });
    };
    p->canvas->annotationActivated = [this, p](int) {
        const int i = p->canvas->selectedAnnotation;
        if (i < 0 || i >= static_cast<int>(p->canvas->annotations.size())) return;
        const auto kind = p->canvas->annotations[i].kind;
        if (kind != AnnotationKind::Note && kind != AnnotationKind::FreeText) return;
        bool ok = false;
        auto text = QInputDialog::getMultiLineText(this, "Edit annotation", "Text:", QString::fromStdString(p->canvas->annotations[i].contents), &ok);
        if (ok && !text.isEmpty()) editAnnotation(p, [text](AddAnnotation& r) { r.contents = text.toStdString(); });
    };
    p->canvas->fieldClicked = [this, p](int index) { fillField(p, index); };
    for (auto key : {QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)}) {
        auto removal = new QShortcut(key, p->canvas); removal->setContext(Qt::WidgetShortcut);
        connect(removal, &QShortcut::activated, this, [this, p] { if (p->canvas->tool == TextCanvas::Tool::Select) removeSelectedAnnotation(p); });
    }
    connect(p->query, &QLineEdit::returnPressed, this, [this, p] { search(p); });
    connect(p->matches, &QListWidget::itemActivated, this, [p](QListWidgetItem* item) { if (!p->busy) p->pages->setCurrentRow(item->data(Qt::UserRole).toInt()); });
    auto deletion = new QShortcut(QKeySequence::Delete, p->pages); deletion->setContext(Qt::WidgetShortcut);
    connect(deletion, &QShortcut::activated, this, [this, p] { if (active() == p && delete_->isEnabled()) command(CommandKind::Delete); });
    return p;
}
std::array<double, 3> Window::annotationRgb() const {
    if (customColor_) return {customColor_->redF(), customColor_->greenF(), customColor_->blueF()};
    switch (toolKind_) {
    case AnnotationKind::Highlight: case AnnotationKind::Note: return {1, 0.9, 0.1};
    case AnnotationKind::FreeText: return {0, 0, 0};
    default: return {0.85, 0.1, 0.1};
    }
}
void Window::selectTool(DocumentPane* p, QAction* action) {
    if (!p) return;
    const int code = action->data().toInt();
    p->canvas->tool = static_cast<TextCanvas::Tool>(code / 100); toolKind_ = static_cast<AnnotationKind>(code % 100);
    if (p->canvas->editing()) cancelTextEdit(p);
    p->canvas->editMode = false; p->canvas->selectedAnnotation = -1; p->canvas->update();
    p->canvas->setCursor(p->canvas->tool == TextCanvas::Tool::Select || p->canvas->tool == TextCanvas::Tool::Form ? Qt::ArrowCursor : Qt::CrossCursor);
    statusBar()->showMessage(action->toolTip()); updateActions();
}
void Window::fillField(DocumentPane* p, int index) {
    if (!p || p->busy || index < 0 || index >= static_cast<int>(p->canvas->fields.size())) return;
    const auto field = p->canvas->fields[index];
    if (field.kind == FormFieldKind::Signature) { statusBar()->showMessage("Signature fields are not changed by FolioForge."); return; }
    if (field.readOnly || field.kind == FormFieldKind::Button) { statusBar()->showMessage("This field is read-only or is a button that needs scripts."); return; }
    SetFormValue request; request.page = p->info.pages[p->currentPage].id; request.expectedRevision = p->info.revision; request.widget = field.widget;
    auto apply = [this, p, request](SetFormValue value) {
        run(p, "Filling form", [p, value] { p->document->setFormValue(value); }, [this, p] { render(p); });
    };
    switch (field.kind) {
    case FormFieldKind::Checkbox: request.checked = !field.checked; apply(request); break;
    case FormFieldKind::Radio: request.checked = true; apply(request); break;
    case FormFieldKind::Choice: {
        QMenu menu;
        for (std::size_t i = 0; i < field.options.size() && i < 500; ++i) {
            auto a = menu.addAction(QString::fromStdString(field.options[i])); a->setData(static_cast<int>(i)); a->setCheckable(true); a->setChecked(field.optionValues[i] == field.value);
        }
        QAction* other = nullptr; QAction* clear = nullptr;
        if (field.editable) { menu.addSeparator(); other = menu.addAction("Other…"); }
        if (!field.value.empty()) clear = menu.addAction("Clear");
        auto chosen = menu.exec(p->canvas->mapToGlobal(p->canvas->fieldBox(field).bottomLeft().toPoint()));
        if (!chosen) return;
        if (chosen == other) {
            bool ok = false; auto text = QInputDialog::getText(this, "Value", "Value:", QLineEdit::Normal, QString::fromStdString(field.value), &ok);
            if (!ok) return; request.text = text.toStdString();
        } else if (chosen == clear) request.text.clear();
        else request.text = field.optionValues[chosen->data().toInt()];
        apply(request); break;
    }
    case FormFieldKind::Text: {
        if (field.multiline) {
            bool ok = false; auto text = QInputDialog::getMultiLineText(this, "Fill field", QString::fromStdString(field.name), QString::fromStdString(field.value), &ok);
            if (!ok) return; request.text = text.toStdString(); apply(request); break;
        }
        auto editor = new InlineEditor(p->canvas);
        auto rect = p->canvas->fieldBox(field).toRect();
        editor->setGeometry(rect.adjusted(-1, -1, 1, 1)); editor->setText(QString::fromStdString(field.value));
        if (field.password) editor->setEchoMode(QLineEdit::Password);
        if (field.maxLength > 0) editor->setMaxLength(field.maxLength);
        auto finish = [editor](bool) { editor->finished = true; editor->hide(); editor->deleteLater(); };
        editor->cancel = [finish] { finish(false); };
        editor->commit = [this, editor, finish, apply, request] {
            auto value = request; value.text = editor->text().toStdString(); finish(true); apply(value);
        };
        connect(editor, &QLineEdit::returnPressed, editor, [editor] { if (editor->commit) editor->commit(); });
        editor->show(); editor->setFocus(); editor->selectAll();
        break;
    }
    default: break;
    }
}
void Window::placeAnnotation(DocumentPane* p, AddAnnotation request) {
    if (!p || p->busy || !p->document || !p->info.editable) return;
    request.page = p->info.pages[p->currentPage].id; request.expectedRevision = p->info.revision; request.color = annotationRgb();
    run(p, "Adding annotation", [p, request] { p->document->addAnnotation(request); }, [this, p] { render(p); });
}
void Window::applyRedactions(DocumentPane* p) {
    if (!p || p->busy || !p->document || !p->info.editable || p->canvas->redactions.empty()) return;
    if (!smoke_ && QMessageBox::warning(this, "Apply redactions",
            "This page will be flattened into a picture with the marked areas blacked out. All text, links, notes and form data on the page are removed and cannot be selected afterwards.\n\nYou can undo until you save. Continue?",
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    const auto& info = p->info.pages[p->currentPage];
    const bool turned = info.rotation == 90 || info.rotation == 270;
    const double pageW = turned ? info.height : info.width, pageH = turned ? info.width : info.height;
    const double scale = std::min(200.0 / 72.0, std::sqrt(40e6 / (pageW * pageH)));
    const double zoom = scale / p->canvas->scale;
    std::vector<QRectF> boxes;
    for (const auto& box : p->canvas->redactions) boxes.push_back(QRectF(p->canvas->toView(box.first) * zoom, p->canvas->toView(box.second) * zoom).normalized());
    auto snapshot = p->document->snapshot(); int page = p->currentPage; auto id = info.id; auto revision = p->info.revision;
    p->canvas->redactions.clear(); p->canvas->update();
    run(p, "Redacting", [p, snapshot, page, id, revision, scale, boxes, renderer = renderer_] {
        auto image = imageOf(renderer->render(snapshot, page, scale)).convertToFormat(QImage::Format_RGB888);
        { QPainter painter(&image); for (const auto& box : boxes) painter.fillRect(box, Qt::black); }
        ImagePage raster; raster.width = image.width(); raster.height = image.height(); raster.components = 3;
        raster.data.reserve(static_cast<std::size_t>(image.width()) * image.height() * 3);
        for (int y = 0; y < image.height(); ++y) { auto line = image.constScanLine(y); raster.data.insert(raster.data.end(), line, line + image.width() * 3); }
        p->document->redactPage(id, revision, raster);
    }, [this, p] { render(p); });
}
void Window::signDocument(DocumentPane* p) {
    if (!p || p->busy || !p->document || !p->info.editable) return;
#ifndef __APPLE__
    QMessageBox::information(this, "Sign", "Digital signing is currently available on macOS only."); return;
#endif
    auto certificate = QFileDialog::getOpenFileName(this, "Choose your signing certificate (PKCS#12)", {}, "Certificates (*.p12 *.pfx)");
    if (certificate.isEmpty()) return;
    bool ok = false;
    auto password = QInputDialog::getText(this, "Certificate password", "Password:", QLineEdit::Password, {}, &ok); if (!ok) return;
    auto reason = QInputDialog::getText(this, "Signature", "Reason for signing (optional):", QLineEdit::Normal, {}, &ok); if (!ok) return;
    auto base = QFileInfo(p->property("suggestedName").toString().isEmpty() ? QString::fromStdString(p->info.path.filename().string()) : p->property("suggestedName").toString()).completeBaseName();
    auto out = QFileDialog::getSaveFileName(this, "Save signed copy", base.isEmpty() ? "signed.pdf" : base + "-signed.pdf", "PDF files (*.pdf)");
    if (out.isEmpty()) return;
    SignOptions options; options.certificate = certificate.toStdU16String(); options.password = password.toStdString(); options.reason = reason.toStdString();
    const std::filesystem::path target = out.toStdU16String();
    run(p, "Signing", [p, options, target] { p->document->signTo(target, options, true); }, [this, out] { statusBar()->showMessage("Signed copy saved to " + out + ". Opening it read-only.", 8000); openPath(out); });
}
void Window::editAnnotation(DocumentPane* p, std::function<void(AddAnnotation&)> change) {
    if (!p || p->busy || !p->document || !p->info.editable) return;
    const int selected = p->canvas->selectedAnnotation;
    if (selected < 0 || selected >= static_cast<int>(p->canvas->annotations.size())) return;
    const auto target = p->canvas->annotations[selected];
    AddAnnotation request; request.page = p->info.pages[p->currentPage].id; request.expectedRevision = p->info.revision; request.kind = target.kind;
    request.x0 = target.x0; request.y0 = target.y0; request.x1 = target.x1; request.y1 = target.y1; request.color = target.color;
    request.contents = target.contents; request.strokes = target.strokes; request.lineWidth = target.lineWidth; request.fontSize = target.fontSize;
    change(request);
    run(p, "Editing annotation", [p, request, index = target.index] { p->document->updateAnnotation(index, request); }, [this, p] { render(p); });
}
void Window::removeSelectedAnnotation(DocumentPane* p) {
    if (!p || p->busy || !p->document || !p->info.editable) return;
    const int selected = p->canvas->selectedAnnotation;
    if (selected < 0 || selected >= static_cast<int>(p->canvas->annotations.size())) return;
    const auto target = p->canvas->annotations[selected];
    if (!target.removable) { statusBar()->showMessage("This annotation came from another program and is preserved unchanged."); return; }
    auto id = p->info.pages[p->currentPage].id; auto revision = p->info.revision;
    run(p, "Removing annotation", [p, id, target, revision] { p->document->removeAnnotation(id, target.index, revision); }, [this, p] { render(p); });
}
void Window::newDocument() { auto p = addPane("Untitled.pdf"); run(p, "Creating PDF", [p] { p->document = Document::create(); }, [this, p] { render(p); }); }
void Window::mergeFiles(const std::vector<MergeItem>& items, const QString& name) {
    if (items.empty()) return;
    auto p = addPane(name); p->setProperty("suggestedName", name);
    run(p, "Merging files", [p, items] { p->document = assemble(items); }, [this, p] { render(p); },
        [this, p] { tabs_->removeTab(tabs_->indexOf(p)); p->deleteLater(); updateActions(); });
}
void Window::openImages(const QStringList& paths) {
    if (paths.isEmpty()) return;
    auto p = addPane(QFileInfo(paths.first()).completeBaseName() + ".pdf");
    p->setProperty("suggestedName", QFileInfo(paths.first()).completeBaseName() + ".pdf");
    run(p, "Converting images to PDF", [p, paths] {
        std::shared_ptr<Document> document;
        for (const auto& path : paths) {
            auto image = loadImage(path);
            if (!document) document = Document::createFromImage(image);
            else { auto info = document->info(); document->insertImage(image, info.pages.back().id, info.revision); }
        }
        p->document = document;
    }, [this, p] { render(p); }, [this, p] { tabs_->removeTab(tabs_->indexOf(p)); p->deleteLater(); updateActions(); });
}
void Window::openPath(const QString& path) {
    if (isImagePath(path)) { openImages({path}); return; }
    if (office::isOfficePath(path)) { mergeFiles({{path, {}}}, QFileInfo(path).completeBaseName() + ".pdf"); return; }
    auto p = addPane(QFileInfo(path).fileName()); p->setProperty("openingPath", path); load(p, path);
}
void Window::load(DocumentPane* p, const QString& path, const QString& password) {
    run(p, "Opening PDF", [p, path, password] { p->document = Document::open(localPath(path), password.toUtf8().toStdString()); }, [this, p] { render(p); });
}
void Window::run(DocumentPane* p, const QString& label, std::function<void()> work, std::function<void()> done, std::function<void()> failed) {
    if (!p || p->busy) return;
    p->busy = true; p->query->setEnabled(false); updateActions(); statusBar()->showMessage(label + "…");
    auto watcher = new QFutureWatcher<JobResult>(p);
    connect(watcher, &QFutureWatcher<JobResult>::finished, this, [this, p, watcher, done, failed] {
        auto result = watcher->result(); watcher->deleteLater(); p->busy = false; p->query->setEnabled(true);
        refresh(p); updateActions();
        if (!result.error.isEmpty()) {
            hadError_ = true; statusBar()->showMessage(result.error);
            if (result.password && !smoke_) {
                bool ok = false; auto password = QInputDialog::getText(this, "PDF password", result.error, QLineEdit::Password, {}, &ok);
                if (ok) load(p, p->property("openingPath").toString(), password);
            } else if (!smoke_) QMessageBox::warning(this, "FolioForge", result.error);
            if (failed) failed();
            return;
        }
        statusBar()->showMessage("Ready"); if (done) done();
    });
    watcher->setFuture(QtConcurrent::run(&worker_, [work = std::move(work)] {
        try { work(); return JobResult{}; }
        catch (const Error& e) { return JobResult{QString::fromUtf8(e.what()), e.code == ErrorCode::PasswordRequired}; }
        catch (const std::exception&) { return JobResult{"The operation failed. Your last committed document state is retained.", false}; }
    }));
}
void Window::selectAnnotationByIndex(DocumentPane* p, int index) {
    for (int i = 0; i < static_cast<int>(p->canvas->annotations.size()); ++i)
        if (static_cast<int>(p->canvas->annotations[i].index) == index) { p->canvas->selectedAnnotation = i; p->canvas->update(); updateActions(); if (p->canvas->annotationSelected) p->canvas->annotationSelected(i); return; }
}
void Window::refresh(DocumentPane* p) {
    if (!p->document) return;
    p->info = p->document->info(); p->currentPage = std::clamp(p->currentPage, 0, static_cast<int>(p->info.pages.size()) - 1);
    QSignalBlocker block(p->pages);
    // Retain raster thumbnails only while their document revision matches.
    auto oldRevision = p->pages->property("revision").toULongLong();
    if (oldRevision != p->info.revision || p->pages->count() != static_cast<int>(p->info.pages.size())) {
        p->pages->clear();
        for (std::size_t i = 0; i < p->info.pages.size(); ++i) {
            auto item = new QListWidgetItem(QString::number(i + 1));
            item->setTextAlignment(Qt::AlignHCenter); item->setIcon(style()->standardIcon(QStyle::SP_FileIcon)); p->pages->addItem(item);
        }
        p->pages->setProperty("revision", QVariant::fromValue<qulonglong>(p->info.revision));
        p->matches->clear();
        loadThumbnails(p);
    }
    if (p->comments->property("revision").toULongLong() != p->info.revision || !p->comments->property("built").toBool()) {
        p->comments->clear();
        static const char* names[] = {"Highlight", "Underline", "Strike-out", "Note", "Text box", "Pen", "Rectangle", "Ellipse", "Annotation"};
        int total = 0;
        for (std::size_t i = 0; i < p->info.pages.size(); ++i) {
            std::vector<Annotation> found;
            try { found = p->document->annotations(p->info.pages[i].id); } catch (const std::exception&) {}
            std::vector<QListWidgetItem*> cards;
            std::vector<bool> placed(found.size(), false);
            std::function<void(int, int)> place = [&](int parent, int depth) {
                for (std::size_t k = 0; k < found.size(); ++k) {
                    const auto& a = found[k];
                    if (placed[k] || a.parent != parent || depth > 8) continue;
                    placed[k] = true;
                    auto text = QString::fromStdString(a.contents).trimmed();
                    const bool shown = !(text.isEmpty() && a.kind != AnnotationKind::Note && a.kind != AnnotationKind::FreeText);
                    if (shown) {
                        auto title = QString::fromStdString(a.author); if (title.isEmpty()) title = names[static_cast<int>(a.kind)];
                        auto item = new QListWidgetItem;
                        item->setData(Qt::UserRole, static_cast<int>(i)); item->setData(Qt::UserRole + 1, static_cast<int>(a.index));
                        item->setData(Qt::UserRole + 2, title); item->setData(Qt::UserRole + 3, text); item->setData(Qt::UserRole + 5, depth);
                        item->setData(Qt::UserRole + 6, static_cast<int>(a.kind)); cards.push_back(item);
                    }
                    place(static_cast<int>(a.index), shown ? depth + 1 : depth);
                }
            };
            place(-1, 0);
            for (std::size_t k = 0; k < found.size(); ++k) if (!placed[k]) {
                const auto& a = found[k]; auto text = QString::fromStdString(a.contents).trimmed(); if (text.isEmpty()) continue;
                auto item = new QListWidgetItem; item->setData(Qt::UserRole, static_cast<int>(i)); item->setData(Qt::UserRole + 1, static_cast<int>(a.index));
                item->setData(Qt::UserRole + 2, QString::fromStdString(a.author)); item->setData(Qt::UserRole + 3, text); item->setData(Qt::UserRole + 5, 0);
                item->setData(Qt::UserRole + 6, static_cast<int>(a.kind)); cards.push_back(item);
            }
            if (cards.empty()) continue;
            auto header = new QListWidgetItem(QString("Page %1").arg(i + 1)); header->setFlags(Qt::ItemIsEnabled);
            header->setData(Qt::UserRole + 4, QString::number(cards.size())); p->comments->addItem(header);
            for (auto c : cards) p->comments->addItem(c);
            total += static_cast<int>(cards.size());
        }
        p->commentsTitle->setText(total ? QString("Comments  %1").arg(total) : "Comments");
        if (!total) { auto item = new QListWidgetItem("No comments yet. Type above to add one."); item->setFlags(Qt::ItemIsEnabled); item->setData(Qt::UserRole + 4, QString()); item->setData(Qt::DisplayRole, "No comments yet"); p->comments->addItem(item); }
        p->comments->setProperty("revision", QVariant::fromValue<qulonglong>(p->info.revision)); p->comments->setProperty("built", true);
    }
    if (p->bookmarks->property("revision").toULongLong() != p->info.revision || !p->bookmarks->property("built").toBool()) {
        QSignalBlocker blockBookmarks(p->bookmarks); p->bookmarks->clear();
        std::vector<Bookmark> found; try { found = p->document->bookmarks(); } catch (const std::exception&) {}
        for (const auto& b : found) {
            auto item = new QListWidgetItem(QString(b.level * 3, QChar(' ')) + QString::fromStdString(b.title));
            item->setData(Qt::UserRole, b.page); item->setData(Qt::UserRole + 1, static_cast<int>(b.index));
            item->setToolTip(b.page >= 0 ? QString("Page %1").arg(b.page + 1) : "Destination not on a page"); p->bookmarks->addItem(item);
        }
        if (found.empty()) { auto item = new QListWidgetItem("No bookmarks yet. Use + to bookmark the current page."); item->setFlags(Qt::NoItemFlags); p->bookmarks->addItem(item); }
        p->bookmarks->setProperty("revision", QVariant::fromValue<qulonglong>(p->info.revision)); p->bookmarks->setProperty("built", true);
    }
    if (p->layers->property("revision").toULongLong() != p->info.revision || !p->layers->property("built").toBool()) {
        QSignalBlocker blockLayers(p->layers); p->layers->clear();
        std::vector<Layer> found; try { found = p->document->layers(); } catch (const std::exception&) {}
        for (const auto& l : found) {
            auto item = new QListWidgetItem(QString::fromStdString(l.name));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable); item->setCheckState(l.visible ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, static_cast<int>(l.index)); p->layers->addItem(item);
        }
        if (found.empty()) { auto item = new QListWidgetItem("This document has no layers."); item->setFlags(Qt::NoItemFlags); p->layers->addItem(item); }
        p->layers->setProperty("revision", QVariant::fromValue<qulonglong>(p->info.revision)); p->layers->setProperty("built", true);
    }
    p->pages->setCurrentRow(p->currentPage);
    if (pageBox_ && active() == p) { pageBox_->setText(QString::number(p->currentPage + 1)); pageBox_->setToolTip(QString("Go to page (1–%1)").arg(p->info.pages.size())); }
    auto suggested = p->property("suggestedName").toString();
    auto name = p->info.path.empty() ? (suggested.isEmpty() ? "Untitled.pdf" : suggested) : QFileInfo(displayPath(p->info.path)).fileName();
    tabs_->setTabText(tabs_->indexOf(p), name + (p->info.dirty ? " *" : ""));
    QString notice = QString::fromStdString(p->info.restriction);
    p->notice->setToolTip(notice);
    if (notice.startsWith("This PDF contains")) notice = "Read-only: this PDF has forms, bookmarks, signatures, layers or similar structures that can't be safely edited yet. You can still read and search it. Hover for details.";
    if (p->info.historyPruned) notice += " Older undo history was discarded to stay within the memory budget.";
    p->notice->setText(notice); p->notice->setVisible(!notice.isEmpty());
    const auto& page = p->info.pages[p->currentPage];
    p->properties->setText(QString("Page %1 of %2\n\n%3 × %4 pt\nRotation: %5°\n\n%6")
        .arg(p->currentPage + 1).arg(p->info.pages.size()).arg(page.width, 0, 'f', 1).arg(page.height, 0, 'f', 1).arg(page.rotation)
        .arg(p->info.editable ? "Page organization available" : "Read-only document"));
}
void Window::render(DocumentPane* p, std::function<void()> done) {
    if (!p || p->busy || !p->document) return;
    auto result = std::make_shared<PageRender>();
    int page = p->currentPage; double scale = p->scale; double dpr = devicePixelRatioF(); auto snap = p->document->snapshot();
    // Never show the previous page as if it belonged to a newly selected revision.
    p->canvas->runs.clear(); p->canvas->annotations.clear(); p->canvas->fields.clear(); p->canvas->selectedAnnotation = -1; p->canvas->clear(); p->canvas->setText("Rendering page…"); p->text->clear(); p->image = {};
    run(p, "Rendering page", [p, snap, page, scale, dpr, result, renderer = renderer_] {
        result->image = imageOf(renderer->render(snap, page, scale * dpr)); result->image.setDevicePixelRatio(dpr);
        try { result->text = QString::fromStdU16String(renderer->text(snap, page)); }
        catch (const std::exception&) { result->text = "Text extraction is unavailable for this page. The rendered page remains viewable."; }
        try { result->annotations = p->document->annotations(snap.pages.at(page)); } catch (const std::exception&) {}
        try { result->fields = p->document->formFields(snap.pages.at(page)); } catch (const std::exception&) {}
        try { result->inventory = p->document->textRuns(snap.pages.at(page)); }
        catch (const std::exception&) { result->inventory.explanation = "Text analysis is unavailable for this page. Viewing remains available."; }
    }, [this, p, snap, page, result, done] {
        if (p->info.revision != snap.revision || p->currentPage != page) return;
        p->image = result->image; p->canvas->setPixmap(QPixmap::fromImage(p->image)); p->canvas->setFixedSize(p->image.deviceIndependentSize().toSize());
        p->textInventory = std::move(result->inventory); p->canvas->runs = p->textInventory.runs;
        p->canvas->annotations = std::move(result->annotations);
        if (auto pending = p->property("pendingAnnotation"); pending.isValid()) { p->setProperty("pendingAnnotation", QVariant()); selectAnnotationByIndex(p, pending.toInt()); }
        if (p->canvas->redactionPage != p->info.pages[page].id || p->canvas->redactionRevision != p->info.revision) p->canvas->redactions.clear();
        p->canvas->fields = std::move(result->fields);
        if (!p->canvas->fields.empty() && !p->canvas->formPrompted && p->info.editable && p->canvas->tool == TextCanvas::Tool::None && !p->canvas->editMode) {
            p->canvas->formPrompted = true;
            for (auto a : tools_) if (a->data().toInt() == static_cast<int>(TextCanvas::Tool::Form) * 100 + static_cast<int>(AnnotationKind::Other)) a->trigger();
        }
        p->canvas->pageWidth = p->info.pages[page].width; p->canvas->rotation = p->info.pages[page].rotation;
        p->canvas->selected = 0; p->canvas->scale = p->scale; p->canvas->pageHeight = p->info.pages[page].height;
        p->text->setPlainText(result->text); p->canvas->setAccessibleDescription(QString("Page %1. %2 editable text runs. Enable Edit text, use arrow keys to select, and Enter to edit. Extracted text is available in the Page text panel.").arg(page + 1).arg(p->canvas->runs.size()));
        p->canvas->setToolTip(QString::fromStdString(p->textInventory.explanation));
        p->properties->setText(p->properties->text() + QString("\n\nEditable text runs: %1").arg(p->canvas->runs.size()));
        if (p->canvas->runs.empty()) p->properties->setToolTip(QString::fromStdString(p->textInventory.explanation));
        else p->properties->setToolTip("Select Edit text, then click an outlined text run. Enter applies, Escape cancels.");
        p->pages->item(page)->setIcon(QPixmap::fromImage(p->image.scaled(240, 312, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        statusBar()->showMessage(QString("Page %1 of %2  ·  %3%  ·  %4").arg(page + 1).arg(p->info.pages.size()).arg(qRound(p->scale * 100)).arg(p->info.dirty ? "Unsaved changes" : "Saved"));
        updateActions();
        if (done) done();
    });
}
void Window::updateActions() {
    auto p = active(); bool ready = p && !p->busy && !p->canvas->editing() && p->document; bool editable = ready && p->info.editable;
    const bool editing = p && !p->busy && p->canvas->editing();
    const bool canSave = p && !p->busy && p->document && p->info.editable;
    applyText_->setEnabled(editing); cancelText_->setEnabled(editing);
    applyText_->setVisible(editing); cancelText_->setVisible(editing);
    editText_->setEnabled(ready);
    { QSignalBlocker block(editText_); editText_->setChecked(p && p->canvas->editMode); }
    for (auto a : {save_, saveAs_, insert_, merge_, insertImage_, duplicate_, left_, right_, earlier_, later_, delete_}) a->setEnabled(editable);
    save_->setEnabled(canSave && (p->info.dirty || editing)); saveAs_->setEnabled(canSave);
    undo_->setEnabled(editable && p->info.canUndo); redo_->setEnabled(editable && p->info.canRedo);
    delete_->setEnabled(editable && p->info.pages.size() > 1); earlier_->setEnabled(editable && p->currentPage > 0);
    later_->setEnabled(editable && p->currentPage + 1 < static_cast<int>(p->info.pages.size()));
    for (auto a : tools_) a->setEnabled(editable);
    annotationColor_->setEnabled(editable);
    signAction_->setEnabled(editable);
    applyRedactions_->setEnabled(editable && !p->canvas->redactions.empty());
    deleteAnnotation_->setEnabled(editable && p->canvas->selectedAnnotation >= 0);
    if (p) for (auto a : tools_) { QSignalBlocker block(a); a->setChecked(p->canvas->tool != TextCanvas::Tool::None && a->data().toInt() == static_cast<int>(p->canvas->tool) * 100 + static_cast<int>(toolKind_)); }
    for (auto a : {exportImage_, exportText_, find_}) a->setEnabled(ready);
    zoom_->setEnabled(ready); if (zoomIn_) { zoomIn_->setEnabled(ready); zoomOut_->setEnabled(ready); zoomInButton_->setEnabled(ready); zoomOutButton_->setEnabled(ready); }
    if (ready) zoom_->setCurrentText(QString("%1%").arg(qRound(p->scale * 100)));
}
void Window::command(CommandKind kind) {
    auto p = active(); if (!p || p->busy || !p->document) return;
    Command command{kind, p->info.pages[p->currentPage].id, p->info.revision};
    run(p, "Updating pages", [p, command] { p->document->execute(command); }, [this, p, kind] {
        if (kind == CommandKind::InsertBlank || kind == CommandKind::Duplicate || kind == CommandKind::MoveLater) ++p->currentPage;
        if (kind == CommandKind::MoveEarlier) --p->currentPage;
        refresh(p); render(p);
    });
}
void Window::save(DocumentPane* p, bool saveAs, std::function<void()> done) {
    if (!p || p->busy || !p->document) return;
    if (p->canvas->editing()) {
        const int index = p->canvas->editor->property("runIndex").toInt();
        commitTextEdit(p, index, [this, p, saveAs, done] { save(p, saveAs, done); });
        return;
    }
    QString path = displayPath(p->info.path); bool overwrite = !saveAs && !path.isEmpty();
    if (saveAs || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, "Save PDF", path.isEmpty() ? (p->property("suggestedName").toString().isEmpty() ? "Untitled.pdf" : p->property("suggestedName").toString()) : path, "PDF files (*.pdf)", nullptr, QFileDialog::DontConfirmOverwrite);
        if (path.isEmpty()) return; if (!path.endsWith(".pdf", Qt::CaseInsensitive)) path += ".pdf";
        // Confirm the final path once, after normalizing the extension.
        if (QFileInfo::exists(path)) {
            if (QMessageBox::question(this, "Replace PDF?", "Replace the existing file with this edited PDF?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
            overwrite = true;
        }
    }
    run(p, "Validating and saving PDF", [p, path, overwrite, renderer = renderer_] { renderer->validate(p->document->snapshot()); p->document->save(localPath(path), overwrite); }, [this, done] { statusBar()->showMessage("PDF saved and validated", 6000); if (done) done(); });
}
void Window::closeTab(int index) {
    auto p = dynamic_cast<DocumentPane*>(tabs_->widget(index)); if (!p || p->busy) return;
    if (p->canvas->editing()) { statusBar()->showMessage("Press Enter to apply your text edit or Escape to cancel before closing."); p->canvas->editor->setFocus(); return; }
    if (p->info.dirty) {
        auto answer = QMessageBox::warning(this, "Unsaved PDF", "Save changes before closing this document?", QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel) return;
        if (answer == QMessageBox::Save) { save(p, false, [this, p] { clearRecovery(p); tabs_->removeTab(tabs_->indexOf(p)); p->deleteLater(); updateActions(); }); return; }
    }
    clearRecovery(p); tabs_->removeTab(index); p->deleteLater(); updateActions();
}
QDir Window::recoveryDir() const {
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/recovery"); dir.mkpath("."); return dir;
}
void Window::clearRecovery(DocumentPane* p) {
    auto id = p->property("recoveryId").toString(); if (id.isEmpty()) return;
    auto dir = recoveryDir(); QFile::remove(dir.filePath(id + ".pdf")); QFile::remove(dir.filePath(id + ".json")); p->setProperty("recoveryId", QVariant());
}
// Periodically snapshots every unsaved document so edits survive a crash.
void Window::saveRecovery() {
    auto dir = recoveryDir();
    for (int i = 0; i < tabs_->count(); ++i) {
        auto p = dynamic_cast<DocumentPane*>(tabs_->widget(i)); if (!p || p->busy || !p->document) continue;
        if (!p->info.dirty) { clearRecovery(p); continue; }
        auto revision = QString::number(p->info.revision);
        if (p->property("recoveryRevision").toString() == revision && !p->property("recoveryId").toString().isEmpty()) continue;
        try {
            auto snapshot = p->document->snapshot(); if (!snapshot.bytes) continue;
            auto id = p->property("recoveryId").toString();
            if (id.isEmpty()) id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            pdfengine::atomicWrite(localPath(dir.filePath(id + ".pdf")), *snapshot.bytes, true);
            QFile meta(dir.filePath(id + ".json"));
            if (meta.open(QIODevice::WriteOnly | QIODevice::Truncate))
                meta.write(QJsonDocument(QJsonObject{{"name", tabs_->tabText(i)}, {"path", displayPath(p->info.path)}}).toJson());
            p->setProperty("recoveryId", id); p->setProperty("recoveryRevision", revision);
        } catch (const std::exception&) {}
    }
}
void Window::offerRecovery() {
    auto dir = recoveryDir();
    for (const auto& entry : dir.entryInfoList({"*.json"}, QDir::Files)) {
        QFile meta(entry.absoluteFilePath()); if (!meta.open(QIODevice::ReadOnly)) continue;
        auto object = QJsonDocument::fromJson(meta.readAll()).object(); meta.close();
        auto pdf = dir.filePath(entry.completeBaseName() + ".pdf");
        if (!QFileInfo::exists(pdf)) { QFile::remove(entry.absoluteFilePath()); continue; }
        auto name = object["name"].toString("Untitled.pdf"); auto original = object["path"].toString();
        auto answer = QMessageBox::question(this, "Recover unsaved work", "FolioForge found unsaved changes to \"" + name + "\" from a previous session. Restore them?", QMessageBox::Yes | QMessageBox::Discard, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            auto base = original.isEmpty() ? QDir::homePath() + "/" + QFileInfo(name).completeBaseName() : QFileInfo(original).absolutePath() + "/" + QFileInfo(original).completeBaseName();
            auto target = QFileDialog::getSaveFileName(this, "Save recovered PDF as", base + "-recovered.pdf", "PDF files (*.pdf)");
            if (target.isEmpty()) continue;
            QFile::remove(target);
            if (!QFile::copy(pdf, target)) { QMessageBox::warning(this, "FolioForge", "Could not write the recovered file."); continue; }
            openPath(target);
        }
        QFile::remove(pdf); QFile::remove(entry.absoluteFilePath());
    }
}
void Window::closeEvent(QCloseEvent* event) {
    if (busy()) { statusBar()->showMessage("Wait for the current operation to finish before closing."); event->ignore(); return; }
    for (int i = 0; i < tabs_->count(); ++i) if (auto p = dynamic_cast<DocumentPane*>(tabs_->widget(i)); p && p->canvas->editing()) {
        tabs_->setCurrentWidget(p); p->canvas->editor->setFocus(); statusBar()->showMessage("Press Enter to apply your text edit or Escape to cancel before closing."); event->ignore(); return;
    }
    for (int i = tabs_->count() - 1; i >= 0; --i) {
        auto p = dynamic_cast<DocumentPane*>(tabs_->widget(i)); if (p && p->info.dirty) {
            tabs_->setCurrentWidget(p); int count = tabs_->count(); closeTab(i);
            if (tabs_->count() == count) { event->ignore(); return; }
        }
    }
    if (!smoke_) { QSettings settings; settings.setValue("windowGeometry", saveGeometry()); }
    event->accept();
}
void Window::dragEnterEvent(QDragEnterEvent* event) { if (event->mimeData()->hasUrls()) event->acceptProposedAction(); }
void Window::dropEvent(QDropEvent* event) {
    QStringList images;
    for (const auto& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        auto path = url.toLocalFile();
        if (path.endsWith(".pdf", Qt::CaseInsensitive) || office::isOfficePath(path)) openPath(path); else if (isImagePath(path)) images << path;
    }
    openImages(images); event->acceptProposedAction();
}
void Window::search(DocumentPane* p) {
    if (p->busy || !p->document || p->query->text().trimmed().isEmpty()) return;
    auto result = std::make_shared<QList<QPair<int, QString>>>(); auto query = p->query->text(); auto snapshot = p->document->snapshot();
    run(p, "Searching PDF", [snapshot, query, result, renderer = renderer_] {
        for (std::size_t i = 0; i < snapshot.pages.size(); ++i) {
            auto text = QString::fromStdU16String(renderer->text(snapshot, i)); auto offset = text.indexOf(query, 0, Qt::CaseInsensitive);
            if (offset >= 0) result->append({static_cast<int>(i), text.mid(std::max<qsizetype>(0, offset - 35), 120).simplified()});
        }
    }, [this, p, result] {
        p->matches->clear();
        for (const auto& item : *result) { auto row = new QListWidgetItem(QString("Page %1\n%2").arg(item.first + 1).arg(item.second), p->matches); row->setData(Qt::UserRole, item.first); }
        statusBar()->showMessage(QString("Found text on %1 pages. Activate a result to navigate.").arg(result->size()));
    });
}
void Window::exportImage() {
    auto p = active(); if (!p || p->busy) return;
    auto path = exportPath(this, "Export current page at 144 DPI", "png", "PNG image (*.png)"); if (path.isEmpty()) return;
    auto snapshot = p->document->snapshot(); int page = p->currentPage;
    run(p, "Exporting PNG", [snapshot, page, path, renderer = renderer_] {
        auto image = imageOf(renderer->render(snapshot, page, 2)); image.setDotsPerMeterX(5669); image.setDotsPerMeterY(5669);
        QSaveFile output(path); output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit()) throw Error(ErrorCode::SaveFailed, "The PNG could not be saved.");
    });
}
void Window::exportText() {
    auto p = active(); if (!p || p->busy) return;
    auto path = exportPath(this, "Export UTF-8 document text", "txt", "Text (*.txt)"); if (path.isEmpty()) return;
    auto snapshot = p->document->snapshot();
    run(p, "Exporting text", [snapshot, path, renderer = renderer_] {
        QSaveFile output(path); output.setDirectWriteFallback(false); if (!output.open(QIODevice::WriteOnly)) throw Error(ErrorCode::SaveFailed, "The text output could not be opened.");
        for (std::size_t i = 0; i < snapshot.pages.size(); ++i) {
            auto bytes = QString::fromStdU16String(renderer->text(snapshot, i)).toUtf8() + "\n\f\n";
            if (output.write(bytes) != bytes.size()) throw Error(ErrorCode::SaveFailed, "The text output could not be written.");
        }
        if (!output.commit()) throw Error(ErrorCode::SaveFailed, "The text output could not be saved.");
    });
}
void Window::beginTextEdit(DocumentPane* p, int index, const QString* retry) {
    if (!p || p->busy || p->canvas->editing() || index < 0 || index >= static_cast<int>(p->canvas->runs.size())) return;
    const auto& run = p->canvas->runs[index];
    if (run.revision != p->info.revision) { render(p); return; }
    if (p->canvas->editor) { p->canvas->editor->deleteLater(); p->canvas->editor = nullptr; }
    auto editor = new InlineEditor(p->canvas); p->canvas->editor = editor;
    editor->setProperty("runIndex", index);
    editor->setAccessibleName("Edit PDF text in place"); editor->setMaxLength(4096);
    editor->setValidator(new QRegularExpressionValidator(QRegularExpression("[\\x{20}-\\x{7E}]*"), editor));
    editor->setText(retry ? *retry : QString::fromStdString(run.text));
    QFont font(run.font.starts_with("Courier") ? "Courier New" : "Arial");
    font.setPixelSize(std::max(6, qRound(run.fontSize * p->scale))); font.setBold(run.font.find("Bold") != std::string::npos);
    font.setItalic(run.font.find("Oblique") != std::string::npos); editor->setFont(font);
    editor->setStyleSheet("QLineEdit { background: white; color: #111; border: none; border-bottom: 1px solid #1769E8; padding: 0px; selection-background-color: #B5D3FF; selection-color: #111; }");
    // Place the widget so its text baseline and left edge coincide with the PDF run, then grow it as the user types.
    const QFontMetrics metrics(font);
    const int height = metrics.height() + 4, left = qRound(run.x * p->scale) - 2;
    const int top = qRound((p->canvas->pageHeight - run.baseline) * p->scale) - (height - metrics.height()) / 2 - metrics.ascent();
    const int minimum = std::max(qRound(run.width * p->scale) + 12, 24), limit = std::max(minimum, p->canvas->width() - left);
    auto fit = [editor, metrics, left, top, height, minimum, limit] {
        editor->setGeometry(left, top, std::min(std::max(metrics.horizontalAdvance(editor->text()) + 12, minimum), limit), height);
    };
    fit(); connect(editor, &QLineEdit::textChanged, editor, fit);
    editor->setToolTip("Type to edit. Enter or clicking elsewhere applies, Escape cancels. Save (Ctrl+S) applies and saves.");
    editor->cancel = [this, p] { cancelTextEdit(p); };
    editor->commit = [this, p, index] { commitTextEdit(p, index); };
    connect(editor, &QLineEdit::returnPressed, this, [this, p, index] { commitTextEdit(p, index); });
    p->query->setEnabled(false); p->matches->setEnabled(false);
    editor->show(); editor->raise(); editor->setFocus();
    if (retry || p->canvas->pressPoint.x() < 0) editor->setCursorPosition(editor->text().size());
    else editor->setCursorPosition(editor->cursorPositionAt(editor->mapFrom(p->canvas, p->canvas->pressPoint.toPoint())));
    p->canvas->pressPoint = {-1, -1}; p->canvas->update(); updateActions();
    statusBar()->showMessage("Type to edit · Enter or click elsewhere applies · Escape cancels · Ctrl+S applies and saves");
}
void Window::cancelTextEdit(DocumentPane* p) {
    if (!p || !p->canvas->editor) return;
    auto editor = p->canvas->editor; p->canvas->editor = nullptr; editor->finished = true; editor->hide(); editor->deleteLater();
    p->query->setEnabled(true); p->matches->setEnabled(true);
    p->canvas->setFocus(); p->canvas->update(); updateActions(); statusBar()->showMessage("Text edit cancelled");
}
void Window::commitTextEdit(DocumentPane* p, int index, std::function<void()> done) {
    if (!p || p->busy || !p->canvas->editing() || index < 0 || index >= static_cast<int>(p->canvas->runs.size())) return;
    auto input = p->canvas->editor->text(); auto run = p->canvas->runs[index];
    cancelTextEdit(p);
    if (input.toStdString() == run.text) return;
    ReplaceText request{run.page, run.id, run.revision, input.toUtf8().toStdString()};
    this->run(p, "Applying text edit", [p, request] { p->document->replaceText(request); }, [this, p, done] { render(p, done); },
        [this, p, index, input] { beginTextEdit(p, index, &input); });
}
