[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 64)]
    [int]$ParallelJobs = 1,
    [switch]$SkipTests,
    [switch]$Package,
    [switch]$SkipArchive
)

$ErrorActionPreference = 'Stop'
if ($Package -and $Configuration -ne 'Release') {
    throw 'Переносимый пакет собирается в Release. Используйте .\Build.ps1 -Package.'
}
if ($Package -and $SkipTests) {
    throw 'Обновление App требует выполненных проверок. Для подготовки без запуска тестов используйте сборку без -Package.'
}
$previousQtRoot = $env:QT_ROOT
$previousVsLanguage = $env:VSLANG
$buildProcess = [Diagnostics.Process]::GetCurrentProcess()
$previousBuildPriority = $buildProcess.PriorityClass
try {
    $buildProcess.PriorityClass = [Diagnostics.ProcessPriorityClass]::Idle
    if (-not $env:QT_ROOT) {
        $localQtRoot = Join-Path $PSScriptRoot '.tools\Qt'
        if (-not (Test-Path -LiteralPath (Join-Path $localQtRoot 'lib\cmake\Qt6\Qt6Config.cmake'))) {
            throw 'Qt SDK отсутствует. Укажите установленный Qt 6.8 MSVC x64 в QT_ROOT.'
        }
        $env:QT_ROOT = $localQtRoot
    }
    # Prefer English diagnostics when that language pack is installed.
    $env:VSLANG = '1033'
    & (Join-Path $PSScriptRoot 'source\build.ps1') `
        -Configuration $Configuration -ParallelJobs $ParallelJobs -SkipTests:$SkipTests -Package:$Package -SkipArchive
    if ($Package) {
        & (Join-Path $PSScriptRoot 'source\tools\package.ps1') -SkipArchive:$SkipArchive
    }
} finally {
    $buildProcess.PriorityClass = $previousBuildPriority
    $env:QT_ROOT = $previousQtRoot
    $env:VSLANG = $previousVsLanguage
}
