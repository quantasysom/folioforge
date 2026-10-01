#pragma once
#include <cmath>
#include "pdfengine/document.h"
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QPolygonF>
#include <algorithm>
#include <vector>
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
    double scale{1}, pageHeight{}, pageWidth{};
    int rotation{};
    // Annotation tool: None does nothing, Area drags a rectangle, Pen draws freehand, Point places at a click, Select picks an annotation.
    enum class Tool { None, Area, Pen, Point, Select, Form } tool{Tool::None};
    std::vector<pdfengine::FormField> fields;
    bool formPrompted{};
    std::function<void(int)> fieldClicked;
    std::vector<pdfengine::Annotation> annotations;
    int selectedAnnotation{-1};
    bool movingAnnotation{false};
    std::function<void(QPointF, QPointF)> areaDrawn;
    std::function<void(std::vector<std::vector<QPointF>>)> strokeDrawn;
    std::function<void(QPointF)> pointPicked;
    std::function<void(int)> annotationSelected;
    std::function<void(int, QPointF)> annotationMoved;   // index, PDF-point offset
    std::function<void(int)> annotationActivated;        // double-click
    // Page space (points, y up, unrotated page) <-> canvas pixels.
    QPointF toPdf(const QPointF& view) const {
        const double u = view.x() / scale, v = view.y() / scale;
        switch (rotation) {
        case 90: return {v, u};
        case 180: return {pageWidth - u, v};
        case 270: return {pageWidth - v, pageHeight - u};
        default: return {u, pageHeight - v};
        }
    }
    QPointF toView(const QPointF& pdf) const {
        switch (rotation) {
        case 90: return {pdf.y() * scale, pdf.x() * scale};
        case 180: return {(pageWidth - pdf.x()) * scale, pdf.y() * scale};
        case 270: return {(pageHeight - pdf.y()) * scale, (pageWidth - pdf.x()) * scale};
        default: return {pdf.x() * scale, (pageHeight - pdf.y()) * scale};
        }
    }
    QRectF annotationBox(const pdfengine::Annotation& a) const {
        return QRectF(toView({a.x0, a.y0}), toView({a.x1, a.y1})).normalized().adjusted(-2, -2, 2, 2);
    }
    QRectF fieldBox(const pdfengine::FormField& f) const {
        return QRectF(toView({f.x0, f.y0}), toView({f.x1, f.y1})).normalized();
    }
    int fieldAt(const QPointF& point) const {
        for (int i = 0; i < static_cast<int>(fields.size()); ++i) if (fieldBox(fields[i]).contains(point)) return i;
        return -1;
    }
    bool editMode{};
    int selected{}, hovered{-1};
    QPointF pressPoint{-1, -1};
    InlineEditor* editor{};
    bool dragging{};
    QPointF dragStart, dragEnd;
    QList<QPointF> currentStroke;
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
        if (tool == Tool::Form) {
            QPainter overlay(this);
            for (const auto& f : fields) {
                const bool locked = f.readOnly || f.kind == pdfengine::FormFieldKind::Signature || f.kind == pdfengine::FormFieldKind::Button;
                overlay.setPen(QPen(locked ? QColor(130, 130, 130, 160) : QColor(23, 105, 232, 200), 1));
                overlay.setBrush(locked ? QColor(130, 130, 130, 25) : QColor(23, 105, 232, 30));
                overlay.drawRect(fieldBox(f));
            }
        }
        if (tool != Tool::None) {
            QPainter overlay(this); overlay.setRenderHint(QPainter::Antialiasing);
            if (selectedAnnotation >= 0 && selectedAnnotation < static_cast<int>(annotations.size())) {
                overlay.setPen(QPen(QColor("#1769E8"), 1.5, Qt::DashLine)); overlay.setBrush(Qt::NoBrush);
                auto box = annotationBox(annotations[selectedAnnotation]);
                if (movingAnnotation) box.translate(dragEnd - dragStart);
                overlay.drawRect(box);
            }
            if (dragging && tool == Tool::Area) {
                overlay.setPen(QPen(QColor("#1769E8"), 1, Qt::DashLine)); overlay.setBrush(QColor(23, 105, 232, 30));
                overlay.drawRect(QRectF(dragStart, dragEnd).normalized());
            } else if (dragging && tool == Tool::Pen) {
                overlay.setPen(QPen(QColor("#1769E8"), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); overlay.drawPolyline(QPolygonF(currentStroke));
            }
        }
        if (!editMode || editing() || hovered < 0 || hovered >= static_cast<int>(runs.size())) return;
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#1769E8"), 1)); painter.setBrush(QColor(23, 105, 232, 28)); painter.drawRect(box(hovered));
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (movingAnnotation) { dragEnd = event->position(); update(); return; }
        if (tool != Tool::None && dragging) {
            dragEnd = event->position();
            if (tool == Tool::Pen) currentStroke.append(event->position());
            update(); return;
        }
        if (editMode && !editing()) {
            int match = runAt(event->position());
            if (match != hovered) { hovered = match; update(); }
            setCursor(match >= 0 ? Qt::IBeamCursor : Qt::ArrowCursor);
        }
        QLabel::mouseMoveEvent(event);
    }
    void leaveEvent(QEvent* event) override { hovered = -1; unsetCursor(); update(); QLabel::leaveEvent(event); }
    void mousePressEvent(QMouseEvent* event) override {
        if (tool != Tool::None && !editMode && event->button() == Qt::LeftButton) {
            if (tool == Tool::Form) { int hit = fieldAt(event->position()); if (hit >= 0 && fieldClicked) fieldClicked(hit); return; }
            if (tool == Tool::Point) { if (pointPicked) pointPicked(toPdf(event->position())); return; }
            if (tool == Tool::Select) {
                int match = -1; double area = 0;
                for (int i = 0; i < static_cast<int>(annotations.size()); ++i) {
                    auto rect = annotationBox(annotations[i]);
                    if (rect.contains(event->position()) && (match < 0 || rect.width() * rect.height() < area)) { match = i; area = rect.width() * rect.height(); }
                }
                selectedAnnotation = match; update(); if (annotationSelected) annotationSelected(match);
                if (match >= 0 && annotations[match].removable) { movingAnnotation = true; dragStart = dragEnd = event->position(); }
                return;
            }
            dragging = true; dragStart = dragEnd = event->position(); currentStroke = {event->position()}; update(); return;
        }
        if (editMode && !editing()) {
            int match = runAt(event->position());
            if (match >= 0) { selected = match; pressPoint = event->position(); update(); if (editRequested) editRequested(match); return; }
        }
        QLabel::mousePressEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (tool == Tool::Select && !editMode && selectedAnnotation >= 0 && annotationActivated) { movingAnnotation = false; annotationActivated(selectedAnnotation); return; }
        QLabel::mouseDoubleClickEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (movingAnnotation && event->button() == Qt::LeftButton) {
            movingAnnotation = false; dragEnd = event->position();
            const auto delta = dragEnd - dragStart; update();
            if ((std::abs(delta.x()) > 2 || std::abs(delta.y()) > 2) && annotationMoved) {
                auto a = toPdf(dragStart), b = toPdf(dragEnd); annotationMoved(selectedAnnotation, b - a);
            }
            return;
        }
        if (dragging && event->button() == Qt::LeftButton) {
            dragging = false; dragEnd = event->position(); update();
            if (tool == Tool::Area && areaDrawn) areaDrawn(toPdf(dragStart), toPdf(dragEnd));
            else if (tool == Tool::Pen && strokeDrawn) {
                std::vector<QPointF> stroke; for (const auto& point : currentStroke) stroke.push_back(toPdf(point));
                strokeDrawn({std::move(stroke)});
            }
            currentStroke.clear(); return;
        }
        QLabel::mouseReleaseEvent(event);
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
