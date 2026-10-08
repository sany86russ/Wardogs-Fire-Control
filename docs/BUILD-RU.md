# Сборка WARDOGS Fire Control

[English](BUILD-EN.md) · [Описание программы](../README.md)

## Что требуется

- Windows x64; проверенный комплект разработки — Visual Studio 2022 с рабочей нагрузкой **«Разработка классических приложений на C++»**, Windows SDK и встроенными CMake / Ninja.
- CMake 3.24 или новее, C++20, MSVC x64. Скрипт использует CMake и Ninja из выбранной через `vswhere` Visual Studio.
- Qt **6.8.3**, комплект **MSVC 2022 64-bit**. CMake допускает Qt 6.8+, но публичная сборка фиксирует 6.8.3 для воспроизводимой версии библиотек и соответствующих исходников.
- PowerShell. Git нужен для клонирования и для создания архива исходников из коммита. Python 3.11 и `aqtinstall` нужны только для установки Qt в CI; приложение их не использует.

Исходники Zstandard, заголовки и x64-библиотеки ONNX Runtime, распознаватель PP-OCRv6 и его лицензия находятся в репозитории. Сама C++-сборка не скачивает зависимости. Первичная установка Visual Studio / Qt требует отдельных загрузок. Настройки, игровые журналы и реальные высотные пакеты не требуются для сборки и тестов.

## Получение исходников

```powershell
git clone https://github.com/sany86russ/Wardogs-Fire-Control.git
Set-Location Wardogs-Fire-Control
```

Можно также скачать `WardogsFireControl-v<версия>-source.zip` из Releases и распаковать целиком. Архив содержит код, тесты, модель, ONNX Runtime, документацию, лицензии и сборочные скрипты; создание нового архива через `-IncludeSource` требует Git-клона.

## Сборка и тесты

Qt должен включать модули **Widgets** и **Network**, а также TLS-плагин **Schannel**. Публичный комплект содержит `Qt6Network.dll`, `tls/qschannelbackend.dll` и помощник `Update.ps1`; для TLS используются сертификаты Windows.

Укажите путь к установленному комплекту Qt:

```powershell
$env:QT_ROOT = 'C:\Qt\6.8.3\msvc2022_64'
.\Build.ps1 -Configuration Release
```

Готовый EXE находится в `source/build/release/`. Скрипт запускает весь CTest последовательно: расчёты, OCR, настройки, локализация, горячие клавиши, захват собственного тестового окна и сценарии приложения RU / EN. Некоторые тесты открывают тестовые окна. Запускайте проверки в подходящей Windows-сессии; результаты сохраняются в `source/build/release/Testing/`. Windows OCR может сообщить CTest `Skipped` с кодом 77, если системный OCR-компонент недоступен; основной встроенный OCR проверяется отдельно.

`Build.ps1 -SkipTests` предназначен для подготовки компиляции. Он не подтверждает готовность релиза и не разрешён вместе с обновлением `-Package`.

## Переносимый пакет

Обычная локальная команда ниже выполняет тесты, **обновляет локальную папку `App/`**, сохраняет предыдущий комплект в `.recovery/releases/` и создаёт ZIP в `dist/`:

```powershell
.\Build.ps1 -Configuration Release -Package
```

Для создания публичного комплекта без замены `App/` используйте путь CI:

```powershell
.\source\build.ps1 -Configuration Release -ParallelJobs 2 -Package -SkipArchive
.\tools\ci-package.ps1 -IncludeQtSources
```

Результат находится в `source/out/public-release/`. Эта папка должна не содержать файлов того же релиза; скрипт отказывается перезаписывать готовые артефакты. При повторной сборке задайте новую папку внутри рабочей копии через `-OutputDirectory`. Добавление `-IncludeSource` создаёт ZIP исходников из текущего **коммита**, поэтому сначала сохраните требуемые изменения в Git.

Пакет включает EXE, Qt DLL / Windows-плагин, официальные DLL Visual C++ из `VC/Redist/MSVC/.../x64/Microsoft.VC143.CRT`, ONNX Runtime, OCR-модель, инструкции RU / EN, скриншоты и лицензии. `package-manifest.json` содержит длину и SHA-256 каждого файла; скрипт проверяет каждый элемент готового ZIP. Игровые `.wdt` в переносимый пакет не допускаются. Синтетический файл `source/tests/data/terrain_test.wdt` сохраняется только в исходниках для проверки формата.

## GitHub Actions и релизы

[`windows.yml`](https://github.com/sany86russ/Wardogs-Fire-Control/blob/main/.github/workflows/windows.yml) выполняется для `main`, веток `codex/*`, Pull Request в `main` и по ручному запуску. Runner — `windows-2022`; Python 3.11, `aqtinstall==3.3.0`, Qt 6.8.3 MSVC x64. Сборка выполняется в Release с двумя процессами, тесты — последовательно. Отчёты тестов и артефакты сохраняются в Actions на 14 дней.

Тег вида `v2.7.0` должен точно соответствовать версии в `source/CMakeLists.txt`. Только успешная сборка такого тега создаёт GitHub Release. Обычная ветка или Pull Request не публикует релиз. В релиз входят:

- `WardogsFireControl-v<версия>-win-x64.zip` — готовая программа;
- `WardogsFireControl-v<версия>-source.zip` — исходники из публичного коммита;
- `qtbase-everywhere-src-6.8.3.tar.xz` и `qttranslations-everywhere-src-6.8.3.tar.xz` — соответствующие исходники Qt;
- `package-manifest.json` и `SHA256SUMS.txt` — контрольные суммы.

Исходники Qt загружаются из [официального архива](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/) с проверкой закреплённых SHA-256. Программа использует Qt динамически; LGPL/GPL, уведомление и сведения о замене совместимых DLL находятся в `licenses/qt/`. [Условия Qt](https://www.qt.io/development/open-source-lgpl-obligations), [Microsoft Redistributable](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170).

Для файлов тегового релиза создаются GitHub attestations. При установленном GitHub CLI происхождение скачанного файла проверяется так:

```powershell
gh attestation verify .\WardogsFireControl-v2.7.0-win-x64.zip --repo sany86russ/Wardogs-Fire-Control
Get-FileHash .\WardogsFireControl-v2.7.0-win-x64.zip -Algorithm SHA256
```

Сравните SHA-256 с `SHA256SUMS.txt` из того же релиза. Проверки CI подтверждают сборку и предусмотренные программные сценарии. Они не подтверждают разрешение издателя игры, результат живого выстрела, работу всех конфигураций мониторов или видимость наложений в настоящем эксклюзивном полноэкранном режиме.
