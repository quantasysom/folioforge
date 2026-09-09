#pragma once
#include "pdfengine/document.h"
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <functional>

class InlineEditor : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    std::function<void()> cancel;
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) { if (cancel) cancel(); event->accept(); }
        else QLineEdit::keyPressEvent(event);
    }
};
class TextCanvas : public QLabel {
public:
    explicit TextCanvas(const QString& label = {}) : QLabel(label) { setFocusPolicy(Qt::StrongFocus); }
    std::vector<pdfengine::TextRun> runs;
    double scale{1}, pageHeight{};
    bool editMode{};
    int selected{};
    InlineEditor* editor{};
    std::function<void(int)> editRequested;
    bool editing() const { return editor && editor->isVisible(); }
    QRectF box(int index) const {
        const auto& run = runs.at(index);
        return QRectF(run.x * scale, (pageHeight - run.baseline - run.fontSize) * scale,
                      run.width * scale, run.fontSize * 1.3 * scale).adjusted(-2, -2, 2, 2);
    }
protected:
    void paintEvent(QPaintEvent* event) override {
        QLabel::paintEvent(event);
        if (!editMode || editing()) return;
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < static_cast<int>(runs.size()); ++i) {
            painter.setPen(QPen(i == selected ? QColor("#1769E8") : QColor("#779AC8"), i == selected ? 2 : 1, Qt::DashLine));
            painter.setBrush(Qt::NoBrush); painter.drawRect(box(i));
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (editMode && !editing()) {
            int match = -1;
            for (int i = 0; i < static_cast<int>(runs.size()); ++i) if (box(i).contains(event->position())) {
                if (match != -1) { setToolTip("Overlapping text is ambiguous. Use arrow keys and Enter to choose a supported run."); return; }
                match = i;
            }
            if (match >= 0) { selected = match; update(); if (editRequested) editRequested(match); return; }
        }
        QLabel::mousePressEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (editMode && !editing() && !runs.empty()) {
            if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
                selected = (selected + (event->key() == Qt::Key_Left ? -1 : 1) + static_cast<int>(runs.size())) % static_cast<int>(runs.size());
                update(); event->accept(); return;
            }
            if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_F2) {
                if (editRequested) editRequested(selected); event->accept(); return;
            }
        }
        QLabel::keyPressEvent(event);
    }
};
