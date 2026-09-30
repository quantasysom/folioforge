#pragma once
#include "pdfengine/document.h"
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <functional>

class InlineEditor : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    std::function<void()> cancel, commit;
    bool finished{};
protected:
    // Clicking elsewhere applies the edit, like leaving a text field in an editor.
    void focusOutEvent(QFocusEvent* event) override {
        QLineEdit::focusOutEvent(event);
        if (event->reason() == Qt::ActiveWindowFocusReason || event->reason() == Qt::PopupFocusReason) return;
        QTimer::singleShot(0, this, [this] { if (!finished && commit && isVisible()) commit(); });
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) { if (cancel) cancel(); event->accept(); }
        else QLineEdit::keyPressEvent(event);
    }
};
class TextCanvas : public QLabel {
public:
    explicit TextCanvas(const QString& label = {}) : QLabel(label) { setFocusPolicy(Qt::StrongFocus); setMouseTracking(true); }
    std::vector<pdfengine::TextRun> runs;
    double scale{1}, pageHeight{};
    bool editMode{};
    int selected{}, hovered{-1};
    QPointF pressPoint{-1, -1};
    InlineEditor* editor{};
    std::function<void(int)> editRequested;
    bool editing() const { return editor && editor->isVisible(); }
    QRectF box(int index) const {
        const auto& run = runs.at(index);
        return QRectF(run.x * scale, (pageHeight - run.baseline - run.fontSize) * scale,
                      run.width * scale, run.fontSize * 1.3 * scale).adjusted(-2, -2, 2, 2);
    }
    int runAt(const QPointF& point) const {
        int match = -1; double area = 0;
        for (int i = 0; i < static_cast<int>(runs.size()); ++i) {
            auto rect = box(i);
            if (rect.contains(point) && (match < 0 || rect.width() * rect.height() < area)) { match = i; area = rect.width() * rect.height(); }
        }
        return match;
    }
protected:
    void paintEvent(QPaintEvent* event) override {
        QLabel::paintEvent(event);
        if (!editMode || editing() || hovered < 0 || hovered >= static_cast<int>(runs.size())) return;
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#1769E8"), 1)); painter.setBrush(QColor(23, 105, 232, 28)); painter.drawRect(box(hovered));
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (editMode && !editing()) {
            int match = runAt(event->position());
            if (match != hovered) { hovered = match; update(); }
            setCursor(match >= 0 ? Qt::IBeamCursor : Qt::ArrowCursor);
        }
        QLabel::mouseMoveEvent(event);
    }
    void leaveEvent(QEvent* event) override { hovered = -1; unsetCursor(); update(); QLabel::leaveEvent(event); }
    void mousePressEvent(QMouseEvent* event) override {
        if (editMode && !editing()) {
            int match = runAt(event->position());
            if (match >= 0) { selected = match; pressPoint = event->position(); update(); if (editRequested) editRequested(match); return; }
        }
        QLabel::mousePressEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (editMode && !editing() && !runs.empty()) {
            if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
                selected = (selected + (event->key() == Qt::Key_Left ? -1 : 1) + static_cast<int>(runs.size())) % static_cast<int>(runs.size());
                hovered = selected; update(); event->accept(); return;
            }
            if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_F2) {
                pressPoint = {-1, -1}; if (editRequested) editRequested(selected); event->accept(); return;
            }
        }
        QLabel::keyPressEvent(event);
    }
};
