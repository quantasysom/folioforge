#include "window.h"
#include "text_canvas.h"
#include "merge_dialog.h"
#include "office_convert.h"
#include "pdfengine/render_service.h"
#include <QtWidgets>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <algorithm>
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
DocumentPane::DocumentPane(QWidget* parent) : QWidget(parent) {
    auto outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0); outer->setSpacing(0);
    notice = new QLabel; notice->setWordWrap(true); notice->setMargin(10); notice->hide();
    notice->setStyleSheet("background:#FFF5D8;color:#664500;"); outer->addWidget(notice);
    auto split = new QSplitter; outer->addWidget(split);
    auto navigation = new QTabWidget; navigation->setMinimumWidth(180);
    pages = new QListWidget; pages->setAccessibleName("Document pages"); pages->setSpacing(6);
    pages->setIconSize(QSize(76, 96)); navigation->addTab(pages, "Pages");
    auto searchPane = new QWidget; auto searchLayout = new QVBoxLayout(searchPane);
    query = new QLineEdit; query->setPlaceholderText("Search text, then press Enter"); query->setAccessibleName("Find in document");
    matches = new QListWidget; matches->setWordWrap(true); matches->setAccessibleName("Search results");
    searchLayout->addWidget(query); searchLayout->addWidget(matches); navigation->addTab(searchPane, "Search");
    split->addWidget(navigation);
    scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setAlignment(Qt::AlignCenter);
    scroll->setStyleSheet("QScrollArea {background:#E8ECF2;border:0;} QScrollArea > QWidget > QWidget {background:#E8ECF2;}");
    auto stage = new QWidget; auto stageLayout = new QVBoxLayout(stage); stageLayout->setContentsMargins(28, 28, 28, 28);
    canvas = new TextCanvas("Opening document…"); canvas->setAlignment(Qt::AlignCenter); canvas->setAccessibleName("Rendered PDF page");
    canvas->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed); stageLayout->addWidget(canvas, 0, Qt::AlignCenter);
    scroll->setWidget(stage); split->addWidget(scroll);
    auto inspector = new QWidget; inspector->setMinimumWidth(210); auto propertiesLayout = new QVBoxLayout(inspector);
    auto title = new QLabel("Page properties"); QFont heading = title->font(); heading.setPointSize(12); heading.setBold(true); title->setFont(heading);
    properties = new QLabel; properties->setWordWrap(true); properties->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text = new QPlainTextEdit; text->setReadOnly(true); text->setPlaceholderText("Extracted page text appears here."); text->setAccessibleName("Accessible extracted page text");
    propertiesLayout->addWidget(title); propertiesLayout->addWidget(properties); propertiesLayout->addSpacing(16);
    propertiesLayout->addWidget(new QLabel("Page text")); propertiesLayout->addWidget(text, 1);
    auto scope = new QLabel("Preview · Local files only\n\nClick-to-type text editing (including embedded fonts and non-Latin text) and Annotate tools (highlight, underline, strike-out, shapes, pen, notes, text boxes) are available. Fill form fills text, check box, radio and drop-down fields (scripts are not run; signature fields are untouched). Signing and redaction are not yet available.");
    scope->setWordWrap(true); scope->setStyleSheet("color:#596579;font-size:11px;"); propertiesLayout->addWidget(scope);
    split->addWidget(inspector); split->setStretchFactor(1, 1); split->setSizes({220, 850, 260});
}
Window::Window(std::shared_ptr<RenderService> renderer, bool smoke) : renderer_(std::move(renderer)), smoke_(smoke) {
    worker_.setMaxThreadCount(1);
    setWindowTitle("FolioForge · PDF editor preview"); resize(1440, 900); setMinimumSize(1024, 700); setAcceptDrops(true);
    tabs_ = new QTabWidget; tabs_->setTabsClosable(true); tabs_->setMovable(true); tabs_->setDocumentMode(true); setCentralWidget(tabs_);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &Window::closeTab);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] { updateActions(); });
    auto file = menuBar()->addMenu("&File"); auto edit = menuBar()->addMenu("&Edit"); auto page = menuBar()->addMenu("&Pages");
    auto view = menuBar()->addMenu("&View"); auto help = menuBar()->addMenu("&Help");
    auto toolbar = addToolBar("Document"); toolbar->setMovable(false); toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->setIconSize(QSize(20, 20));
    auto action = [this](QMenu* menu, const QString& label, const QKeySequence& shortcut, auto callback) {
        auto a = menu->addAction(label); a->setShortcut(shortcut); connect(a, &QAction::triggered, this, callback); return a;
    };
    auto create = action(file, "&New PDF", QKeySequence::New, [this] { newDocument(); });
    create->setIcon(style()->standardIcon(QStyle::SP_FileIcon)); toolbar->addAction(create);
    auto open = action(file, "&Open PDF…", QKeySequence::Open, [this] {
        auto paths = QFileDialog::getOpenFileNames(this, "Open PDF or image", {}, "PDF, images and Office files (*.pdf *.png *.jpg *.jpeg *.docx *.doc *.odt *.rtf *.txt *.xlsx *.xls *.ods *.pptx *.ppt *.odp);;PDF files (*.pdf);;Images (*.png *.jpg *.jpeg)");
        for (auto& path : paths) openPath(path);
    });
    open->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton)); toolbar->addAction(open);
    action(file, "&Merge files into new PDF…", QKeySequence("Ctrl+Shift+M"), [this] {
        MergeDialog dialog(this); if (dialog.exec() == QDialog::Accepted) mergeFiles(dialog.items());
    });
    action(file, "Images to new PDF…", QKeySequence("Ctrl+Shift+I"), [this] {
        auto paths = QFileDialog::getOpenFileNames(this, "Convert images to one PDF (one image per page)", {}, "Images (*.png *.jpg *.jpeg)"); openImages(paths);
    });
    file->addSeparator();
    save_ = action(file, "&Save", QKeySequence::Save, [this] { save(active(), false); });
    save_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton)); toolbar->addAction(save_);
    saveAs_ = action(file, "Save &As…", QKeySequence::SaveAs, [this] { save(active(), true); });
    toolbar->addAction(saveAs_);
    save_->setToolTip("Apply any active text edit and save the PDF (Ctrl+S)");
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
    toolbar->addAction(applyText_); toolbar->addAction(cancelText_);
    find_ = action(edit, "&Find…", QKeySequence::Find, [this] { if (auto p = active()) { auto nav = qobject_cast<QTabWidget*>(p->query->parentWidget()->parentWidget()->parentWidget()); if (nav) nav->setCurrentIndex(1); p->query->setFocus(); p->query->selectAll(); } });
    auto annotate = menuBar()->addMenu("&Annotate");
    auto annotationBar = addToolBar("Annotate"); annotationBar->setMovable(false); annotationBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
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
        {"Fill form", AnnotationKind::Other, TextCanvas::Tool::Form, "Click a form field to fill it. Scripts are not run; signature fields are untouched."}};
    for (const auto& spec : specs) {
        auto a = new QAction(spec.label, this); a->setCheckable(true); a->setToolTip(spec.tip); a->setStatusTip(spec.tip);
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
    deleteAnnotation_ = action(annotate, "Delete selected annotation", {}, [this] { removeSelectedAnnotation(active()); });
    annotationBar->addSeparator(); annotationBar->addAction(annotationColor_); annotationBar->addAction(deleteAnnotation_);
    auto organize = addToolBar("Organize pages"); organize->setMovable(false); addToolBarBreak(); addToolBar(Qt::TopToolBarArea, organize);
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
    toolbar->addAction(prev); toolbar->addAction(next);
    zoom_ = new QComboBox; zoom_->addItems({"50%", "75%", "100%", "125%", "150%", "200%", "Fit width", "Fit page"}); zoom_->setCurrentText("100%");
    zoom_->setAccessibleName("Page zoom"); toolbar->addWidget(zoom_);
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
        return 1;
#else
        return 1;
#endif
    default:
        return 1;
    }
    return 0;
}
DocumentPane* Window::addPane(const QString& title) {
    auto p = new DocumentPane; tabs_->setCurrentIndex(tabs_->addTab(p, title));
    connect(p->pages, &QListWidget::currentRowChanged, this, [this, p](int row) { if (row >= 0 && !p->busy && !p->canvas->editing()) { p->currentPage = row; render(p); } });
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
    p->canvas->strokeDrawn = [this, p](std::vector<std::vector<QPointF>> strokes) {
        AddAnnotation request; request.kind = AnnotationKind::Ink; request.lineWidth = 2.5;
        for (const auto& stroke : strokes) { request.strokes.emplace_back(); for (const auto& q : stroke) request.strokes.back().push_back({q.x(), q.y()}); }
        placeAnnotation(p, request);
    };
    p->canvas->pointPicked = [this, p](QPointF at) {
        if (p->busy) return;
        AddAnnotation request; request.kind = AnnotationKind::Note; request.x0 = at.x(); request.y0 = at.y();
        if (smoke_) request.contents = "Smoke note";
        else {
            bool ok = false; auto text = QInputDialog::getMultiLineText(this, "Note", "Note text:", {}, &ok);
            if (!ok) return;
            request.contents = text.toStdString();
        }
        placeAnnotation(p, request);
    };
    p->canvas->annotationSelected = [this](int) { updateActions(); };
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
void Window::editAnnotation(DocumentPane* p, std::function<void(AddAnnotation&)> change) {
    if (!p || p->busy || !p->document || !p->info.editable) return;
    const int selected = p->canvas->selectedAnnotation;
    if (selected < 0 || selected >= static_cast<int>(p->canvas->annotations.size())) return;
    const auto target = p->canvas->annotations[selected];
    if (!target.removable) { statusBar()->showMessage("This annotation came from another program and is preserved unchanged."); return; }
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
    p->busy = true; p->pages->setEnabled(false); p->query->setEnabled(false); updateActions(); statusBar()->showMessage(label + "…");
    auto watcher = new QFutureWatcher<JobResult>(p);
    connect(watcher, &QFutureWatcher<JobResult>::finished, this, [this, p, watcher, done, failed] {
        auto result = watcher->result(); watcher->deleteLater(); p->busy = false; p->pages->setEnabled(true); p->query->setEnabled(true);
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
void Window::refresh(DocumentPane* p) {
    if (!p->document) return;
    p->info = p->document->info(); p->currentPage = std::clamp(p->currentPage, 0, static_cast<int>(p->info.pages.size()) - 1);
    QSignalBlocker block(p->pages);
    // Retain raster thumbnails only while their document revision matches.
    auto oldRevision = p->pages->property("revision").toULongLong();
    if (oldRevision != p->info.revision || p->pages->count() != static_cast<int>(p->info.pages.size())) {
        p->pages->clear();
        for (std::size_t i = 0; i < p->info.pages.size(); ++i) {
            auto item = new QListWidgetItem(QString("Page %1\n%2 × %3 pt").arg(i + 1).arg(p->info.pages[i].width, 0, 'f', 0).arg(p->info.pages[i].height, 0, 'f', 0));
            item->setSizeHint(QSize(170, 106)); item->setIcon(style()->standardIcon(QStyle::SP_FileIcon)); p->pages->addItem(item);
        }
        p->pages->setProperty("revision", QVariant::fromValue<qulonglong>(p->info.revision));
        p->matches->clear();
    }
    p->pages->setCurrentRow(p->currentPage);
    auto suggested = p->property("suggestedName").toString();
    auto name = p->info.path.empty() ? (suggested.isEmpty() ? "Untitled.pdf" : suggested) : QFileInfo(displayPath(p->info.path)).fileName();
    tabs_->setTabText(tabs_->indexOf(p), name + (p->info.dirty ? " *" : ""));
    QString notice = QString::fromStdString(p->info.restriction);
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
        p->pages->item(page)->setIcon(QPixmap::fromImage(p->image.scaled(76, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
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
    deleteAnnotation_->setEnabled(editable && p->canvas->selectedAnnotation >= 0);
    if (p) for (auto a : tools_) { QSignalBlocker block(a); a->setChecked(p->canvas->tool != TextCanvas::Tool::None && a->data().toInt() == static_cast<int>(p->canvas->tool) * 100 + static_cast<int>(toolKind_)); }
    for (auto a : {exportImage_, exportText_, find_}) a->setEnabled(ready);
    zoom_->setEnabled(ready);
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
        if (answer == QMessageBox::Save) { save(p, false, [this, p] { tabs_->removeTab(tabs_->indexOf(p)); p->deleteLater(); updateActions(); }); return; }
    }
    tabs_->removeTab(index); p->deleteLater(); updateActions();
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
    p->pages->setEnabled(false); p->query->setEnabled(false); p->matches->setEnabled(false);
    editor->show(); editor->raise(); editor->setFocus();
    if (retry || p->canvas->pressPoint.x() < 0) editor->setCursorPosition(editor->text().size());
    else editor->setCursorPosition(editor->cursorPositionAt(editor->mapFrom(p->canvas, p->canvas->pressPoint.toPoint())));
    p->canvas->pressPoint = {-1, -1}; p->canvas->update(); updateActions();
    statusBar()->showMessage("Type to edit · Enter or click elsewhere applies · Escape cancels · Ctrl+S applies and saves");
}
void Window::cancelTextEdit(DocumentPane* p) {
    if (!p || !p->canvas->editor) return;
    auto editor = p->canvas->editor; p->canvas->editor = nullptr; editor->finished = true; editor->hide(); editor->deleteLater();
    p->pages->setEnabled(true); p->query->setEnabled(true); p->matches->setEnabled(true);
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
