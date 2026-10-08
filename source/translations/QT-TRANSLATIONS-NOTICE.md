# Qt stock-control translations

`qtbase_ru.json` contains a selected subset of the Russian Qt Widgets translations from the official Qt translations repository, tag **v6.8.3**. Application-owned RU/EN messages remain in the other JSON catalogs.

Source: [qtbase_ru.ts at v6.8.3](https://raw.githubusercontent.com/qt/qttranslations/v6.8.3/translations/qtbase_ru.ts).

Original source SHA-256:

```text
0daef5edb8f40287917370f0d0113b9e5987daea6bace45a263aa7c3dce8a41b
```

The complete, unmodified TS source is preserved in `third_party/qt/qtbase_ru-6.8.3.ts`. The derived JSON keeps the original completed translations for selected contexts covering file dialogs, standard buttons, editable text, spin boxes, accessibility, scrollbars and tabs. It omits unrelated contexts and unfinished/obsolete/vanished messages. Reproduce the extraction from the source directory with:

```powershell
py -3.11 tools/import_qt_translations.py third_party/qt/qtbase_ru-6.8.3.ts translations/qtbase_ru.json
```

The importer uses local files; the build does not download translation data.

The module's default license rule in the official [Qt translations licenseRule.json at v6.8.3](https://github.com/qt/qttranslations/blob/v6.8.3/licenseRule.json) lists commercial licensing or **LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only**. The translation subset is not declared BSD or relicensed under the application's MIT license. Existing Qt license copies are preserved in `third_party/qt/LGPL-3.0.txt` and `third_party/qt/GPL-3.0.txt`; this notice does not grant additional rights or establish legal compliance on its own.
