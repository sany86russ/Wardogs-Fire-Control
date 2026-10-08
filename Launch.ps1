[CmdletBinding()]
param(
    [switch]$Portable,
    [switch]$NoDialog
)

$ErrorActionPreference = 'Stop'
$launchEnglish = $false
$launchSettingsPath = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'WardogsFireControl\settings.ini'
try {
    if (Test-Path -LiteralPath $launchSettingsPath -PathType Leaf) {
        $launchEnglish = [IO.File]::ReadAllText($launchSettingsPath) -match '(?im)^ui_language=en\s*$'
    }
    $launchDirectory = if ($Portable) { $PSScriptRoot } else {
        Join-Path $PSScriptRoot 'App'
    }
    $launchDirectory = [IO.Path]::GetFullPath($launchDirectory)
    $executable = Join-Path $launchDirectory 'WarDogsDistanceCalculator.exe'
    $required = @(
        'WarDogsDistanceCalculator.exe', 'Qt6Core.dll', 'Qt6Gui.dll',
        'Qt6Widgets.dll', 'Qt6Network.dll', 'tls\qschannelbackend.dll',
        'Update.ps1', 'onnxruntime.dll', 'platforms\qwindows.dll',
        'models\PP-OCRv6_rec_small.onnx', 'msvcp140.dll', 'msvcp140_1.dll',
        'msvcp140_2.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'
    )
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $launchDirectory $relative) -PathType Leaf)) {
            if ($launchEnglish) {
                throw "The portable package is missing or incomplete: $relative. Build it with .\Build.ps1 -Package or extract the complete ZIP."
            }
            throw "Переносимый пакет отсутствует или неполон: $relative. Соберите проект командой .\Build.ps1 -Package либо распакуйте готовый ZIP целиком."
        }
    }
    Start-Process -FilePath $executable -WorkingDirectory $launchDirectory `
        -WindowStyle Hidden | Out-Null
} catch {
    $launchFailureTitle = if ($launchEnglish) { 'Could not start WARDOGS Fire Control.' } else { 'Не удалось запустить WARDOGS Fire Control.' }
    $message = $launchFailureTitle + "`n`n" + $_.Exception.Message
    if ($NoDialog) {
        Write-Error -Message $message -ErrorAction Continue
    } else {
        Add-Type -AssemblyName PresentationFramework
        [Windows.MessageBox]::Show($message, 'WARDOGS Fire Control', 'OK', 'Error') | Out-Null
    }
    exit 1
}
