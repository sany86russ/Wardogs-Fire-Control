#pragma once

#include "wardogs/language.hpp"

#include <QString>

class QWidget;
class QTextBrowser;

namespace wardogs::i18n {

UiLanguage language();
void set_language(UiLanguage value);
QString text(const QString& russian);
// Registers only display properties. Editable values and item data are never
// translated. Existing windows and calculations remain alive when switching.
void watch(QWidget* root);
void bind_html(QTextBrowser* browser, const QString& content);
QString source_text(const QString& displayed);
int message_count();

}  // namespace wardogs::i18n
