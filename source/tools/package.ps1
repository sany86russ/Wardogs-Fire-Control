[CmdletBinding()]
param(
    [switch]$SkipArchive
)

$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $sourceRoot '..')).TrimEnd('\')
$workspacePrefix = $workspaceRoot + '\'

function Assert-WorkspacePath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $absolute = [IO.Path]::GetFullPath($Path)
    if (-not $absolute.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Операция вне папки проекта запрещена: $absolute"
    }
    $ancestor = $absolute
    while ($ancestor -and $ancestor.StartsWith($workspaceRoot, [StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Ссылки и перенаправленные папки в пути пакета не поддерживаются: $ancestor"
            }
        }
        if ($ancestor -eq $workspaceRoot) { break }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    return $absolute
}

function Move-OwnedDirectory {
    param([string]$From, [string]$To)
    $checkedFrom = Assert-WorkspacePath $From
    $checkedTo = Assert-WorkspacePath $To
    [IO.Directory]::Move($checkedFrom, $checkedTo)
}

function Move-OwnedFile {
    param([string]$From, [string]$To)
    $checkedFrom = Assert-WorkspacePath $From
    $checkedTo = Assert-WorkspacePath $To
    [IO.File]::Move($checkedFrom, $checkedTo)
}

function Remove-OwnedStage {
    param([string]$Path)
    $checked = Assert-WorkspacePath $Path
    if (-not (Test-Path -LiteralPath $checked)) { return }
    foreach ($entry in Get-ChildItem -LiteralPath $checked -Recurse -Force) {
        [void](Assert-WorkspacePath $entry.FullName)
    }
    Remove-Item -LiteralPath $checked -Recurse -Force
}

function Get-RelativePackagePath {
    param([string]$Root, [string]$Path)
    return $Path.Substring($Root.TrimEnd('\').Length + 1).Replace('\', '/')
}

function Get-VisualCppRedistributable {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Не найден установщик Visual Studio для получения официальных библиотек C++.'
    }
    $visualStudio = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $visualStudio) { throw 'Не найдены официальные библиотеки C++ из Visual Studio.' }
    $redistRoot = Join-Path $visualStudio 'VC\Redist\MSVC'
    $candidates = @(Get-ChildItem -LiteralPath $redistRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
        Sort-Object { [Version]$_.Name } -Descending)
    foreach ($candidate in $candidates) {
        $crt = Join-Path $candidate.FullName 'x64\Microsoft.VC143.CRT'
        if (Test-Path -LiteralPath $crt -PathType Container) {
            return [pscustomobject]@{ directory = $crt; version = $candidate.Name }
        }
    }
    throw 'Не найдены x64 redistributable DLL Visual C++ 2022. Установите компонент C++ в Visual Studio.'
}

function Assert-ZipContents {
    param([string]$Archive, [object[]]$Files)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        foreach ($file in $Files) {
            $entry = $zip.GetEntry($file.path)
            if ($null -eq $entry -or $entry.Length -ne $file.length) {
                throw "Архив неполон: $($file.path)"
            }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
                if ($actual -ne $file.sha256) {
                    throw "Не совпадает контрольная сумма файла в ZIP: $($file.path)"
                }
            } finally {
                $sha.Dispose()
                $stream.Dispose()
            }
        }
    } finally { $zip.Dispose() }
}

$packageRoot = Assert-WorkspacePath (Join-Path $sourceRoot 'out\package')
$appRoot = Assert-WorkspacePath (Join-Path $workspaceRoot 'App')
$distRoot = Assert-WorkspacePath (Join-Path $workspaceRoot 'dist')
$releasesRoot = Assert-WorkspacePath (Join-Path $workspaceRoot '.recovery\releases')
$projectDeclaration = Select-String -LiteralPath (Join-Path $sourceRoot 'CMakeLists.txt') `
    -Pattern 'project\(WarDogsDistanceCalculator VERSION ([0-9]+\.[0-9]+\.[0-9]+)'
if (-not $projectDeclaration) { throw 'Не удалось определить версию проекта.' }
$version = $projectDeclaration.Matches[0].Groups[1].Value
$archiveName = "WardogsFireControl-v$version-win-x64.zip"
$archivePath = Assert-WorkspacePath (Join-Path $distRoot $archiveName)
$required = @(
    'WarDogsDistanceCalculator.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll',
    'onnxruntime.dll', 'platforms\qwindows.dll', 'models\PP-OCRv6_rec_small.onnx',
    'LICENSE', 'THIRD_PARTY_NOTICES.md', 'models\LICENSE.PaddleOCR.txt',
    'licenses\onnxruntime\LICENSE.txt', 'licenses\qt\LGPL-3.0.txt',
    'licenses\qt\GPL-3.0.txt', 'licenses\zstd\LICENSE.txt', 'TERRAIN_DATA_NOTICE.md',
    'ANTICHEAT-RU.md', 'SUPPORT-APPROVAL-DRAFT-RU.md'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageRoot $relative) -PathType Leaf)) {
        throw "Сборка пакета не завершена: отсутствует $relative. Выполните .\Build.ps1 -Package."
    }
}
$versionInfo = (Get-Item -LiteralPath (Join-Path $packageRoot 'WarDogsDistanceCalculator.exe')).VersionInfo
if ($versionInfo.FileVersion -ne $version) {
    throw "Версия собранного EXE ($($versionInfo.FileVersion)) не совпадает с проектом ($version)."
}
$redist = Get-VisualCppRedistributable
$requiredRuntime = @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll')
foreach ($name in $requiredRuntime) {
    if (-not (Test-Path -LiteralPath (Join-Path $redist.directory $name) -PathType Leaf)) {
        throw "Официальный пакет Visual C++ неполон: $name"
    }
}
foreach ($process in Get-Process -Name WarDogsDistanceCalculator -ErrorAction SilentlyContinue) {
    try { $processPath = $process.Path } catch { $processPath = $null }
    if ($processPath -and $processPath.StartsWith($appRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Закройте запущенную копию из папки App перед обновлением переносимого пакета.'
    }
}
$originalPath = Join-Path $workspaceRoot 'WarDogsDistanceCalculator.exe'
$originalHash = if (Test-Path -LiteralPath $originalPath -PathType Leaf) {
    (Get-FileHash -LiteralPath $originalPath -Algorithm SHA256).Hash
} else { $null }
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$stageRoot = Assert-WorkspacePath (Join-Path $releasesRoot ('.stage-' + $stamp))
$stageApp = Assert-WorkspacePath (Join-Path $stageRoot 'App')
$stageArchive = Assert-WorkspacePath (Join-Path $stageRoot $archiveName)
$backupRoot = Assert-WorkspacePath (Join-Path $releasesRoot $stamp)
$backupApp = Assert-WorkspacePath (Join-Path $backupRoot 'App')
$backupArchive = Assert-WorkspacePath (Join-Path $backupRoot $archiveName)
$appBackedUp = $false
$archiveBackedUp = $false
$appPublished = $false
$archivePublished = $false
try {
    New-Item -ItemType Directory -Path $stageApp -Force | Out-Null
    foreach ($entry in Get-ChildItem -LiteralPath $packageRoot -Recurse -Force) {
        [void](Assert-WorkspacePath $entry.FullName)
    }
    foreach ($entry in Get-ChildItem -LiteralPath $packageRoot -Force) {
        Copy-Item -LiteralPath $entry.FullName -Destination $stageApp -Recurse -Force
    }
    # Only application-owned files are staged. Independently imported terrain
    # stays in LocalAppData/WardogsFireControl/terrain-packs across upgrades and
    # must never enter this redistributable ZIP. Older install trees may still
    # contain maps: filter only the new stage, preserving evidence/rollback.
    foreach ($terrainFile in Get-ChildItem -LiteralPath $stageApp -Filter '*.wdt' -File -Recurse) {
        [void](Assert-WorkspacePath $terrainFile.FullName)
        Remove-Item -LiteralPath $terrainFile.FullName -Force
    }
    $runtimeFiles = @(Get-ChildItem -LiteralPath $redist.directory -Filter '*.dll' -File)
    foreach ($runtime in $runtimeFiles) {
        Copy-Item -LiteralPath $runtime.FullName -Destination (Join-Path $stageApp $runtime.Name) -Force
    }
    $runtimeNotice = @(
        '# Microsoft Visual C++ Runtime', '',
        ('В комплект включены официальные x64 Redistributable DLL Visual C++ 2022, версия ' + $redist.version + '.'),
        'Источник: установленная Visual Studio, VC/Redist/MSVC/<version>/x64/Microsoft.VC143.CRT.',
        'Использовано локальное размещение рядом с приложением; файлы из System32 не копировались.', '',
        '[Условия и описание redistributable Microsoft](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170).',
        '[Distributable code Visual Studio 2022](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution).', '',
        'Контрольные суммы:', '', '| Файл | Версия | SHA256 |', '|---|---|---|'
    )
    foreach ($runtime in $runtimeFiles) {
        $runtimeNotice += '| ' + $runtime.Name + ' | ' + $runtime.VersionInfo.FileVersion + ' | ' +
            (Get-FileHash -LiteralPath $runtime.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + ' |'
    }
    $microsoftNotices = Join-Path $stageApp 'licenses\microsoft'
    New-Item -ItemType Directory -Path $microsoftNotices -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $microsoftNotices 'REDISTRIBUTABLE-SOURCE.md'),
        ($runtimeNotice -join "`n"), [Text.UTF8Encoding]::new($false))
    foreach ($name in @('QUICKSTART-RU.md', 'RELEASE-NOTES-RU.md',
            'ARCHITECTURE-RU.md', 'ANTICHEAT-RU.md', 'SUPPORT-APPROVAL-DRAFT-RU.md', 'CALCULATIONS-RU.md',
            'QUICKSTART-EN.md', 'CALCULATIONS-EN.md', 'RELEASE-NOTES-EN.md', 'LOCALIZATION.md',
            'ARCHITECTURE-EN.md', 'ANTICHEAT-EN.md')) {
        Copy-Item -LiteralPath (Join-Path $sourceRoot ('docs\' + $name)) `
            -Destination (Join-Path $stageApp $name) -Force
    }
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'docs\LOCALIZATION.md') `
        -Destination (Join-Path $stageApp 'README.md') -Force
    Copy-Item -LiteralPath (Join-Path $workspaceRoot 'Launch.ps1') `
        -Destination (Join-Path $stageApp 'Launch.ps1') -Force
    $portableLauncher = "@echo off`r`nstart `"`" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"%~dp0Launch.ps1`" -Portable`r`nexit /b`r`n"
    [IO.File]::WriteAllText((Join-Path $stageApp 'Запустить.cmd'), $portableLauncher, [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $stageApp 'Start.cmd'), $portableLauncher, [Text.Encoding]::ASCII)
    $manifestPath = Join-Path $stageApp 'package-manifest.json'
    $files = @(Get-ChildItem -LiteralPath $stageApp -File -Recurse -Force |
        Where-Object { $_.FullName -ne $manifestPath } | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = Get-RelativePackagePath $stageApp $_.FullName
            length = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    $manifest = [ordered]@{
        product = 'WARDOGS Fire Control'
        version = $version
        platform = 'Windows x64'
        configuration = 'Release'
        visual_cpp_redistributable_version = $redist.version
        generated_utc = [DateTime]::UtcNow.ToString('o')
        files = $files
    }
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
    if (-not $SkipArchive) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::CreateFromDirectory($stageApp, $stageArchive,
            [IO.Compression.CompressionLevel]::Optimal, $false)
        $archiveFiles = $files + @([ordered]@{
            path = 'package-manifest.json'
            length = (Get-Item -LiteralPath $manifestPath).Length
            sha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
        })
        Assert-ZipContents $stageArchive $archiveFiles
    }
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    if (-not $SkipArchive -and (Test-Path -LiteralPath $archivePath)) {
        Move-OwnedFile $archivePath $backupArchive
        $archiveBackedUp = $true
    }
    if (Test-Path -LiteralPath $appRoot) {
        Move-OwnedDirectory $appRoot $backupApp
        $appBackedUp = $true
    }
    Move-OwnedDirectory $stageApp $appRoot
    $appPublished = $true
    if (-not $SkipArchive) {
        New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
        Move-OwnedFile $stageArchive $archivePath
        $archivePublished = $true
    }
    if ($originalHash -and (Get-FileHash -LiteralPath $originalPath -Algorithm SHA256).Hash -ne $originalHash) {
        throw 'Контрольная сумма исходного EXE изменилась во время подготовки пакета.'
    }
    Write-Host "Готовая переносимая версия: $appRoot"
    if (-not $SkipArchive) { Write-Host "Архив: $archivePath" }
    if ($appBackedUp -or $archiveBackedUp) { Write-Host "Предыдущая версия сохранена: $backupRoot" }
} catch {
    $deliveryError = $_
    # Keep both old deliveries recoverable if publishing either component fails.
    if ($archivePublished) { Move-OwnedFile $archivePath $stageArchive }
    if ($appPublished) { Move-OwnedDirectory $appRoot $stageApp }
    if ($archiveBackedUp) { Move-OwnedFile $backupArchive $archivePath }
    if ($appBackedUp) { Move-OwnedDirectory $backupApp $appRoot }
    throw $deliveryError
} finally {
    Remove-OwnedStage $stageRoot
}
