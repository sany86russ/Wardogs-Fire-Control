#pragma once

#include <QKeySequenceEdit>
#include <string>

class QKeyEvent;
class QMouseEvent;
class QFocusEvent;

// Keeps the standard Qt shortcut-editor API for existing callers, and also
// records the two side mouse buttons and native keys Qt serializes ambiguously.
class HotkeyCaptureEdit final : public QKeySequenceEdit {
public:
    explicit HotkeyCaptureEdit(const std::wstring& value, QWidget* parent = nullptr);
    [[nodiscard]] std::wstring binding() const;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool event(QEvent* event) override;
    bool focusNextPrevChild(bool next) override;

private:
    void record_key(QKeyEvent* event);
    bool record_mouse(QMouseEvent* event);
    void set_recorded(const std::wstring& value, const QKeySequence& sequence);
    void restore_display();
    std::wstring recorded_;
    bool updating_{};
};
