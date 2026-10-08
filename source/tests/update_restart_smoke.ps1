# This acceptance test launches real GUI processes ONLY on an isolated GitHub Windows runner.
# It must never be run on a developer's computer.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ArchivePath,
    [Parameter(Mandatory = $true)][string]$PreviousArchivePath,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -cne 'true' -or $env:RUNNER_OS -cne 'Windows' -or -not $env:RUNNER_TEMP) {
    throw 'Real update/restart acceptance is permitted only on an isolated GitHub Actions Windows runner.'
}
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
$checkoutRoot = [IO.Path]::GetDirectoryName($sourceRoot)
$evidenceRoot = Join-Path $sourceRoot 'build\release\Testing\update-restart'
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory).TrimEnd('\')
if ($EvidenceDirectory -ine $evidenceRoot -and
        -not $EvidenceDirectory.StartsWith($evidenceRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'EvidenceDirectory must stay in source/build/release/Testing/update-restart in this checkout.'
}
foreach ($path in @($ArchivePath, $PreviousArchivePath)) {
    if (-not [IO.Path]::IsPathRooted($path) -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "An existing absolute archive path is required: $path"
    }
}
$ArchivePath = [IO.Path]::GetFullPath($ArchivePath)
$PreviousArchivePath = [IO.Path]::GetFullPath($PreviousArchivePath)
$runnerRoot = [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\')
if (-not (Test-Path -LiteralPath $runnerRoot -PathType Container) -or $runnerRoot -eq [IO.Path]::GetPathRoot($runnerRoot).TrimEnd('\')) {
    throw 'RUNNER_TEMP must identify a real temporary directory, not a volume root.'
}
$helperSource = Join-Path $sourceRoot 'tools\Update.ps1'
. $helperSource
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class WardogsRestartSmokeWindows
{
    [return: MarshalAs(UnmanagedType.Bool)]
    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    private static extern IntPtr GetWindow(IntPtr window, uint command);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetWindowLongPtrW")]
    private static extern IntPtr GetWindowLongPtr(IntPtr window, int index);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, StringBuilder text, int capacity);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    public static int LastPostMessageError { get; private set; }

    public sealed class WindowCandidate
    {
        public long Handle { get; set; }
        public uint ProcessId { get; set; }
        public string Title { get; set; }
        public string ClassName { get; set; }
        public long ExtendedStyle { get; set; }
        public bool IsToolWindow { get; set; }
        public bool IsOwned { get; set; }
        public bool IsVisible { get; set; }
        public bool Eligible { get; set; }
    }

    public static long ExtendedStyle(IntPtr window)
    {
        return GetWindowLongPtr(window, -20).ToInt64();
    }

    public static string WindowClass(IntPtr window)
    {
        var text = new StringBuilder(256);
        GetClassName(window, text, text.Capacity);
        return text.ToString();
    }

    public static bool OwnsMainWindow(IntPtr window, uint processId)
    {
        // Qt::Tool ghost/overlay windows can inherit the application's caption.
        // They must never qualify as the application's main window.
        if (window == IntPtr.Zero || !IsWindow(window)) return false;
        uint ownerProcess;
        GetWindowThreadProcessId(window, out ownerProcess);
        if (ownerProcess != processId) return false;
        if (GetWindow(window, 4) != IntPtr.Zero || (ExtendedStyle(window) & 0x80) != 0) return false;
        var title = new StringBuilder(256);
        GetWindowText(window, title, title.Capacity);
        return title.ToString() == "WARDOGS Fire Control";
    }

    public static IntPtr FindMainWindow(uint processId)
    {
        IntPtr visible = IntPtr.Zero;
        IntPtr hidden = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            if (!OwnsMainWindow(window, processId)) return true;
            if (IsWindowVisible(window))
            {
                if (visible == IntPtr.Zero) visible = window;
            }
            else if (hidden == IntPtr.Zero) hidden = window;
            return true;
        }, IntPtr.Zero);
        return visible != IntPtr.Zero ? visible : hidden;
    }

    public static WindowCandidate[] WindowCandidates(uint processId)
    {
        var results = new List<WindowCandidate>();
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint ownerProcess;
            GetWindowThreadProcessId(window, out ownerProcess);
            if (ownerProcess != processId) return true;
            var title = new StringBuilder(256);
            GetWindowText(window, title, title.Capacity);
            long style = ExtendedStyle(window);
            results.Add(new WindowCandidate {
                Handle = window.ToInt64(), ProcessId = ownerProcess, Title = title.ToString(),
                ClassName = WindowClass(window), ExtendedStyle = style,
                IsToolWindow = (style & 0x80) != 0, IsOwned = GetWindow(window, 4) != IntPtr.Zero,
                IsVisible = IsWindowVisible(window), Eligible = OwnsMainWindow(window, processId)
            });
            return true;
        }, IntPtr.Zero);
        return results.ToArray();
    }

    public static bool RequestClose(IntPtr window, uint processId)
    {
        LastPostMessageError = 0;
        if (!OwnsMainWindow(window, processId)) return false;
        bool delivered = PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
        if (!delivered) LastPostMessageError = Marshal.GetLastWin32Error();
        return delivered;
    }

    public static uint ProcessExitCode(IntPtr process)
    {
        uint exitCode;
        if (process == IntPtr.Zero) throw new InvalidOperationException("A retained process handle is required.");
        if (!GetExitCodeProcess(process, out exitCode)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return exitCode;
    }
}
'@
foreach ($path in @($ArchivePath, $PreviousArchivePath, $EvidenceDirectory, $runnerRoot)) {
    [void](Assert-WardogsPathWithoutReparse $path)
}
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null

function Read-SmokeRelease {
    param([string]$Path, [string]$Destination, [switch]$RequireUpdater)
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        if ($zip.Entries.Count -lt 2 -or $zip.Entries.Count -gt $script:MaximumFiles + 1) { throw 'Release ZIP file count is invalid.' }
        $entries = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
        [long]$size = 0
        foreach ($entry in $zip.Entries) {
            [void](Assert-WardogsPackagePath $entry.FullName)
            $attributes = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$entry.ExternalAttributes), 0)
            $unixType = ($attributes -shr 16) -band 0xf000
            if (($unixType -ne 0 -and $unixType -ne 0x8000) -or ($attributes -band 0x410) -ne 0 -or
                    $entry.Length -gt $script:MaximumFileBytes -or $entries.ContainsKey($entry.FullName)) {
                throw "Release ZIP entry is unsafe: $($entry.FullName)"
            }
            $entries.Add($entry.FullName, $entry)
            $size += $entry.Length
            if ($size -gt $script:MaximumExpandedBytes) { throw 'Release ZIP exceeds the expanded-size limit.' }
        }
        if (-not $entries.ContainsKey('package-manifest.json')) { throw 'Release ZIP has no package manifest.' }
        $manifestEntry = $entries['package-manifest.json']
        if ($manifestEntry.Length -gt $script:MaximumManifestBytes) { throw 'Release manifest exceeds the size limit.' }
        $inputStream = $manifestEntry.Open()
        $buffer = [IO.MemoryStream]::new()
        try {
            [void](Get-WardogsBoundedStreamHash $inputStream $manifestEntry.Length $buffer)
            $json = [Text.UTF8Encoding]::new($false, $true).GetString($buffer.ToArray())
        } finally { $inputStream.Dispose(); $buffer.Dispose() }
        $manifest = Read-WardogsManifest $json -RequireUpdater:$RequireUpdater
        if ($entries.Count -ne @($manifest.files).Count + 1) { throw 'Release ZIP has unlisted files.' }
        foreach ($file in $manifest.files) {
            if (-not $entries.ContainsKey($file.path) -or $entries[$file.path].FullName -cne $file.path -or
                    $entries[$file.path].Length -ne $file.length) { throw "Release ZIP file is missing: $($file.path)" }
            $outputStream = $null
            if ($Destination) {
                $target = Get-WardogsChildPath $Destination $file.path
                New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
                $outputStream = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            }
            $inputStream = $entries[$file.path].Open()
            try {
                if ((Get-WardogsBoundedStreamHash $inputStream $file.length $outputStream) -ine $file.sha256) {
                    throw "Release ZIP file hash mismatch: $($file.path)"
                }
            } finally { $inputStream.Dispose(); if ($outputStream) { $outputStream.Dispose() } }
        }
        if ($Destination) {
            [IO.File]::WriteAllText((Join-Path $Destination 'package-manifest.json'), $json, [Text.UTF8Encoding]::new($false))
            Assert-WardogsExecutable (Join-Path $Destination 'WarDogsDistanceCalculator.exe') $manifest.version
        }
        return $manifest
    } finally { $zip.Dispose() }
}

function Write-SmokeFile {
    param([string]$Path, [string]$Content)
    [void](Assert-WardogsPathWithoutReparse $Path)
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($Path)) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Content, [Text.UTF8Encoding]::new($false))
}

function Start-SmokeHelper {
    param([string]$ScriptPath, [string]$Mode, [int]$ParentProcessId)
    $values = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $ScriptPath, '-Mode', $Mode,
        '-InstallDirectory', $installRoot, '-WorkDirectory', $workRoot,
        '-ArchiveSha256', $archiveDigest, '-Version', $newManifest.version, '-ParentId', [string]$ParentProcessId)
    $arguments = @($values | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    return Start-Process -FilePath $powershell -ArgumentList $arguments -WorkingDirectory $workRoot `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $EvidenceDirectory ($Mode + '-stdout.txt')) `
        -RedirectStandardError (Join-Path $EvidenceDirectory ($Mode + '-stderr.txt'))
}

function Assert-SmokeProcessPath {
    param([Diagnostics.Process]$Process, [string]$ExpectedPath)
    $Process.Refresh()
    if ($Process.HasExited -or $Process.Path -ine $ExpectedPath) { throw 'The expected portable application process is not alive.' }
}

function Wait-SmokeWindow {
    param([Diagnostics.Process]$Process, [string]$ExpectedPath)
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        Assert-SmokeProcessPath $Process $ExpectedPath
        $window = [WardogsRestartSmokeWindows]::FindMainWindow([uint32]$Process.Id)
        if ($window -ne [IntPtr]::Zero -and [WardogsRestartSmokeWindows]::OwnsMainWindow($window, [uint32]$Process.Id)) {
            return $window
        }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'The portable GUI did not create its main window.' }
        Start-Sleep -Milliseconds 100
    } while ($true)
}

function Get-SmokeRuntimeModules {
    param([Diagnostics.Process]$Process)
    $Process.Refresh()
    $modules = @($Process.Modules | Where-Object { $_.ModuleName -match '^(?i:Qt6|qwindows\.dll$|onnxruntime\.dll$)' } |
        ForEach-Object { [ordered]@{ name = $_.ModuleName; path = $_.FileName } })
    foreach ($module in $modules) {
        if (-not $module.path.StartsWith($installRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Runtime dependency loaded from outside the portable package: $($module.name)"
        }
    }
    foreach ($name in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'qwindows.dll', 'onnxruntime.dll')) {
        if (@($modules | Where-Object { $_.name -ieq $name }).Count -ne 1) { throw "Expected packaged runtime module was not loaded: $name" }
    }
    return $modules
}

function Copy-SmokeSessionLog {
    param([string]$Version, [int]$ProcessId, [string]$DestinationName)
    $session = 'session.start version=' + [Regex]::Escape($Version) + ' pid=' + $ProcessId + '(?:\s|$)'
    foreach ($name in @('latest.log', 'latest.previous.log')) {
        $path = Join-Path $profileRoot ('logs\' + $name)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $text = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
        if ($text -notmatch $session) { continue }
        if ($text -notmatch ('application\.initialized version=' + [Regex]::Escape($Version) + '(?:\s|$)') -or
                $text -notmatch 'window\.ready' -or $text -notmatch 'application\.exit code=0' -or
                $text -notmatch 'session\.end') { throw "Graceful startup/shutdown log is incomplete for $Version, PID $ProcessId." }
        Write-SmokeFile (Join-Path $EvidenceDirectory $DestinationName) $text
        return
    }
    throw "The expected flushed session log was not retained: version=$Version PID=$ProcessId."
}

function Close-SmokeApplication {
    param([Diagnostics.Process]$Process, [string]$ExpectedPath, [IntPtr]$Window)
    Assert-SmokeProcessPath $Process $ExpectedPath
    # Processes obtained through Get-Process otherwise may lose access to the
    # exit code when their PID disappears. Keep the actual native handle alive.
    $processHandle = $Process.Handle
    if ($processHandle -eq [IntPtr]::Zero) { throw 'The known application process handle could not be retained.' }
    # Qt may recreate the native window after the startup probe. Only a freshly
    # enumerated and ownership-checked HWND may receive the shutdown message.
    $currentWindow = [WardogsRestartSmokeWindows]::FindMainWindow([uint32]$Process.Id)
    $attempt = [ordered]@{
        process_id = $Process.Id; startup_window = $Window.ToInt64(); close_window = $currentWindow.ToInt64()
        started_utc = [DateTime]::UtcNow.ToString('o'); request_delivered = $false; exited = $false
        process_handle_retained = $true
        close_window_class = [WardogsRestartSmokeWindows]::WindowClass($currentWindow)
        close_window_extended_style = [WardogsRestartSmokeWindows]::ExtendedStyle($currentWindow)
        window_candidates = @([WardogsRestartSmokeWindows]::WindowCandidates([uint32]$Process.Id))
    }
    $receipt.close_attempts += $attempt
    if ($currentWindow -eq [IntPtr]::Zero -or -not [WardogsRestartSmokeWindows]::OwnsMainWindow($currentWindow, [uint32]$Process.Id)) {
        throw "No current owned WARDOGS main window is available for graceful close, PID $($Process.Id)."
    }
    $attempt.request_delivered = [WardogsRestartSmokeWindows]::RequestClose($currentWindow, [uint32]$Process.Id)
    $attempt.post_message_error = [WardogsRestartSmokeWindows]::LastPostMessageError
    if (-not $attempt.request_delivered) {
        throw "WM_CLOSE could not be delivered to the verified WARDOGS window, PID $($Process.Id), Win32 error $($attempt.post_message_error)."
    }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $attempt.exited = $Process.WaitForExit(30000)
    $timer.Stop()
    $attempt.wait_milliseconds = $timer.ElapsedMilliseconds
    if (-not $attempt.exited) {
        $Process.Refresh()
        $attempt.responding_after_timeout = $Process.Responding
        throw "WARDOGS received WM_CLOSE but did not exit within 30 seconds, PID $($Process.Id)."
    }
    $exitCode = [WardogsRestartSmokeWindows]::ProcessExitCode($processHandle)
    $attempt.exit_code = $exitCode
    $attempt.exit_code_source = 'GetExitCodeProcess with retained own process handle'
    if ($null -eq $exitCode -or $exitCode -ne 0) { throw "Portable application exited with code $exitCode." }
}

function Copy-SmokeFailureLog {
    param([string]$Version, [int]$ProcessId, [string]$DestinationName)
    $session = 'session.start version=' + [Regex]::Escape($Version) + ' pid=' + $ProcessId + '(?:\s|$)'
    foreach ($name in @('latest.log', 'latest.previous.log')) {
        $path = Join-Path $profileRoot ('logs\' + $name)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $text = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
        if ($text -match $session) {
            # Failed runs retain partial logs as diagnostics, never as acceptance proof.
            Write-SmokeFile (Join-Path $EvidenceDirectory $DestinationName) $text
            return
        }
    }
}

$receiptPath = Join-Path $EvidenceDirectory 'restart-smoke.json'
$receipt = [ordered]@{
    state = 'starting'; boundary = 'Actual full portable replacement and GUI restart on an isolated runner; two-second liveness does not prove long-term health.'
    new_archive_sha256 = Get-WardogsFileHash $ArchivePath
    previous_archive_sha256 = Get-WardogsFileHash $PreviousArchivePath
    cleanup_process_ids = @()
    close_attempts = @()
}
$oldProcess = $null
$newProcess = $null
$prepareProcess = $null
$installProcess = $null
$installRoot = $null
$workRoot = $null
$profileRoot = $null
$profileSettings = $null
$profileSettingsExisted = $false
$profileSettingsPrepared = $false
$profileTerrainMarker = $null
$originalSettingsBackup = Join-Path $EvidenceDirectory 'prior-profile-settings.ini'
$savedEnvironment = @{}
foreach ($name in @('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $newManifest = Read-SmokeRelease $ArchivePath -RequireUpdater
    $oldManifest = Read-SmokeRelease $PreviousArchivePath
    $receipt.new_version = $newManifest.version
    $receipt.previous_version = $oldManifest.version
    if ([Version]$oldManifest.version -ge [Version]$newManifest.version) {
        $receipt.state = 'skipped'
        $receipt.reason = 'Latest published stable package is the same version or newer; there is no forward update to exercise.'
        Write-WardogsJson $receiptPath $receipt
        Write-Output "SKIP real restart smoke: published $($oldManifest.version) >= candidate $($newManifest.version)."
        return
    }
    $fixtureRoot = Join-Path $runnerRoot ('wardogs-update-restart-' + [Guid]::NewGuid().ToString('N'))
    if (-not $fixtureRoot.StartsWith($runnerRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $fixtureRoot)) {
        throw 'The runner fixture path is not a fresh direct child of RUNNER_TEMP.'
    }
    $installRoot = Join-Path $fixtureRoot 'Portable application'
    $workRoot = Join-Path $installRoot ('.wardogs-update-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $installRoot, $workRoot -Force | Out-Null
    $receipt.fixture_directory = $fixtureRoot
    [void](Read-SmokeRelease $PreviousArchivePath $installRoot)
    $executable = Join-Path $installRoot 'WarDogsDistanceCalculator.exe'
    $archiveDigest = $receipt.new_archive_sha256
    Copy-Item -LiteralPath $ArchivePath -Destination (Join-Path $workRoot 'download.zip')
    Copy-Item -LiteralPath $helperSource -Destination (Join-Path $workRoot 'Update.ps1')
    $sentinel = [Guid]::NewGuid().ToString('N')
    $preserved = @{
        'settings.ini' = 'portable-user-settings-' + $sentinel
        'terrain-packs/smoke-preserved.wdt' = 'portable-user-height-data-' + $sentinel
        'unknown/smoke-preserved.txt' = 'unlisted-user-file-' + $sentinel
    }
    foreach ($relative in $preserved.Keys) { Write-SmokeFile (Join-Path $installRoot $relative.Replace('/', '\')) $preserved[$relative] }

    # SHGetKnownFolderPath ignores an overridden LOCALAPPDATA environment variable.
    # Use the isolated runner's actual profile and retain its prior settings for restoration.
    $profileRoot = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)) 'WardogsFireControl'
    [void](Assert-WardogsPathWithoutReparse $profileRoot)
    $profileSettings = Join-Path $profileRoot 'settings.ini'
    $profileSettingsExisted = Test-Path -LiteralPath $profileSettings -PathType Leaf
    if ($profileSettingsExisted) { Copy-Item -LiteralPath $profileSettings -Destination $originalSettingsBackup }
    New-Item -ItemType Directory -Path $profileRoot -Force | Out-Null
    $settingsText = "[settings]`r`nquick_workflow_version=1`r`nui_language=en`r`ncheck_updates_on_start=0`r`ngame_integration_enabled=0`r`nmiddle_mouse_enabled=0`r`nmouse_capture_delay_ms=417`r`nsmoke_user_sentinel=$sentinel`r`n[ci-preserved]`r`nvalue=$sentinel`r`n"
    $profileSettingsPrepared = $true
    [IO.File]::WriteAllText($profileSettings, $settingsText, [Text.Encoding]::Unicode)
    $profileSettingsHash = Get-WardogsFileHash $profileSettings
    $profileTerrainMarker = Join-Path $profileRoot ('terrain-packs\smoke-preserved-' + $sentinel + '.wdt')
    Write-SmokeFile $profileTerrainMarker ('persistent-user-height-data-' + $sentinel)
    $profileTerrainHash = Get-WardogsFileHash $profileTerrainMarker
    Copy-Item -LiteralPath $profileSettings -Destination (Join-Path $EvidenceDirectory 'smoke-profile-settings.ini')
    $env:QT_PLUGIN_PATH = $installRoot
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $installRoot 'platforms'
    $env:QT_QPA_PLATFORM = 'windows'
    $env:PATH = (Join-Path $env:SystemRoot 'System32') + ';' + $env:SystemRoot
    $receipt.runtime_path = $env:PATH
    $receipt.qt_plugin_path = $env:QT_PLUGIN_PATH

    $oldProcess = Start-Process -FilePath $executable -WorkingDirectory $installRoot -WindowStyle Hidden -PassThru
    $receipt.old_process_id = $oldProcess.Id
    $oldWindow = Wait-SmokeWindow $oldProcess $executable
    Start-Sleep -Seconds 2
    Assert-SmokeProcessPath $oldProcess $executable
    $receipt.old_runtime_modules = @(Get-SmokeRuntimeModules $oldProcess)
    $receipt.old_main_window = $oldWindow.ToInt64()
    $receipt.old_main_window_class = [WardogsRestartSmokeWindows]::WindowClass($oldWindow)
    $receipt.old_main_window_extended_style = [WardogsRestartSmokeWindows]::ExtendedStyle($oldWindow)
    $receipt.old_window_candidates = @([WardogsRestartSmokeWindows]::WindowCandidates([uint32]$oldProcess.Id))

    $prepareProcess = Start-SmokeHelper $helperSource 'Prepare' $oldProcess.Id
    if (-not $prepareProcess.WaitForExit(90000) -or $prepareProcess.ExitCode -ne 0) { throw 'Real package preparation failed or timed out.' }
    $installStarted = [DateTime]::UtcNow
    $installProcess = Start-SmokeHelper (Join-Path $workRoot 'Update.ps1') 'Install' $oldProcess.Id
    $receipt.install_helper_id = $installProcess.Id
    $readyPath = Join-Path $workRoot 'install-ready.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    $ready = $null
    while (-not $ready) {
        if (Test-Path -LiteralPath (Join-Path $workRoot 'error.log')) { throw ([IO.File]::ReadAllText((Join-Path $workRoot 'error.log'))) }
        if ($installProcess.HasExited) { throw 'Installer exited before declaring readiness.' }
        if (Test-Path -LiteralPath $readyPath -PathType Leaf) {
            # The helper's JSON write can be observed between file creation and completion.
            # A transient sharing/parse error does not authorize shutdown; retry until a complete receipt is valid.
            try {
                $candidateReady = [IO.File]::ReadAllText($readyPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
                if ($candidateReady.state -ceq 'ready' -and $candidateReady.parent_id -eq $oldProcess.Id) {
                    $ready = $candidateReady
                }
            } catch { }
        }
        if ($ready) { break }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Installer readiness timed out.' }
        Start-Sleep -Milliseconds 100
    }
    $approvalTemporary = Join-Path $workRoot 'install-approved.tmp'
    Write-WardogsJson $approvalTemporary ([ordered]@{ state = 'approved'; parent_id = $oldProcess.Id })
    Move-Item -LiteralPath $approvalTemporary -Destination (Join-Path $workRoot 'install-approved.json')
    Close-SmokeApplication $oldProcess $executable $oldWindow
    if (-not $installProcess.WaitForExit(90000) -or $installProcess.ExitCode -ne 0) { throw 'Real installation/restart failed or timed out.' }
    $installed = [IO.File]::ReadAllText((Join-Path $workRoot 'installed.json'), [Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($installed.state -cne 'installed' -or $installed.version -cne $newManifest.version) { throw 'Successful installation receipt is missing.' }
    $installedManifest = Read-WardogsManifest ([IO.File]::ReadAllText((Join-Path $installRoot 'package-manifest.json'), [Text.Encoding]::UTF8)) $newManifest.version -RequireUpdater
    foreach ($file in $installedManifest.files) {
        $path = Get-WardogsChildPath $installRoot $file.path
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -ne $file.length -or
                (Get-WardogsFileHash $path) -ine $file.sha256) { throw "Real installed package file does not match: $($file.path)" }
    }
    Assert-WardogsExecutable $executable $newManifest.version
    $receipt.verified_package_files = @($installedManifest.files).Count
    foreach ($relative in $preserved.Keys) {
        if ([IO.File]::ReadAllText((Join-Path $installRoot $relative.Replace('/', '\'))) -cne $preserved[$relative]) {
            throw "User-owned portable file was changed: $relative"
        }
    }
    if ((Get-WardogsFileHash $profileSettings) -ine $profileSettingsHash) { throw 'Persistent profile preferences changed during update.' }
    if ((Get-WardogsFileHash $profileTerrainMarker) -ine $profileTerrainHash) { throw 'Persistent user height data changed during update.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (-not $newProcess) {
        foreach ($candidate in @(Get-Process -Name 'WarDogsDistanceCalculator' -ErrorAction SilentlyContinue)) {
            if ($candidate.Id -ne $oldProcess.Id -and $candidate.Path -ieq $executable -and
                    $candidate.StartTime.ToUniversalTime() -ge $installStarted) {
                if ($candidate.Handle -eq [IntPtr]::Zero) { throw 'The restarted application process handle could not be retained.' }
                $newProcess = $candidate
                $receipt.new_process_handle_retained = $true
                break
            }
            $candidate.Dispose()
        }
        if ($newProcess) { break }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'The installer did not restart the updated portable executable.' }
        Start-Sleep -Milliseconds 100
    }
    $newWindow = Wait-SmokeWindow $newProcess $executable
    Start-Sleep -Seconds 2
    Assert-SmokeProcessPath $newProcess $executable
    $receipt.new_process_id = $newProcess.Id
    $receipt.new_process_start_utc = $newProcess.StartTime.ToUniversalTime().ToString('o')
    $receipt.new_main_window = $newWindow.ToInt64()
    $receipt.new_main_window_class = [WardogsRestartSmokeWindows]::WindowClass($newWindow)
    $receipt.new_main_window_extended_style = [WardogsRestartSmokeWindows]::ExtendedStyle($newWindow)
    $receipt.new_window_candidates = @([WardogsRestartSmokeWindows]::WindowCandidates([uint32]$newProcess.Id))
    $receipt.new_runtime_modules = @(Get-SmokeRuntimeModules $newProcess)
    Close-SmokeApplication $newProcess $executable $newWindow
    # Both logs are now closed and rotation is complete: latest belongs to the new
    # process and latest.previous to the old process, without a read/rename race.
    Copy-SmokeSessionLog $oldManifest.version $oldProcess.Id 'old-session.log'
    Copy-SmokeSessionLog $newManifest.version $newProcess.Id 'new-session.log'
    if ((Get-WardogsFileHash $profileSettings) -ine $profileSettingsHash) { throw 'Persistent profile preferences changed after restarted GUI shutdown.' }
    if ((Get-WardogsFileHash $profileTerrainMarker) -ine $profileTerrainHash) { throw 'Persistent user height data changed after restarted GUI shutdown.' }
    $receipt.preserved_portable_files = @($preserved.Keys | Sort-Object)
    $receipt.profile_settings_preserved = $true
    $receipt.profile_height_data_preserved = $true
    $receipt.state = 'passed'
    Write-Output "PASS real portable update/restart: $($oldManifest.version) -> $($newManifest.version); $($receipt.verified_package_files) package files verified."
} catch {
    $receipt.state = 'failed'
    $receipt.error = $_.Exception.Message
    throw
} finally {
    # Cleanup is limited to process objects created by this test and exact executable paths in its unique fixture.
    if ($workRoot -and $receipt.state -eq 'failed') {
        try { Write-SmokeFile (Join-Path $workRoot 'cancel-install') 'CI acceptance aborted' } catch { }
    }
    foreach ($process in @($prepareProcess, $installProcess)) {
        if ($process) {
            try {
                if (-not $process.HasExited -and -not $process.WaitForExit(15000)) {
                    Stop-Process -Id $process.Id -Force -ErrorAction Stop
                    $receipt.cleanup_process_ids += $process.Id
                }
            } catch { $receipt.cleanup_error = $_.Exception.Message }
        }
    }
    $cleanupApps = @($oldProcess, $newProcess)
    if ($installRoot) {
        $expected = Join-Path $installRoot 'WarDogsDistanceCalculator.exe'
        foreach ($candidate in @(Get-Process -Name 'WarDogsDistanceCalculator' -ErrorAction SilentlyContinue)) {
            if ($candidate.Path -ieq $expected) { $cleanupApps += $candidate } else { $candidate.Dispose() }
        }
        foreach ($process in $cleanupApps) {
            if (-not $process) { continue }
            try {
                $process.Refresh()
                if (-not $process.HasExited -and $process.Path -ieq $expected) {
                    $window = [WardogsRestartSmokeWindows]::FindMainWindow([uint32]$process.Id)
                    if (-not [WardogsRestartSmokeWindows]::RequestClose($window, [uint32]$process.Id) -or -not $process.WaitForExit(5000)) {
                        Stop-Process -Id $process.Id -Force -ErrorAction Stop
                        $receipt.cleanup_process_ids += $process.Id
                    }
                }
            } catch { $receipt.cleanup_error = $_.Exception.Message }
        }
    }
    if ($receipt.state -eq 'failed' -and $profileRoot) {
        try {
            if ($oldProcess) { Copy-SmokeFailureLog $oldManifest.version $oldProcess.Id 'old-session-failure.log' }
            if ($newProcess) { Copy-SmokeFailureLog $newManifest.version $newProcess.Id 'new-session-failure.log' }
        } catch { $receipt.failure_log_copy_error = $_.Exception.Message }
    }
    foreach ($process in @($prepareProcess, $installProcess) + $cleanupApps) { if ($process) { $process.Dispose() } }
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
    if ($profileSettingsPrepared) {
        try {
            if ($profileSettingsExisted) { Copy-Item -LiteralPath $originalSettingsBackup -Destination $profileSettings -Force }
            elseif (Test-Path -LiteralPath $profileSettings -PathType Leaf) { Remove-Item -LiteralPath $profileSettings -Force }
        } catch { $receipt.profile_restore_error = $_.Exception.Message }
    }
    if ($profileTerrainMarker -and (Test-Path -LiteralPath $profileTerrainMarker -PathType Leaf)) {
        try { Remove-Item -LiteralPath $profileTerrainMarker -Force }
        catch { $receipt.profile_restore_error = $_.Exception.Message }
    }
    if ($workRoot -and (Test-Path -LiteralPath $workRoot -PathType Container)) {
        foreach ($name in @('prepared.json', 'install-ready.json', 'install-approved.json', 'installed.json', 'transaction.json', 'error.log')) {
            $path = Join-Path $workRoot $name
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                try { Copy-Item -LiteralPath $path -Destination (Join-Path $EvidenceDirectory $name) -Force }
                catch { $receipt.evidence_copy_error = $_.Exception.Message }
            }
        }
    }
    $receipt.completed_utc = [DateTime]::UtcNow.ToString('o')
    $cleanupFailed = $receipt.state -eq 'passed' -and
        ($receipt.Contains('cleanup_error') -or $receipt.Contains('profile_restore_error') -or $receipt.Contains('evidence_copy_error'))
    if ($cleanupFailed) { $receipt.state = 'failed'; $receipt.error = 'The update/restart passed but scoped cleanup or evidence retention failed.' }
    Write-WardogsJson $receiptPath $receipt
    if ($cleanupFailed) { throw $receipt.error }
}
