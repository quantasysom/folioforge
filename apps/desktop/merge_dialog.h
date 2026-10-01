#pragma once
#include <QtWidgets>
#include <vector>

struct MergeItem { QString path, ranges; };

// Pick several PDFs, images and Office documents, order them, and choose page ranges for the PDFs.
class MergeDialog : public QDialog {
public:
    explicit MergeDialog(QWidget* parent, const QStringList& initial = {}) : QDialog(parent) {
        setWindowTitle("Merge files into one PDF"); resize(640, 400);
        auto layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel("Files are merged top to bottom into a new PDF. For PDFs, Pages accepts ranges such as 1-3,5,8- (blank = all pages)."));
        table_ = new QTableWidget(0, 2); table_->setHorizontalHeaderLabels({"File", "Pages"});
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch); table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection); table_->setAccessibleName("Files to merge");
        layout->addWidget(table_);
        auto row = new QHBoxLayout; layout->addLayout(row);
        auto add = new QPushButton("Add files…"), remove = new QPushButton("Remove"), up = new QPushButton("Move up"), down = new QPushButton("Move down");
        for (auto b : {add, remove, up, down}) row->addWidget(b);
        row->addStretch();
        connect(add, &QPushButton::clicked, this, [this] {
            addFiles(QFileDialog::getOpenFileNames(this, "Add files to merge", {},
                "Supported files (*.pdf *.png *.jpg *.jpeg *.docx *.doc *.odt *.rtf *.txt *.xlsx *.xls *.ods *.pptx *.ppt *.odp);;PDF files (*.pdf);;Images (*.png *.jpg *.jpeg);;Office documents (*.docx *.doc *.odt *.rtf *.txt *.xlsx *.xls *.ods *.pptx *.ppt *.odp)"));
        });
        connect(remove, &QPushButton::clicked, this, [this] { if (table_->currentRow() >= 0) table_->removeRow(table_->currentRow()); });
        connect(up, &QPushButton::clicked, this, [this] { move(-1); });
        connect(down, &QPushButton::clicked, this, [this] { move(1); });
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("Merge");
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (table_->rowCount() == 0) { QMessageBox::information(this, "Merge", "Add at least one file."); return; }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        addFiles(initial);
    }
    void addFiles(const QStringList& paths) {
        for (const auto& path : paths) {
            const int r = table_->rowCount(); table_->insertRow(r);
            auto name = new QTableWidgetItem(QFileInfo(path).fileName()); name->setData(Qt::UserRole, path); name->setToolTip(path);
            name->setFlags(name->flags() & ~Qt::ItemIsEditable); table_->setItem(r, 0, name);
            auto ranges = new QTableWidgetItem; table_->setItem(r, 1, ranges);
            if (!path.endsWith(".pdf", Qt::CaseInsensitive)) { ranges->setText("—"); ranges->setFlags(ranges->flags() & ~Qt::ItemIsEditable); }
        }
    }
    std::vector<MergeItem> items() const {
        std::vector<MergeItem> out;
        for (int r = 0; r < table_->rowCount(); ++r) {
            auto path = table_->item(r, 0)->data(Qt::UserRole).toString();
            out.push_back({path, path.endsWith(".pdf", Qt::CaseInsensitive) ? table_->item(r, 1)->text().trimmed() : QString()});
        }
        return out;
    }
private:
    QTableWidget* table_{};
    void move(int delta) {
        const int r = table_->currentRow(), t = r + delta;
        if (r < 0 || t < 0 || t >= table_->rowCount()) return;
        for (int c = 0; c < 2; ++c) { auto a = table_->takeItem(r, c), b = table_->takeItem(t, c); table_->setItem(r, c, b); table_->setItem(t, c, a); }
        table_->setCurrentCell(t, 0);
    }
};
