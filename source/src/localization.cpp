#include "localization.hpp"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMetaProperty>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTranslator>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <stdexcept>
#include <vector>

static void initialize_translation_resources() { Q_INIT_RESOURCE(localization); }

namespace wardogs::i18n {
namespace {

struct Message {
    QString ru, en;
    QRegularExpression ru_pattern, en_pattern;
    std::vector<QString> placeholders;
    bool ru_fragment{}, en_fragment{};
};

QRegularExpression pattern_for(const QString& value) {
    const QRegularExpression placeholders(QStringLiteral("%[1-9][0-9]*"));
    QString expression(QStringLiteral("\\A"));
    qsizetype offset = 0;
    auto matches = placeholders.globalMatch(value);
    while (matches.hasNext()) {
        const auto match = matches.next();
        expression += QRegularExpression::escape(value.mid(offset, match.capturedStart() - offset));
        expression += QStringLiteral("(.*?)");
        offset = match.capturedEnd();
    }
    expression += QRegularExpression::escape(value.mid(offset)) + QStringLiteral("\\z");
    return QRegularExpression(expression, QRegularExpression::DotMatchesEverythingOption);
}

// Paths, links, and HTML attributes are data, even when their names coincide
// with translated interface words such as "target", "Save", or "Close".
std::vector<std::pair<qsizetype, qsizetype>> protected_spans(const QString& value) {
    static const QRegularExpression opaque(QStringLiteral(
        R"(<[^>]*>|(?:https?|file)://[^\s<>"']+|[A-Za-z]:[\\/][^\r\n<>"']+|\\\\[^\r\n<>"']+|[^\s<>"']+[\\/][^\s<>"']+|[A-Za-z0-9_.-]+\.(?:onnx|wdt|ini|log|json|png|jpg|jpeg|bmp|txt|html|exe|dll)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    std::vector<std::pair<qsizetype, qsizetype>> result;
    auto matches = opaque.globalMatch(value);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result.emplace_back(match.capturedStart(), match.capturedEnd());
    }
    return result;
}

// Qt's built-in menus and dialog buttons also use the chosen app language.
// These strings are deliberately separate from application source messages.
class StockTranslator final : public QTranslator {
public:
    using QTranslator::QTranslator;
    bool isEmpty() const override { return false; }
    QString translate(const char* context, const char* source, const char*, int n) const override {
        // Context-qualified Qt 6.8.3 translations also cover file dialogs,
        // editing menus, plural messages and accessibility actions.
        static const std::map<QString, QJsonValue> qt_messages = [] {
            QFile file(QStringLiteral(":/i18n/qtbase_ru.json"));
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("Embedded Qt translations unavailable");
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(file.readAll(), &error);
            if (error.error != QJsonParseError::NoError || !document.isArray())
                throw std::runtime_error("Invalid Qt translation catalog");
            std::map<QString, QJsonValue> result;
            for (const auto& value : document.array()) {
                const auto entry = value.toObject();
                result.emplace(entry.value(QStringLiteral("context")).toString() + QChar(0x1f) +
                    entry.value(QStringLiteral("source")).toString(), entry.value(QStringLiteral("translation")));
            }
            return result;
        }();
        const auto key = QString::fromUtf8(context ? context : "") + QChar(0x1f) + QString::fromUtf8(source);
        if (const auto found = qt_messages.find(key); found != qt_messages.end()) {
            if (found->second.isString()) return found->second.toString();
            const auto forms = found->second.toArray();
            if (forms.isEmpty()) return {};
            const int form = n >= 0 && n % 10 == 1 && n % 100 != 11 ? 0
                : n >= 0 && n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 10 || n % 100 >= 20) ? 1 : 2;
            return forms.at(std::min(form, static_cast<int>(forms.size()) - 1)).toString();
        }
        static const std::map<QString, QString> labels{
            {QStringLiteral("OK"), QStringLiteral("ОК")},
            {QStringLiteral("&OK"), QStringLiteral("ОК")},
            {QStringLiteral("Cancel"), QStringLiteral("Отмена")},
            {QStringLiteral("&Cancel"), QStringLiteral("Отмена")},
            {QStringLiteral("Close"), QStringLiteral("Закрыть")},
            {QStringLiteral("&Close"), QStringLiteral("Закрыть")},
            {QStringLiteral("Save"), QStringLiteral("Сохранить")},
            {QStringLiteral("&Save"), QStringLiteral("Сохранить")},
            {QStringLiteral("Open"), QStringLiteral("Открыть")},
            {QStringLiteral("&Open"), QStringLiteral("Открыть")},
            {QStringLiteral("Apply"), QStringLiteral("Применить")},
            {QStringLiteral("&Apply"), QStringLiteral("Применить")},
            {QStringLiteral("Yes"), QStringLiteral("Да")},
            {QStringLiteral("&Yes"), QStringLiteral("Да")},
            {QStringLiteral("No"), QStringLiteral("Нет")},
            {QStringLiteral("&No"), QStringLiteral("Нет")},
            {QStringLiteral("&Undo"), QStringLiteral("Отменить")},
            {QStringLiteral("&Redo"), QStringLiteral("Повторить")},
            {QStringLiteral("Cu&t"), QStringLiteral("Вырезать")},
            {QStringLiteral("&Copy"), QStringLiteral("Копировать")},
            {QStringLiteral("&Paste"), QStringLiteral("Вставить")},
            {QStringLiteral("Delete"), QStringLiteral("Удалить")},
            {QStringLiteral("Select All"), QStringLiteral("Выделить всё")},
            {QStringLiteral("Select &All"), QStringLiteral("Выделить всё")},
            {QStringLiteral("Clear"), QStringLiteral("Очистить")},
            {QStringLiteral("Step up"), QStringLiteral("Увеличить")},
            {QStringLiteral("Step down"), QStringLiteral("Уменьшить")},
            {QStringLiteral("Show Details..."), QStringLiteral("Подробнее…")},
            {QStringLiteral("Hide Details..."), QStringLiteral("Скрыть подробности")},
            {QStringLiteral("Look in:"), QStringLiteral("Папка:")},
            {QStringLiteral("File name:"), QStringLiteral("Имя файла:")},
            {QStringLiteral("Files of type:"), QStringLiteral("Тип файлов:")},
            {QStringLiteral("Directory:"), QStringLiteral("Папка:")},
            {QStringLiteral("Choose"), QStringLiteral("Выбрать")},
            {QStringLiteral("&Choose"), QStringLiteral("Выбрать")},
            {QStringLiteral("Back"), QStringLiteral("Назад")},
            {QStringLiteral("Forward"), QStringLiteral("Вперёд")},
            {QStringLiteral("Parent Directory"), QStringLiteral("Родительская папка")},
            {QStringLiteral("Create New Folder"), QStringLiteral("Создать папку")},
            {QStringLiteral("List View"), QStringLiteral("Список")},
            {QStringLiteral("Detail View"), QStringLiteral("Таблица")},
            {QStringLiteral("Name"), QStringLiteral("Имя")},
            {QStringLiteral("Size"), QStringLiteral("Размер")},
            {QStringLiteral("Type"), QStringLiteral("Тип")},
            {QStringLiteral("Date Modified"), QStringLiteral("Дата изменения")},
            {QStringLiteral("All Files (*)"), QStringLiteral("Все файлы (*)")},
            {QStringLiteral("Find Directory"), QStringLiteral("Выбрать папку")},
            {QStringLiteral("&Rename"), QStringLiteral("Переименовать")},
            {QStringLiteral("&Delete"), QStringLiteral("Удалить")},
            {QStringLiteral("Show &hidden files"), QStringLiteral("Показать скрытые файлы")},
            {QStringLiteral("New Folder"), QStringLiteral("Новая папка")},
        };
        const auto found = labels.find(QString::fromUtf8(source));
        return found == labels.end() ? QString{} : found->second;
    }
};

enum class Display { property, combo, combo_tip, tab, tab_tip, html };
struct Binding {
    QPointer<QObject> object;
    Display kind;
    QByteArray property;
    int index{-1};
    QString source, rendered;

    QString read() const {
        if (!object) return {};
        if (kind == Display::combo || kind == Display::combo_tip) {
            auto* combo = qobject_cast<QComboBox*>(object.data());
            return kind == Display::combo ? combo->itemText(index)
                : combo->itemData(index, Qt::ToolTipRole).toString();
        }
        if (kind == Display::tab || kind == Display::tab_tip) {
            auto* tabs = qobject_cast<QTabWidget*>(object.data());
            return kind == Display::tab ? tabs->tabText(index) : tabs->tabToolTip(index);
        }
        if (kind == Display::html) return rendered; // Qt normalizes HTML.
        return object->property(property.constData()).toString();
    }

    void write(const QString& value) {
        if (!object) return;
        const QSignalBlocker blocker(object);
        if (kind == Display::combo || kind == Display::combo_tip) {
            auto* combo = qobject_cast<QComboBox*>(object.data());
            if (index >= combo->count()) return;
            if ((kind == Display::combo ? combo->itemText(index)
                    : combo->itemData(index, Qt::ToolTipRole).toString()) == value) {
                rendered = value;
                return;
            }
            auto* editor = combo->isEditable() ? combo->lineEdit() : nullptr;
            const QString entered = editor ? editor->text() : QString{};
            const int cursor = editor ? editor->cursorPosition() : 0;
            const int selection = editor ? editor->selectionStart() : -1;
            const int selection_length = editor ? static_cast<int>(editor->selectedText().size()) : 0;
            const bool modified = editor && editor->isModified();
            const QSignalBlocker edit_blocker(editor);
            if (kind == Display::combo) combo->setItemText(index, value);
            else combo->setItemData(index, value, Qt::ToolTipRole);
            // Qt refreshes the editor on model dataChanged for every item
            // role, including tooltips. Preserve entered OCR/coordinate text.
            if (editor) {
                editor->setText(entered);
                editor->setCursorPosition(cursor);
                if (selection >= 0) editor->setSelection(selection, selection_length);
                editor->setModified(modified);
            }
        } else if (kind == Display::tab || kind == Display::tab_tip) {
            auto* tabs = qobject_cast<QTabWidget*>(object.data());
            if (index >= tabs->count()) return;
            if (kind == Display::tab) tabs->setTabText(index, value);
            else tabs->setTabToolTip(index, value);
        } else if (kind == Display::html) {
            auto* browser = qobject_cast<QTextBrowser*>(object.data());
            const int vertical = browser->verticalScrollBar()->value();
            const int horizontal = browser->horizontalScrollBar()->value();
            browser->setHtml(value);
            browser->verticalScrollBar()->setValue(vertical);
            browser->horizontalScrollBar()->setValue(horizontal);
        } else object->setProperty(property.constData(), value);
        rendered = value;
    }
};

class Localizer {
public:
    std::atomic<UiLanguage> current{UiLanguage::russian};
    StockTranslator stock;
    std::vector<Message> messages;
    std::vector<Binding> bindings;
    std::vector<QPointer<QWidget>> roots;
    bool stock_installed{};

    Localizer() {
        initialize_translation_resources();
        std::map<QString, QString> unique;
        for (const auto* name : {"main.json", "dialogs.json", "errors.json", "common.json"}) {
            QFile file(QStringLiteral(":/i18n/") + QString::fromLatin1(name));
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("Embedded translation catalog unavailable");
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(file.readAll(), &error);
            if (error.error != QJsonParseError::NoError || !document.isArray())
                throw std::runtime_error("Invalid embedded translation catalog");
            for (const auto& item : document.array()) {
                const auto entry = item.toObject();
                const auto ru = entry.value(QStringLiteral("ru")).toString();
                const auto en = entry.value(QStringLiteral("en")).toString();
                if (ru.isEmpty() || en.isEmpty()) throw std::runtime_error("Empty translation catalog message");
                const auto [position, inserted] = unique.emplace(ru, en);
                if (!inserted && position->second != en)
                    throw std::runtime_error("Conflicting translation catalog message");
            }
        }
        const QRegularExpression placeholders(QStringLiteral("%[1-9][0-9]*"));
        const QRegularExpression whitespace(QStringLiteral("\\s"));
        for (const auto& [ru, en] : unique) {
            Message message{ru, en, pattern_for(ru), pattern_for(en), {}};
            message.ru_fragment = ru.contains(whitespace);
            message.en_fragment = en.contains(whitespace);
            auto matches = placeholders.globalMatch(ru);
            while (matches.hasNext()) message.placeholders.push_back(matches.next().captured());
            messages.push_back(std::move(message));
        }
        std::stable_sort(messages.begin(), messages.end(), [](const Message& a, const Message& b) {
            return a.ru.size() > b.ru.size();
        });
    }

    QString convert(const QString& input, bool english, int depth = 0) const {
        if (input.isEmpty()) return input;
        const auto protected_ranges = protected_spans(input);
        if (protected_ranges.size() == 1 && protected_ranges.front().first == 0 &&
            protected_ranges.front().second == input.size()) return input;
        for (const auto& message : messages)
            if (input == (english ? message.ru : message.en))
                return english ? message.en : message.ru;
        // Re-render saved dynamic display values from their original template.
        // Parameters retain their content; known nested UI messages translate.
        if (depth < 3) {
            for (const auto& message : messages) {
                if (message.placeholders.empty()) continue;
                const auto match = (english ? message.ru_pattern : message.en_pattern).match(input);
                if (!match.hasMatch()) continue;
                const auto& destination = english ? message.en : message.ru;
                std::map<QString, QString> arguments;
                const QRegularExpression tokens(QStringLiteral("%[1-9][0-9]*"));
                auto source_slots = tokens.globalMatch(english ? message.ru : message.en);
                int capture = 1;
                while (source_slots.hasNext()) arguments[source_slots.next().captured()] =
                    convert(match.captured(capture++), english, depth + 1);
                QString output;
                qsizetype offset = 0;
                auto destination_slots = tokens.globalMatch(destination);
                while (destination_slots.hasNext()) {
                    const auto slot = destination_slots.next();
                    output += destination.mid(offset, slot.capturedStart() - offset);
                    output += arguments.at(slot.captured());
                    offset = slot.capturedEnd();
                }
                return output + destination.mid(offset);
            }
        }
        // Core errors can carry platform codes, filenames and nested errors.
        // Translate catalogued fragments once, never by cascaded replacement.
        QString result;
        std::size_t protected_index = 0;
        for (qsizetype offset = 0; offset < input.size();) {
            if (protected_index < protected_ranges.size() &&
                offset == protected_ranges[protected_index].first) {
                const auto end = protected_ranges[protected_index++].second;
                result += input.mid(offset, end - offset);
                offset = end;
                continue;
            }
            const auto next_protected = protected_index < protected_ranges.size()
                ? protected_ranges[protected_index].first : input.size();
            const Message* best = nullptr;
            qsizetype length = 0;
            for (const auto& message : messages) {
                const auto& source = english ? message.ru : message.en;
                if (!message.placeholders.empty() || source.size() < 4 || source.size() <= length ||
                    !(english ? message.ru_fragment : message.en_fragment) ||
                    offset + source.size() > next_protected) continue;
                if (!source.isEmpty() && source.front().isLetterOrNumber() && offset > 0 &&
                    input[offset - 1].isLetterOrNumber()) continue;
                if (!source.isEmpty() && source.back().isLetterOrNumber() &&
                    offset + source.size() < input.size() &&
                    input[offset + source.size()].isLetterOrNumber()) continue;
                if (input.mid(offset, source.size()) == source) {
                    best = &message;
                    length = source.size();
                }
            }
            if (best) {
                result += english ? best->en : best->ru;
                offset += length;
            } else result += input[offset++];
        }
        return result;
    }

    void add(QObject* object, Display kind, const QByteArray& property = {}, int index = -1,
             const QString& html = {}) {
        const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const Binding& value) {
            return value.object == object && value.kind == kind && value.property == property && value.index == index;
        });
        if (found != bindings.end()) return;
        Binding binding{object, kind, property, index, {}, html};
        const auto display = kind == Display::html ? html : binding.read();
        binding.source = current.load() == UiLanguage::english ? convert(display, false) : display;
        binding.rendered = display;
        bindings.push_back(std::move(binding));
        if (current.load() == UiLanguage::english) bindings.back().write(convert(bindings.back().source, true));
    }

    void watch_object(QObject* object) {
        for (const auto* property : {"toolTip", "statusTip", "whatsThis", "accessibleName",
                "accessibleDescription", "windowTitle", "placeholderText", "title", "suffix", "prefix"}) {
            const int index = object->metaObject()->indexOfProperty(property);
            if (index >= 0 && object->metaObject()->property(index).isWritable()) add(object, Display::property, property);
        }
        if (qobject_cast<QLabel*>(object) || qobject_cast<QAbstractButton*>(object) || qobject_cast<QAction*>(object))
            add(object, Display::property, "text");
        if (auto* combo = qobject_cast<QComboBox*>(object))
            for (int index = 0; index < combo->count(); ++index) {
                add(object, Display::combo, {}, index);
                add(object, Display::combo_tip, {}, index);
            }
        if (auto* tabs = qobject_cast<QTabWidget*>(object))
            for (int index = 0; index < tabs->count(); ++index) {
                add(object, Display::tab, {}, index);
                add(object, Display::tab_tip, {}, index);
            }
    }
};

Localizer& localizer() { static Localizer instance; return instance; }

}  // namespace

UiLanguage language() { return localizer().current.load(); }
QString text(const QString& russian) {
    auto& state = localizer();
    return state.current.load() == UiLanguage::english ? state.convert(russian, true) : russian;
}
QString source_text(const QString& displayed) {
    return language() == UiLanguage::english ? localizer().convert(displayed, false) : displayed;
}
int message_count() { return static_cast<int>(localizer().messages.size()); }

void set_language(UiLanguage value) {
    auto& state = localizer();
    // Record changed statuses/results before changing the language.
    for (auto& binding : state.bindings) {
        if (!binding.object) continue;
        const auto display = binding.read();
        if (display != binding.rendered) {
            binding.source = state.current.load() == UiLanguage::english ? state.convert(display, false) : display;
            binding.rendered = display;
        }
    }
    state.current.store(value);
    if (qApp) {
        if (state.stock_installed) QCoreApplication::removeTranslator(&state.stock);
        state.stock_installed = value == UiLanguage::russian;
        if (state.stock_installed) QCoreApplication::installTranslator(&state.stock);
    }
    for (auto& binding : state.bindings)
        if (binding.object) binding.write(value == UiLanguage::english ? state.convert(binding.source, true) : binding.source);
    std::erase_if(state.bindings, [](const Binding& binding) { return binding.object.isNull(); });
    std::erase_if(state.roots, [](const QPointer<QWidget>& root) { return root.isNull(); });
    // Custom-painted overlays and state-dependent captions must refresh even
    // on the first EN switch, when no Qt stock translator has been installed.
    const auto watched_roots = state.roots;
    for (const auto& root : watched_roots) {
        if (!root) continue;
        QEvent changed(QEvent::LanguageChange);
        QCoreApplication::sendEvent(root, &changed);
    }
}

void watch(QWidget* root) {
    auto& state = localizer();
    if (std::find(state.roots.begin(), state.roots.end(), root) == state.roots.end())
        state.roots.emplace_back(root);
    state.watch_object(root);
    for (auto* child : root->findChildren<QObject*>()) state.watch_object(child);
}

void bind_html(QTextBrowser* browser, const QString& content) {
    auto& state = localizer();
    const auto source = state.current.load() == UiLanguage::english
        ? state.convert(content, false) : content;
    state.add(browser, Display::html, {}, -1, content);
    const auto binding = std::find_if(state.bindings.begin(), state.bindings.end(),
        [browser](const Binding& value) {
            return value.object == browser && value.kind == Display::html;
        });
    if (binding != state.bindings.end()) {
        binding->source = source;
        binding->write(state.current.load() == UiLanguage::english
            ? state.convert(source, true) : source);
    }
}

}  // namespace wardogs::i18n
