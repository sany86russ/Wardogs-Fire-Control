[CmdletBinding()]
param(
    [string]$PackageDirectory,
    [string]$OutputDirectory,
    [switch]$IncludeSource,
    [switch]$IncludeQtSources
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
$workspacePrefix = $workspaceRoot + '\'
if (-not $PackageDirectory) { $PackageDirectory = Join-Path $workspaceRoot 'source\out\package' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $workspaceRoot 'source\out\public-release' }

function Assert-WorkspacePath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $absolute = [IO.Path]::GetFullPath($Path)
    if (-not $absolute.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Packaging paths must stay inside this checkout: $absolute"
    }
    $ancestor = $absolute
    while ($ancestor -and $ancestor.StartsWith($workspaceRoot, [StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $ancestor) {
            $entry = Get-Item -LiteralPath $ancestor -Force
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Reparse points are not supported in packaging paths: $ancestor"
            }
        }
        if ($ancestor -eq $workspaceRoot) { break }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    return $absolute
}

function Write-Utf8 {
    param([string]$Path, [string]$Content)
    [IO.File]::WriteAllText($Path, $Content, [Text.UTF8Encoding]::new($false))
}

function Get-RelativePackagePath {
    param([string]$Root, [string]$Path)
    return $Path.Substring($Root.TrimEnd('\').Length + 1).Replace('\', '/')
}

function Assert-Zip {
    param([string]$Archive, [object[]]$Files)
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        if ($zip.Entries.Count -ne $Files.Count) { throw "Unexpected file count in $Archive" }
        foreach ($file in $Files) {
            $entry = $zip.GetEntry($file.path)
            if ($null -eq $entry -or $entry.Length -ne $file.length) {
                throw "A packaged file is missing or incomplete: $($file.path)"
            }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
                if ($actual -ne $file.sha256) { throw "ZIP hash mismatch: $($file.path)" }
            } finally {
                $sha.Dispose()
                $stream.Dispose()
            }
        }
    } finally { $zip.Dispose() }
}

$PackageDirectory = Assert-WorkspacePath $PackageDirectory
$OutputDirectory = Assert-WorkspacePath $OutputDirectory
$sourceRoot = Join-Path $workspaceRoot 'source'
$projectDeclaration = Select-String -LiteralPath (Join-Path $sourceRoot 'CMakeLists.txt') `
    -Pattern 'project\(WarDogsDistanceCalculator VERSION ([0-9]+\.[0-9]+\.[0-9]+)'
if (-not $projectDeclaration) { throw 'CMake project version was not found.' }
$version = $projectDeclaration.Matches[0].Groups[1].Value
$archiveName = "WardogsFireControl-v$version-win-x64.zip"
$sourceArchiveName = "WardogsFireControl-v$version-source.zip"
$qtVersion = '6.8.3'
$qtSources = @(
    @{ name = "qtbase-everywhere-src-$qtVersion.tar.xz"; sha256 = '56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80' },
    @{ name = "qttranslations-everywhere-src-$qtVersion.tar.xz"; sha256 = 'c3c61d79c3d8fe316a20b3617c64673ce5b5519b2e45535f49bee313152fa531' }
)
$required = @(
    'WarDogsDistanceCalculator.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll',
    'Qt6Network.dll', 'tls\qschannelbackend.dll', 'Update.ps1',
    'onnxruntime.dll', 'platforms\qwindows.dll', 'models\PP-OCRv6_rec_small.onnx',
    'LICENSE', 'THIRD_PARTY_NOTICES.md', 'TERRAIN_DATA_NOTICE.md',
    'models\LICENSE.PaddleOCR.txt', 'licenses\onnxruntime\LICENSE.txt',
    'licenses\onnxruntime\ThirdPartyNotices.txt', 'licenses\qt\LGPL-3.0.txt',
    'licenses\qt\GPL-3.0.txt', 'licenses\zstd\LICENSE.txt',
    'licenses\qt-translations\QT-TRANSLATIONS-NOTICE.md'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageDirectory $relative) -PathType Leaf)) {
        throw "The CMake install is incomplete: $relative. Run source/build.ps1 -Package -SkipArchive first."
    }
}
if ((Get-Item -LiteralPath (Join-Path $PackageDirectory 'WarDogsDistanceCalculator.exe')).VersionInfo.FileVersion -ne $version) {
    throw 'The installed executable version does not match CMakeLists.txt.'
}
foreach ($qtDll in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll')) {
    $installedQtVersion = (Get-Item -LiteralPath (Join-Path $PackageDirectory $qtDll)).VersionInfo.ProductVersion
    if ($installedQtVersion -notmatch ('^' + [Regex]::Escape($qtVersion) + '(?:\D|$)')) {
        throw "Corresponding-source packaging requires Qt $qtVersion; $qtDll reports $installedQtVersion."
    }
}
if (@(Get-ChildItem -LiteralPath $PackageDirectory -Filter '*.wdt' -File -Recurse).Count -ne 0) {
    throw 'Community game terrain data must not be bundled in the public application package.'
}
foreach ($name in @('README.md', 'README.en.md', 'Launch.ps1')) {
    if (-not (Test-Path -LiteralPath (Join-Path $workspaceRoot $name) -PathType Leaf)) {
        throw "A public distribution file is missing: $name"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $workspaceRoot 'docs') -PathType Container)) {
    throw 'The public docs directory is missing.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $visualStudio) { throw 'Visual Studio 2022 C++ redistributable files were not found.' }
$runtimeDirectory = $null
$runtimeVersion = $null
$runtimeCandidates = @(Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Redist\MSVC') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [Version]$_.Name } -Descending)
foreach ($candidate in $runtimeCandidates) {
    $crtDirectory = Join-Path $candidate.FullName 'x64\Microsoft.VC143.CRT'
    if (Test-Path -LiteralPath $crtDirectory -PathType Container) {
        $runtimeDirectory = $crtDirectory
        $runtimeVersion = $candidate.Name
        break
    }
}
if (-not $runtimeDirectory) { throw 'The official x64 Microsoft.VC143.CRT redistributable directory was not found.' }
foreach ($runtimeName in @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $runtimeDirectory $runtimeName) -PathType Leaf)) {
        throw "The Visual C++ redistributable directory is incomplete: $runtimeName"
    }
}

$plannedNames = @($archiveName, 'package-manifest.json', 'SHA256SUMS.txt')
if ($IncludeSource) { $plannedNames += $sourceArchiveName }
if ($IncludeQtSources) { $plannedNames += @($qtSources | ForEach-Object { $_.name }) }
foreach ($name in $plannedNames) {
    if (Test-Path -LiteralPath (Join-Path $OutputDirectory $name)) {
        throw "Refusing to overwrite an existing release file: $name. Use a new OutputDirectory."
    }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stageRoot = Assert-WorkspacePath (Join-Path $OutputDirectory ('.stage-' + [Guid]::NewGuid().ToString('N')))
$stageApp = Join-Path $stageRoot 'portable'
$stageAssets = Join-Path $stageRoot 'assets'
$publishedFiles = @()
try {
    New-Item -ItemType Directory -Path $stageApp, $stageAssets -Force | Out-Null
    foreach ($entry in Get-ChildItem -LiteralPath $PackageDirectory -Recurse -Force) {
        [void](Assert-WorkspacePath $entry.FullName)
    }
    foreach ($entry in Get-ChildItem -LiteralPath $PackageDirectory -Force) {
        Copy-Item -LiteralPath $entry.FullName -Destination $stageApp -Recurse -Force
    }
    foreach ($name in @('README.md', 'README.en.md', 'Launch.ps1')) {
        Copy-Item -LiteralPath (Join-Path $workspaceRoot $name) -Destination (Join-Path $stageApp $name) -Force
    }
    # Retain the existing package filename as an English documentation alias.
    Copy-Item -LiteralPath (Join-Path $workspaceRoot 'README.en.md') -Destination (Join-Path $stageApp 'README-EN.md') -Force
    foreach ($documentationRoot in @((Join-Path $workspaceRoot 'docs'), (Join-Path $sourceRoot 'docs'))) {
        foreach ($entry in Get-ChildItem -LiteralPath $documentationRoot -Recurse -Force) {
            [void](Assert-WorkspacePath $entry.FullName)
        }
    }
    Copy-Item -LiteralPath (Join-Path $workspaceRoot 'docs') -Destination (Join-Path $stageApp 'docs') -Recurse -Force
    New-Item -ItemType Directory -Path (Join-Path $stageApp 'source') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'docs') -Destination (Join-Path $stageApp 'source\docs') -Recurse -Force
    # CMake's local install flattens these guides. Public documentation keeps
    # the repository layout so ../ and ../../ links stay inside the package.
    foreach ($guide in Get-ChildItem -LiteralPath (Join-Path $sourceRoot 'docs') -File) {
        if ($guide.Name -in @('README.md', 'README.en.md', 'README-EN.md', 'LICENSE',
                'THIRD_PARTY_NOTICES.md', 'TERRAIN_DATA_NOTICE.md')) { continue }
        $flatGuide = Assert-WorkspacePath (Join-Path $stageApp $guide.Name)
        if (Test-Path -LiteralPath $flatGuide -PathType Leaf) {
            Remove-Item -LiteralPath $flatGuide -Force
        }
    }
    foreach ($name in @('LICENSE', 'THIRD_PARTY_NOTICES.md', 'TERRAIN_DATA_NOTICE.md')) {
        Copy-Item -LiteralPath (Join-Path $sourceRoot $name) -Destination (Join-Path $stageApp $name) -Force
        Copy-Item -LiteralPath (Join-Path $sourceRoot $name) -Destination (Join-Path $stageApp ('source\' + $name)) -Force
    }
    if (Test-Path -LiteralPath (Join-Path $workspaceRoot 'LICENSE') -PathType Leaf) {
        Copy-Item -LiteralPath (Join-Path $workspaceRoot 'LICENSE') -Destination (Join-Path $stageApp 'LICENSE') -Force
    }
    $portableLauncher = "@echo off`r`nstart `"`" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"%~dp0Launch.ps1`" -Portable`r`nexit /b`r`n"
    [IO.File]::WriteAllText((Join-Path $stageApp 'Start.cmd'), $portableLauncher, [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $stageApp 'Запустить.cmd'), $portableLauncher, [Text.Encoding]::ASCII)
    $runtimeFiles = @(Get-ChildItem -LiteralPath $runtimeDirectory -Filter '*.dll' -File)
    foreach ($runtime in $runtimeFiles) {
        Copy-Item -LiteralPath $runtime.FullName -Destination (Join-Path $stageApp $runtime.Name) -Force
    }
    $microsoftNoticeDirectory = Join-Path $stageApp 'licenses\microsoft'
    New-Item -ItemType Directory -Path $microsoftNoticeDirectory -Force | Out-Null
    $runtimeNotice = @(
        '# Microsoft Visual C++ Runtime', '',
        "This package contains unmodified official Visual C++ 2022 x64 redistributable DLLs, version $runtimeVersion.",
        'Source: Visual Studio VC/Redist/MSVC/<version>/x64/Microsoft.VC143.CRT. Files are deployed beside the application; they were not copied from System32.', '',
        '[Redistributable terms and deployment documentation](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170).',
        '[Visual Studio 2022 distributable code](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution).', '',
        '| File | Version | SHA-256 |', '| --- | --- | --- |'
    )
    foreach ($runtime in $runtimeFiles) {
        $runtimeNotice += '| ' + $runtime.Name + ' | ' + $runtime.VersionInfo.FileVersion + ' | ' +
            (Get-FileHash -LiteralPath $runtime.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + ' |'
    }
    Write-Utf8 (Join-Path $microsoftNoticeDirectory 'REDISTRIBUTABLE-SOURCE.md') ($runtimeNotice -join "`n")
    $repository = if ($env:GITHUB_REPOSITORY) { $env:GITHUB_REPOSITORY } else { 'sany86russ/Wardogs-Fire-Control' }
    $qtNotice = @(
        '# Qt corresponding source and replacement / Исходники и замена Qt', '',
        "This application dynamically links unmodified Qt $qtVersion Core, Gui and Widgets under LGPLv3. The Qt Windows platform plugin comes from the same QtBase module.",
        'The LGPLv3 and GPLv3 texts accompany this notice. Corresponding QtBase and QtTranslations source archives are distributed with the same GitHub release:', '',
        "- https://github.com/$repository/releases/download/v$version/qtbase-everywhere-src-$qtVersion.tar.xz",
        "- https://github.com/$repository/releases/download/v$version/qttranslations-everywhere-src-$qtVersion.tar.xz", '',
        "Upstream: https://download.qt.io/archive/qt/6.8/$qtVersion/submodules/", '',
        'You may replace Qt DLLs and platform plugins with ABI-compatible versions, rebuild/relink this application using its published source, and reverse engineer it for debugging changes to these libraries. The application does not impose signatures, encryption or installation restrictions preventing replacement. Back up the portable folder, replace the affected Qt DLLs/plugins, retain the model and ONNX Runtime files, then run Start.cmd. Use Qt MSVC x64 builds of compatible version and configuration.', '',
        'Программа динамически использует немодифицированные Qt Core, Gui, Widgets и Windows-плагин. Исходники QtBase и QtTranslations доступны в том же релизе по ссылкам выше. Можно заменять совместимые DLL/плагины Qt, пересобирать приложение и исследовать его для отладки изменений этих библиотек. Перед заменой сохраните копию папки программы; ограничений подписи или шифрования для замены нет.', '',
        '[Building Qt from source](https://doc.qt.io/qt-6.8/build-sources.html).',
        '[Qt open-source obligations](https://www.qt.io/development/open-source-lgpl-obligations).'
    )
    Write-Utf8 (Join-Path $stageApp 'licenses\qt\CORRESPONDING-SOURCE.md') ($qtNotice -join "`n")
    $manifestPath = Join-Path $stageApp 'package-manifest.json'
    $files = @(Get-ChildItem -LiteralPath $stageApp -File -Recurse -Force | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = Get-RelativePackagePath $stageApp $_.FullName
            length = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    $commit = 'source-archive'
    if (Test-Path -LiteralPath (Join-Path $workspaceRoot '.git')) {
        $commit = & git -C $workspaceRoot rev-parse HEAD
        if ($LASTEXITCODE -ne 0) { throw 'The source commit could not be read.' }
    } elseif ($IncludeSource) {
        throw 'IncludeSource requires a Git clone, because the source ZIP is created from the committed public tree.'
    }
    $manifest = [ordered]@{
        product = 'WARDOGS Fire Control'
        version = $version
        platform = 'Windows x64'
        configuration = 'Release'
        source_commit = $commit
        qt_version = $qtVersion
        visual_cpp_redistributable_version = $runtimeVersion
        generated_utc = [DateTime]::UtcNow.ToString('o')
        terrain_data_included = $false
        files = $files
    }
    Write-Utf8 $manifestPath ($manifest | ConvertTo-Json -Depth 6)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archivePath = Join-Path $stageAssets $archiveName
    [IO.Compression.ZipFile]::CreateFromDirectory($stageApp, $archivePath, [IO.Compression.CompressionLevel]::Optimal, $false)
    Assert-Zip $archivePath ($files + @([ordered]@{
        path = 'package-manifest.json'
        length = (Get-Item -LiteralPath $manifestPath).Length
        sha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }))
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $stageAssets 'package-manifest.json')
    if ($IncludeSource) {
        $publicSourcePaths = @('.github', '.gitattributes', '.gitignore', 'LICENSE', 'README.md', 'README.en.md',
            'Build.ps1', 'Launch.ps1', 'Start.cmd', 'Запустить.cmd', 'source', 'docs', 'tools')
        & git -C $workspaceRoot archive --format=zip "--prefix=WardogsFireControl-v$version/" `
            "--output=$(Join-Path $stageAssets $sourceArchiveName)" HEAD -- @publicSourcePaths
        if ($LASTEXITCODE -ne 0) { throw 'The public source archive could not be created.' }
        $sourceZip = [IO.Compression.ZipFile]::OpenRead((Join-Path $stageAssets $sourceArchiveName))
        try {
            foreach ($relative in @('source/CMakeLists.txt', 'source/build.ps1', 'source/src/main_window.cpp',
                    'source/assets/models/PP-OCRv6_rec_small.onnx', 'source/third_party/onnxruntime/lib/x64/onnxruntime.dll',
                    'README.md', 'README.en.md', 'LICENSE', 'Build.ps1', 'tools/ci-package.ps1', '.github/workflows/windows.yml')) {
                if ($null -eq $sourceZip.GetEntry("WardogsFireControl-v$version/$relative")) {
                    throw "A required source archive file is missing: $relative"
                }
            }
            foreach ($entry in $sourceZip.Entries) {
                if ($entry.FullName -match '/(?:analysis|App|dist|\.recovery|\.tools|build|out)/' -or
                    $entry.FullName -match '/source/terrain-packs/[^/]+\.wdt$') {
                    throw "A private or generated path entered the source archive: $($entry.FullName)"
                }
            }
        } finally { $sourceZip.Dispose() }
    }
    if ($IncludeQtSources) {
        foreach ($qtSource in $qtSources) {
            $downloadPath = Join-Path $stageAssets $qtSource.name
            $downloadUrl = "https://download.qt.io/archive/qt/6.8/$qtVersion/submodules/$($qtSource.name)"
            & curl.exe --fail --location --retry 3 --output $downloadPath $downloadUrl
            if ($LASTEXITCODE -ne 0) { throw "The Qt corresponding source download failed: $($qtSource.name)" }
            $downloadHash = (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($downloadHash -ne $qtSource.sha256) { throw "Official Qt source SHA-256 mismatch: $($qtSource.name)" }
        }
    }
    $checksumLines = @(Get-ChildItem -LiteralPath $stageAssets -File | Sort-Object Name | ForEach-Object {
        (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $_.Name
    })
    Write-Utf8 (Join-Path $stageAssets 'SHA256SUMS.txt') (($checksumLines -join "`n") + "`n")
    foreach ($asset in Get-ChildItem -LiteralPath $stageAssets -File) {
        $target = Assert-WorkspacePath (Join-Path $OutputDirectory $asset.Name)
        [IO.File]::Move($asset.FullName, $target)
        $publishedFiles += $target
    }
    Write-Host "Verified public release files: $OutputDirectory"
} catch {
    # Roll back only files created by this invocation; existing output is never overwritten.
    foreach ($createdFile in $publishedFiles) {
        [void](Assert-WorkspacePath $createdFile)
        Remove-Item -LiteralPath $createdFile -Force
    }
    throw
} finally {
    if (Test-Path -LiteralPath $stageRoot) {
        [void](Assert-WorkspacePath $stageRoot)
        foreach ($entry in Get-ChildItem -LiteralPath $stageRoot -Recurse -Force) {
            [void](Assert-WorkspacePath $entry.FullName)
        }
        Remove-Item -LiteralPath $stageRoot -Recurse -Force
    }
}
