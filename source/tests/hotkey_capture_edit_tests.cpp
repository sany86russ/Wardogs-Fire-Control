#include "hotkey_capture_edit.hpp"
#include "wardogs/hotkeys.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QVBoxLayout>

#include <iostream>
#include <chrono>
#include <thread>

namespace {
int checks{}, failures{};
void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

void key(HotkeyCaptureEdit& edit, int value, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
         unsigned native = 0) {
    QKeyEvent event(QEvent::KeyPress, value, modifiers, 0, native, 0);
    QApplication::sendEvent(&edit, &event);
}

void mouse(QObject* target, Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
    QMouseEvent event(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5),
                      button, button, modifiers);
    QApplication::sendEvent(target, &event);
}
}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    HotkeyCaptureEdit edit(L"F8");
    check(edit.binding() == L"F8", "existing keyboard assignments are retained");
    key(edit, Qt::Key_F4, Qt::ShiftModifier | Qt::AltModifier, VK_F4);
    check(edit.binding() == L"Alt+Shift+F4", "a three-key chord records its native key and both modifiers");
    key(edit, Qt::Key_Escape, Qt::NoModifier, VK_ESCAPE);
    check(edit.binding() == L"Esc", "Escape can be assigned rather than closing the editor");
    key(edit, Qt::Key_Tab, Qt::NoModifier, VK_TAB);
    check(edit.binding() == L"Tab", "Tab records a binding rather than moving focus");
    key(edit, Qt::Key_Backspace, Qt::NoModifier, VK_BACK);
    check(edit.binding() == L"Backspace", "Backspace records its key instead of clearing the assignment");
    key(edit, Qt::Key_F12, Qt::ControlModifier, VK_F12);
    check(edit.binding() == L"Ctrl+F12", "F12 can be recorded for the hook-backed listener");
    key(edit, Qt::Key_1, Qt::KeypadModifier, VK_NUMPAD1);
    check(edit.binding() == L"Numpad1", "keypad digits remain distinct from the top number row");
    key(edit, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier, VK_OEM_PLUS);
    const auto plus = wardogs::parse_hotkey(edit.binding());
    check(plus.virtual_key == VK_OEM_PLUS &&
          plus.modifiers == (MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT),
          "OEM plus has an unambiguous portable binding with its modifiers");
    mouse(&edit, Qt::XButton1, Qt::ControlModifier | Qt::AltModifier);
    check(edit.binding() == L"Ctrl+Alt+Mouse4", "the first side button records a modified assignment");
    auto* line = edit.findChild<QLineEdit*>();
    check(line && line->text() == QStringLiteral("Ctrl+Alt+Mouse4"),
          "side-button assignments are visibly named in the shortcut field");
    if (line) mouse(line, Qt::XButton2, Qt::ShiftModifier);
    check(edit.binding() == L"Shift+Mouse5", "side-button clicks on the embedded field are captured too");
    QKeySequenceEdit* base = &edit;
    base->clear();
    check(edit.binding().empty(), "clearing through the existing Qt base API clears a mouse assignment");
    base->setKeySequence(QKeySequence(QStringLiteral("Ctrl+F9")));
    check(edit.binding() == L"Ctrl+F9", "existing setKeySequence callers can replace a native mouse binding");
    base->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Num+2")));
    check(edit.binding() == L"Ctrl+Numpad2", "Qt portable keypad assignments retain their native meaning");
    HotkeyCaptureEdit restored(L"Alt+Mouse5");
    check(restored.binding() == L"Alt+Mouse5", "saved side-button bindings are restored without Qt key-code loss");
    HotkeyCaptureEdit keypad(L"Numpad1");
    check(keypad.binding() == L"Numpad1", "canonical saved keypad names restore without Qt serialization support");
    static_cast<QKeySequenceEdit*>(&keypad)->clear();
    check(keypad.binding().empty(), "restored native keypad assignments remain clearable through the Qt API");
    key(restored, Qt::Key_K, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier, L'K');
    check(restored.binding() == L"Ctrl+Alt+Shift+K", "three modifiers and a keyboard key can be recorded together");
    {
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_K,
            Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier, 0, L'K', 0);
        QApplication::sendEvent(&restored, &release);
        QElapsedTimer finishing;
        finishing.start();
        while (finishing.elapsed() < 1100) {
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        QFocusEvent focus_out(QEvent::FocusOut);
        QApplication::sendEvent(&restored, &focus_out);
        check(restored.binding() == L"Ctrl+Alt+Shift+K" &&
              restored.findChild<QLineEdit*>()->text() == QStringLiteral("Ctrl+Alt+Shift+K"),
              "key release, Qt's finishing timer and focus loss preserve the recorded keyboard chord");
    }
    {
        mouse(&restored, Qt::XButton2, Qt::AltModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(5, 5), QPointF(5, 5),
                            Qt::XButton2, Qt::NoButton, Qt::AltModifier);
        QApplication::sendEvent(restored.findChild<QLineEdit*>(), &release);
        QFocusEvent focus_out(QEvent::FocusOut);
        QApplication::sendEvent(&restored, &focus_out);
        QApplication::processEvents();
        check(restored.binding() == L"Alt+Mouse5" &&
              restored.findChild<QLineEdit*>()->text() == QStringLiteral("Alt+Mouse5"),
              "mouse release and focus loss preserve the visible side-button assignment");
    }
    {
        QWidget surface;
        auto* layout = new QVBoxLayout(&surface);
        auto* visible = new HotkeyCaptureEdit(L"F8", &surface);
        auto* next = new QLineEdit(&surface);
        layout->addWidget(visible);
        layout->addWidget(next);
        surface.show();
        surface.activateWindow();
        visible->setFocus();
        QApplication::processEvents();
        check(visible->hasFocus(), "the shown shortcut editor receives real Qt focus");
        mouse(visible->findChild<QLineEdit*>(), Qt::XButton1, Qt::ControlModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(5, 5), QPointF(5, 5),
                            Qt::XButton1, Qt::NoButton, Qt::ControlModifier);
        QApplication::sendEvent(visible->findChild<QLineEdit*>(), &release);
        next->setFocus();
        QApplication::processEvents();
        check(next->hasFocus() && visible->binding() == L"Ctrl+Mouse4" &&
              visible->findChild<QLineEdit*>()->text() == QStringLiteral("Ctrl+Mouse4"),
              "a normal shown-widget focus transition preserves the recorded mouse shortcut");
    }
    std::cout << checks << " shortcut editor checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
