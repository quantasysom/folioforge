#pragma once
#include "merge_dialog.h"
#include "pdfengine/document.h"
#include "pdfengine/render_service.h"
#include <QMainWindow>
#include <QColor>
#include <QImage>
#include <optional>
#include <QThreadPool>
#include <array>
#include <functional>
class QTabWidget;
class QListWidget;
class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QLineEdit;
class QComboBox;
class TextCanvas;

class DocumentPane : public QWidget {
public:
    explicit DocumentPane(QWidget* parent = nullptr);
    std::shared_ptr<pdfengine::Document> document;
    pdfengine::DocumentInfo info;
    QListWidget *pages{}, *matches{};
    TextCanvas* canvas{};
    QLabel *properties{}, *notice{};
    QPlainTextEdit* text{};
    QScrollArea* scroll{};
    QLineEdit* query{};
    QImage image;
    int currentPage{};
    double scale{1.0};
    bool busy{};
    pdfengine::TextInventory textInventory;
};
class Window : public QMainWindow {
public:
    explicit Window(std::shared_ptr<pdfengine::RenderService> renderer, bool smoke = false);
    void openPath(const QString& path);
    bool busy() const;
    bool smokeReady() const;
    int advanceSmokeTest();
    bool hadError() const { return hadError_; }
protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    QTabWidget* tabs_{};
    std::shared_ptr<pdfengine::RenderService> renderer_;
    QThreadPool worker_;
    QComboBox* zoom_{};
    QAction *save_{}, *saveAs_{}, *undo_{}, *redo_{}, *delete_{}, *insert_{}, *merge_{}, *insertImage_{}, *duplicate_{},
            *left_{}, *right_{}, *earlier_{}, *later_{}, *exportImage_{}, *exportText_{}, *find_{}, *editText_{}, *applyText_{}, *cancelText_{};
    std::vector<QAction*> tools_;
    QAction *deleteAnnotation_{}, *annotationColor_{};
    pdfengine::AnnotationKind toolKind_{pdfengine::AnnotationKind::Highlight};
    std::optional<QColor> customColor_;
    bool smoke_{}, hadError_{};
    int smokeStep_{};
    DocumentPane* active() const;
    DocumentPane* addPane(const QString&);
    void newDocument();
    void openImages(const QStringList&);
    void mergeFiles(const std::vector<MergeItem>&, const QString& name = "Merged.pdf");
    void load(DocumentPane*, const QString&, const QString& password = {});
    void run(DocumentPane*, const QString&, std::function<void()>, std::function<void()> done = {}, std::function<void()> failed = {});
    void refresh(DocumentPane*);
    void render(DocumentPane*, std::function<void()> done = {});
    void updateActions();
    void command(pdfengine::CommandKind);
    void save(DocumentPane*, bool saveAs, std::function<void()> done = {});
    void closeTab(int);
    void selectTool(DocumentPane*, QAction*);
    std::array<double, 3> annotationRgb() const;
    void placeAnnotation(DocumentPane*, pdfengine::AddAnnotation);
    void removeSelectedAnnotation(DocumentPane*);
    void fillField(DocumentPane*, int);
    void search(DocumentPane*);
    void exportImage();
    void exportText();
    void beginTextEdit(DocumentPane*, int index, const QString* retry = nullptr);
    void cancelTextEdit(DocumentPane*);
    void commitTextEdit(DocumentPane*, int index, std::function<void()> done = {});
};
