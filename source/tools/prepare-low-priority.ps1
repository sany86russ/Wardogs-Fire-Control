[CmdletBinding()]
param(
    [switch]$InstallLocalTerrain,
    [string]$TerrainSource
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$buildProcess = [Diagnostics.Process]::GetCurrentProcess()
$previousPriority = $buildProcess.PriorityClass
$previousParallelLevel = $env:CMAKE_BUILD_PARALLEL_LEVEL
try {
    $buildProcess.PriorityClass = [Diagnostics.ProcessPriorityClass]::Idle
    $env:CMAKE_BUILD_PARALLEL_LEVEL = '1'
    Write-Output 'Подготовка: Idle, один процесс компиляции; тесты и обновление App отложены.'
    & (Join-Path $workspaceRoot 'Build.ps1') -ParallelJobs 1 -SkipTests
    if ($InstallLocalTerrain) {
        if (-not $TerrainSource) { throw 'Для локальной установки укажите папку ранее полученных пакетов высот.' }
        $probe = Join-Path $workspaceRoot 'source\build\release\terrain_probe.exe'
        & $probe --install ([IO.Path]::GetFullPath($TerrainSource))
        if ($LASTEXITCODE -ne 0) { throw 'Локальные карты высот не прошли проверку установки.' }
    }
    Write-Output 'Подготовленная сборка не запускается и не заменяет работающий App. Перед обновлением выполните Build.ps1 без -SkipTests.'
} finally {
    $env:CMAKE_BUILD_PARALLEL_LEVEL = $previousParallelLevel
    $buildProcess.PriorityClass = $previousPriority
}
