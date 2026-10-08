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
$projectRoot = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $visualStudio) {
    throw 'Не найдена Visual Studio 2022 с компонентом «Разработка классических приложений на C++».'
}

$developerPrompt = Join-Path $visualStudio 'Common7\Tools\VsDevCmd.bat'
$cmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$qtRoot = if ($env:QT_ROOT) { $env:QT_ROOT } else { 'D:\Qt\6.8.3\msvc2022_64' }
$qtCmake = Join-Path $qtRoot 'lib\cmake'
$qtDeploy = Join-Path $qtRoot 'bin\windeployqt.exe'
if (-not (Test-Path $qtCmake) -or -not (Test-Path $qtDeploy)) {
    throw "Не найден комплект разработки Qt 6 C++: $qtRoot"
}
$projectDeclaration = Select-String -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') `
    -Pattern 'project\(WarDogsDistanceCalculator VERSION ([0-9]+\.[0-9]+\.[0-9]+)'
if (-not $projectDeclaration) {
    throw 'Не удалось прочитать версию проекта из CMakeLists.txt.'
}
$projectVersion = $projectDeclaration.Matches[0].Groups[1].Value
$buildDirectory = Join-Path $projectRoot "build\$($Configuration.ToLowerInvariant())"
$installDirectory = Join-Path $projectRoot "out\package"

if ($Package -and (Test-Path $installDirectory)) {
    $resolvedProject = [IO.Path]::GetFullPath($projectRoot).TrimEnd('\') + '\'
    $resolvedInstall = [IO.Path]::GetFullPath($installDirectory)
    if (-not $resolvedInstall.StartsWith($resolvedProject, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Запрещена очистка папки за пределами проекта: $resolvedInstall"
    }
    Remove-Item -LiteralPath $resolvedInstall -Recurse -Force
}

$commands = @(
    'call "{0}" -arch=x64 -host_arch=x64' -f $developerPrompt
    'chcp 65001 >nul'
    '"{0}" -S "{1}" -B "{2}" -G Ninja -DCMAKE_MAKE_PROGRAM="{3}" -DCMAKE_BUILD_TYPE={4} -DCMAKE_PREFIX_PATH="{5}" -DCMAKE_AUTOGEN_PARALLEL=1' -f $cmake, $projectRoot, $buildDirectory, $ninja, $Configuration, $qtCmake
    '"{0}" --build "{1}" --parallel {2}' -f $cmake, $buildDirectory, $ParallelJobs
)
if (-not $SkipTests) {
    $commands += '"{0}" --test-dir "{1}" --output-on-failure --parallel 1' -f (Join-Path (Split-Path $cmake) 'ctest.exe'), $buildDirectory
} else {
    Write-Warning 'Тесты отложены. Эта сборка не подтверждает готовность версии к обновлению App.'
}
if ($Package) {
    $commands += '"{0}" --install "{1}" --prefix "{2}"' -f $cmake, $buildDirectory, $installDirectory
}

& $env:ComSpec /d /s /c ($commands -join ' && ')
if ($LASTEXITCODE -ne 0) {
    throw "Сборка завершилась с ошибкой, код $LASTEXITCODE"
}

if ($Package) {
    & $qtDeploy --release --no-translations --no-opengl-sw `
        --no-system-d3d-compiler `
        --skip-plugin-types generic,iconengines,imageformats,networkinformation,styles,tls `
        --dir $installDirectory `
        (Join-Path $installDirectory 'WarDogsDistanceCalculator.exe')
    if ($LASTEXITCODE -ne 0) {
        throw "Не удалось подготовить библиотеки Qt, код $LASTEXITCODE"
    }
    $qtLicenseDirectory = Join-Path $installDirectory 'licenses\qt'
    New-Item -ItemType Directory -Path $qtLicenseDirectory -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party\qt\LGPL-3.0.txt') `
        -Destination $qtLicenseDirectory
    Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party\qt\GPL-3.0.txt') `
        -Destination $qtLicenseDirectory
    if (-not $SkipArchive) {
        $archive = Join-Path $projectRoot "out\WarDogsDistanceCalculator-v$projectVersion-win-x64.zip"
        Compress-Archive -Path (Join-Path $installDirectory '*') -DestinationPath $archive -Force
        Write-Host "Архив готов: $archive"
    } else {
        Write-Host "Пакет готов: $installDirectory"
    }
} else {
    Write-Host "Программа готова: $(Join-Path $buildDirectory 'WarDogsDistanceCalculator.exe')"
}
