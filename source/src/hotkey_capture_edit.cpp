#include "hotkey_capture_edit.hpp"
#include "wardogs/hotkeys.hpp"

#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>

namespace {
UINT modifiers(Qt::KeyboardModifiers value) {
    UINT result{};
    if (value & Qt::ControlModifier) result |= MOD_CONTROL;
    if (value & Qt::AltModifier) result |= MOD_ALT;
    if (value & Qt::ShiftModifier) result |= MOD_SHIFT;
    if (value & Qt::MetaModifier) result |= MOD_WIN;
    return result;
}

UINT virtual_key(const QKeyEvent& event) {
    if (event.nativeVirtualKey() > 0 && event.nativeVirtualKey() <= 255)
        return static_cast<UINT>(event.nativeVirtualKey());
    const int key = event.key();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return static_cast<UINT>(key);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return event.modifiers() & Qt::KeypadModifier ? VK_NUMPAD0 + key - Qt::Key_0 : static_cast<UINT>(key);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) return VK_F1 + key - Qt::Key_F1;
    switch (key) {
    case Qt::Key_Escape: return VK_ESCAPE;
    case Qt::Key_Tab: case Qt::Key_Backtab: return VK_TAB;
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Return: case Qt::Key_Enter: return VK_RETURN;
    case Qt::Key_Insert: return VK_INSERT;
    case Qt::Key_Delete: return VK_DELETE;
    case Qt::Key_Pause: return VK_PAUSE;
    case Qt::Key_Print: return VK_SNAPSHOT;
    case Qt::Key_Clear: return VK_CLEAR;
    case Qt::Key_Home: return VK_HOME;
    case Qt::Key_End: return VK_END;
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_PageUp: return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    case Qt::Key_CapsLock: return VK_CAPITAL;
    case Qt::Key_NumLock: return VK_NUMLOCK;
    case Qt::Key_ScrollLock: return VK_SCROLL;
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Plus: return event.modifiers() & Qt::KeypadModifier ? VK_ADD : VK_OEM_PLUS;
    case Qt::Key_Equal: return VK_OEM_PLUS;
    case Qt::Key_Minus: return event.modifiers() & Qt::KeypadModifier ? VK_SUBTRACT : VK_OEM_MINUS;
    case Qt::Key_Asterisk: if (event.modifiers() & Qt::KeypadModifier) return VK_MULTIPLY; break;
    case Qt::Key_Slash: return event.modifiers() & Qt::KeypadModifier ? VK_DIVIDE : VK_OEM_2;
    case Qt::Key_Period: return event.modifiers() & Qt::KeypadModifier ? VK_DECIMAL : VK_OEM_PERIOD;
    case Qt::Key_Comma: return event.modifiers() & Qt::KeypadModifier ? VK_DECIMAL : VK_OEM_COMMA;
    case Qt::Key_Semicolon: return VK_OEM_1;
    case Qt::Key_QuoteLeft: return VK_OEM_3;
    case Qt::Key_BracketLeft: return VK_OEM_4;
    case Qt::Key_Backslash: return VK_OEM_5;
    case Qt::Key_BracketRight: return VK_OEM_6;
    case Qt::Key_Apostrophe: return VK_OEM_7;
    case Qt::Key_Menu: return VK_APPS;
    case Qt::Key_VolumeMute: return VK_VOLUME_MUTE;
    case Qt::Key_VolumeDown: return VK_VOLUME_DOWN;
    case Qt::Key_VolumeUp: return VK_VOLUME_UP;
    case Qt::Key_MediaNext: return VK_MEDIA_NEXT_TRACK;
    case Qt::Key_MediaPrevious: return VK_MEDIA_PREV_TRACK;
    case Qt::Key_MediaStop: return VK_MEDIA_STOP;
    case Qt::Key_MediaPlay: case Qt::Key_MediaPause: case Qt::Key_MediaTogglePlayPause: return VK_MEDIA_PLAY_PAUSE;
    default: break;
    }
    return 0;
}

bool modifier_key(int key) {
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt ||
        key == Qt::Key_Meta || key == Qt::Key_AltGr || key == Qt::Key_Super_L || key == Qt::Key_Super_R;
}
}  // namespace

HotkeyCaptureEdit::HotkeyCaptureEdit(const std::wstring& value, QWidget* parent)
    : QKeySequenceEdit(parent) {
    setMaximumSequenceLength(1);
    setFinishingKeyCombinations({});
    connect(this, &QKeySequenceEdit::keySequenceChanged, this, [this] {
        if (!updating_) recorded_.clear();
    });
    if (auto* edit = findChild<QLineEdit*>()) edit->installEventFilter(this);
    const auto hotkey = wardogs::parse_hotkey(value);
    // Mouse buttons have no QKeySequence representation. A valid Qt-only F35
    // sentinel keeps the base clear()/setKeySequence() API observable, while
    // binding() and the embedded field retain the actual native assignment.
    auto sequence = wardogs::is_mouse_hotkey(hotkey)
        ? QKeySequence(Qt::Key_F35)
        : QKeySequence(QString::fromStdWString(value));
    if (sequence.isEmpty())
        sequence = QKeySequence(Qt::Key_F35);
    set_recorded(hotkey.display, sequence);
}

std::wstring HotkeyCaptureEdit::binding() const {
    if (!recorded_.empty()) return recorded_;
    const auto text = keySequence().toString(QKeySequence::PortableText).toStdWString();
    return text.empty() ? text : wardogs::parse_hotkey(text).display;
}

void HotkeyCaptureEdit::set_recorded(const std::wstring& value, const QKeySequence& sequence) {
    updating_ = true;
    setKeySequence(sequence);
    recorded_ = value;
    updating_ = false;
    restore_display();
    emit editingFinished();
}

void HotkeyCaptureEdit::restore_display() {
    if (!recorded_.empty())
        if (auto* edit = findChild<QLineEdit*>()) edit->setText(QString::fromStdWString(recorded_));
}

void HotkeyCaptureEdit::record_key(QKeyEvent* event) {
    event->accept();
    if (event->isAutoRepeat() || modifier_key(event->key())) return;
    const UINT key = virtual_key(*event);
    if (!key) return;
    auto native_modifiers = modifiers(event->modifiers());
    if (event->key() == Qt::Key_Backtab) native_modifiers |= MOD_SHIFT;
    const auto value = wardogs::hotkey_from_native(key, native_modifiers);
    set_recorded(value.display, QKeySequence(QKeyCombination(event->modifiers(),
        static_cast<Qt::Key>(event->key()))));
}

bool HotkeyCaptureEdit::record_mouse(QMouseEvent* event) {
    if (event->button() != Qt::XButton1 && event->button() != Qt::XButton2) return false;
    const auto key = event->button() == Qt::XButton1 ? VK_XBUTTON1 : VK_XBUTTON2;
    const auto value = wardogs::hotkey_from_native(key, modifiers(event->modifiers()));
    setFocus(Qt::MouseFocusReason);
    set_recorded(value.display, QKeySequence(Qt::Key_F35));
    event->accept();
    return true;
}

void HotkeyCaptureEdit::keyPressEvent(QKeyEvent* event) { record_key(event); }
void HotkeyCaptureEdit::keyReleaseEvent(QKeyEvent* event) {
    // Recording is committed on DOWN. Letting Qt start its multi-chord finish
    // timer on UP would later replace a native mouse binding with its sentinel.
    event->accept();
}
void HotkeyCaptureEdit::mousePressEvent(QMouseEvent* event) {
    if (!record_mouse(event)) QKeySequenceEdit::mousePressEvent(event);
}

void HotkeyCaptureEdit::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    restore_display();
    emit editingFinished();
}

bool HotkeyCaptureEdit::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        record_key(static_cast<QKeyEvent*>(event));
        return true;
    }
    if (event->type() == QEvent::KeyRelease) { event->accept(); return true; }
    if (event->type() == QEvent::MouseButtonRelease) {
        const auto button = static_cast<QMouseEvent*>(event)->button();
        if (button == Qt::XButton1 || button == Qt::XButton2) { event->accept(); return true; }
    }
    if ((event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) &&
        record_mouse(static_cast<QMouseEvent*>(event))) return true;
    return QKeySequenceEdit::eventFilter(watched, event);
}

bool HotkeyCaptureEdit::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
    const auto result = QKeySequenceEdit::event(event);
    if (event->type() == QEvent::Show || event->type() == QEvent::FocusOut) restore_display();
    return result;
}

bool HotkeyCaptureEdit::focusNextPrevChild(bool) { return false; }
