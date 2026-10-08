# Building WARDOGS Fire Control

[Русский](BUILD-RU.md) · [Application overview](../README.en.md)

## Requirements

- Windows x64; the verified development toolchain is Visual Studio 2022 with **Desktop development with C++**, Windows SDK and bundled CMake / Ninja.
- CMake 3.24 or newer, C++20 and MSVC x64. The script uses CMake and Ninja from the Visual Studio installation selected through `vswhere`.
- Qt **6.8.3**, **MSVC 2022 64-bit**. CMake accepts Qt 6.8+, but the public pipeline pins 6.8.3 to keep library versions and corresponding source consistent.
- PowerShell. Git is required to clone the repository and create a source archive from a commit. Python 3.11 and `aqtinstall` are only used to install Qt in CI; the application does not need them.

Zstandard source, ONNX Runtime headers and x64 libraries, the PP-OCRv6 recognizer and its license are included in the repository. The C++ build itself does not download dependencies. Initial Visual Studio / Qt installation requires separate downloads. User settings, game logs and actual terrain packages are not required to build or test.

## Obtaining source

```powershell
git clone https://github.com/sany86russ/Wardogs-Fire-Control.git
Set-Location Wardogs-Fire-Control
```

Alternatively, download `WardogsFireControl-v<version>-source.zip` from Releases and extract it completely. It includes code, tests, the model, ONNX Runtime, documentation, licenses and build scripts. Creating another archive through `-IncludeSource` requires a Git clone.

## Building and testing

Set the installed Qt SDK path:

```powershell
$env:QT_ROOT = 'C:\Qt\6.8.3\msvc2022_64'
.\Build.ps1 -Configuration Release
```

The executable is generated in `source/build/release/`. The script runs the complete CTest suite sequentially: calculations, OCR, settings, localization, hotkeys, capture of its own fixture window and RU / EN application workflows. Some tests open test windows. Run them in an appropriate Windows session; reports are retained in `source/build/release/Testing/`. Windows OCR may be reported as `Skipped` with code 77 when the system OCR component is unavailable; the main embedded OCR is tested separately.

`Build.ps1 -SkipTests` is intended for compilation preparation. It does not establish release readiness and cannot be combined with a `-Package` update.

## Portable packaging

The ordinary local command below runs tests, **updates the local `App/` directory**, backs up the previous package in `.recovery/releases/` and creates a ZIP in `dist/`:

```powershell
.\Build.ps1 -Configuration Release -Package
```

To produce the public package without replacing `App/`, use the CI path:

```powershell
.\source\build.ps1 -Configuration Release -ParallelJobs 2 -Package -SkipArchive
.\tools\ci-package.ps1 -IncludeQtSources
```

Output is written to `source/out/public-release/`. This directory must not contain artifacts with the same names; the script refuses to overwrite existing release files. For another run, select a new directory within the checkout through `-OutputDirectory`. Adding `-IncludeSource` produces the source ZIP from the current **commit**, so commit the intended changes first.

The package contains the executable, Qt DLLs / Windows plugin, official Visual C++ DLLs from `VC/Redist/MSVC/.../x64/Microsoft.VC143.CRT`, ONNX Runtime, OCR model, RU / EN guides, screenshots and licenses. `package-manifest.json` records each file's length and SHA-256; the script verifies every entry of the completed ZIP. Game `.wdt` files are rejected from portable packages. The synthetic `source/tests/data/terrain_test.wdt` remains only in the source tree to test the format.

## GitHub Actions and releases

[`windows.yml`](https://github.com/sany86russ/Wardogs-Fire-Control/blob/main/.github/workflows/windows.yml) runs for `main`, `codex/*` branches, Pull Requests targeting `main`, and manual dispatch. The runner is `windows-2022`, with Python 3.11, `aqtinstall==3.3.0` and Qt 6.8.3 MSVC x64. Release builds use two compilation processes; tests run sequentially. Test reports and build artifacts are retained in Actions for 14 days.

A tag such as `v2.7.0` must exactly match the version in `source/CMakeLists.txt`. Only a successful build of such a tag creates a GitHub Release. Branch and Pull Request builds do not publish releases. Release assets include:

- `WardogsFireControl-v<version>-win-x64.zip` — portable application;
- `WardogsFireControl-v<version>-source.zip` — source from the public commit;
- `qtbase-everywhere-src-6.8.3.tar.xz` and `qttranslations-everywhere-src-6.8.3.tar.xz` — corresponding Qt source;
- `package-manifest.json` and `SHA256SUMS.txt` — checksums.

Qt source is downloaded from the [official archive](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/) and checked against pinned SHA-256 values. The application dynamically links Qt; LGPL/GPL texts, attribution and instructions for replacing compatible DLLs are in `licenses/qt/`. [Qt obligations](https://www.qt.io/development/open-source-lgpl-obligations), [Microsoft Redistributable documentation](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170).

Tag release files receive GitHub attestations. With GitHub CLI installed, verify the origin of a downloaded file:

```powershell
gh attestation verify .\WardogsFireControl-v2.7.0-win-x64.zip --repo sany86russ/Wardogs-Fire-Control
Get-FileHash .\WardogsFireControl-v2.7.0-win-x64.zip -Algorithm SHA256
```

Compare SHA-256 with `SHA256SUMS.txt` from the same release. CI checks establish a successful build and the implemented software workflows. They do not establish game publisher approval, a live shot's result, support for every monitor configuration or overlay visibility in true exclusive fullscreen.
