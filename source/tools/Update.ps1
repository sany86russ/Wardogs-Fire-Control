# Windows PowerShell 5.1. The application runs a private copy of this helper.
[CmdletBinding()]
param(
    [ValidateSet('Prepare', 'Install')][string]$Mode,
    [string]$InstallDirectory,
    [string]$WorkDirectory,
    [string]$ArchiveSha256,
    [string]$Version,
    [int]$ParentId = 0,
    [switch]$NoRestart
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$script:MaximumExpandedBytes = 1GB
$script:MaximumFileBytes = 256MB
$script:MaximumManifestBytes = 5MB
$script:MaximumFiles = 10000

function Write-WardogsJson {
    param([string]$Path, [object]$Value)
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}

function Assert-WardogsPathWithoutReparse {
    param([string]$Path)
    $absolute = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $current = $absolute
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Reparse points are not supported: $current"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
    return $absolute
}

function Assert-WardogsDirectoryTree {
    param([string]$Path)
    [void](Assert-WardogsPathWithoutReparse $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { throw "Directory is missing: $Path" }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($Path)
    while ($pending.Count -gt 0) {
        foreach ($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Reparse points are not supported: $($entry.FullName)"
            }
            if ($entry.PSIsContainer) { $pending.Push($entry.FullName) }
        }
    }
}

function Get-WardogsContext {
    param([string]$InstallRoot, [string]$WorkRoot, [string]$ExpectedSha256, [string]$ExpectedVersion)
    if ($InstallRoot -notmatch '^[A-Za-z]:[\\/].+' -or $WorkRoot -notmatch '^[A-Za-z]:[\\/].+') {
        throw 'Installation and work directories must be absolute local Windows paths.'
    }
    $install = Assert-WardogsPathWithoutReparse $InstallRoot
    $work = Assert-WardogsPathWithoutReparse $WorkRoot
    if ([IO.Path]::GetDirectoryName($work) -ine $install -or
            [IO.Path]::GetFileName($work) -notmatch '^\.wardogs-update-(?:[a-fA-F0-9]{32}|[A-Za-z0-9]{6})$') {
        throw 'The work directory must be a unique .wardogs-update-<GUID> direct child of the installation.'
    }
    if ($ExpectedSha256 -notmatch '^[a-fA-F0-9]{64}$' -or $ExpectedVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
        throw 'A SHA-256 digest and three-part release version are required.'
    }
    Assert-WardogsDirectoryTree $install
    Assert-WardogsDirectoryTree $work
    return [pscustomobject]@{
        Install = $install
        Work = $work
        Archive = Join-Path $work 'download.zip'
        Stage = Join-Path $work 'staged'
        Backup = Join-Path $work 'backup'
        Sha256 = $ExpectedSha256.ToLowerInvariant()
        Version = $ExpectedVersion
    }
}

function Assert-WardogsPackagePath {
    param([string]$Path)
    if (-not $Path -or $Path.Length -gt 220 -or $Path -match '[<>:"\\|?*\x00-\x1f]' -or
            $Path.StartsWith('/') -or $Path.EndsWith('/') -or $Path.Contains('//')) {
        throw "Invalid package path: $Path"
    }
    foreach ($component in $Path.Split('/')) {
        if ($component -in @('.', '..') -or $component -match '[ .]$' -or
                $component -match ('^(?i:CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[1-9' + [char]0x00b9 +
                    [char]0x00b2 + [char]0x00b3 + ']|LPT[1-9' + [char]0x00b9 + [char]0x00b2 + [char]0x00b3 + '])(?:\.|$)') -or
                $component -ieq '.git' -or
                $component -match '^\.wardogs-update-' -or $component -ieq 'settings.ini' -or
                $component -match '(?i)\.wdt$') {
            throw "Protected or invalid package path: $Path"
        }
    }
    return $Path
}

function Get-WardogsChildPath {
    param([string]$Root, [string]$Relative)
    [void](Assert-WardogsPackagePath $Relative)
    $full = [IO.Path]::GetFullPath((Join-Path $Root $Relative.Replace('/', '\')))
    if (-not $full.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package path escapes its root: $Relative"
    }
    [void](Assert-WardogsPathWithoutReparse $full)
    return $full
}

function Read-WardogsManifest {
    param([string]$Json, [string]$ExpectedVersion, [switch]$RequireUpdater)
    try { $manifest = $Json | ConvertFrom-Json } catch { throw 'Package manifest is not valid JSON.' }
    if (-not $manifest -or -not $manifest.PSObject.Properties['product'] -or
            -not $manifest.PSObject.Properties['version'] -or
            -not $manifest.PSObject.Properties['platform'] -or
            -not $manifest.PSObject.Properties['configuration'] -or
            -not $manifest.PSObject.Properties['files']) { throw 'Package manifest is incomplete.' }
    $terrainProperty = $manifest.PSObject.Properties['terrain_data_included']
    $terrainInvalid = if ($terrainProperty) {
        $terrainProperty.Value -isnot [bool] -or $terrainProperty.Value -ne $false
    } else { [bool]$RequireUpdater }
    if ($manifest.product -cne 'WARDOGS Fire Control' -or $manifest.platform -cne 'Windows x64' -or
            $manifest.configuration -cne 'Release' -or $terrainInvalid -or $manifest.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -or
            ($ExpectedVersion -and $manifest.version -cne $ExpectedVersion)) {
        throw 'Package product, platform, configuration or release version does not match.'
    }
    $files = @($manifest.files)
    if ($files.Count -lt 1 -or $files.Count -gt $script:MaximumFiles) { throw 'Package manifest file count is invalid.' }
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    [long]$total = 0
    foreach ($file in $files) {
        if (-not $file -or -not $file.PSObject.Properties['path'] -or
                -not $file.PSObject.Properties['length'] -or -not $file.PSObject.Properties['sha256'] -or
                $file.path -isnot [string] -or $file.sha256 -isnot [string] -or
                $file.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or
                ($file.length -isnot [int] -and $file.length -isnot [long]) -or
                $file.length -lt 0 -or $file.length -gt $script:MaximumFileBytes) {
            throw 'Package manifest contains an invalid file record.'
        }
        [void](Assert-WardogsPackagePath $file.path)
        if ($file.path -ieq 'package-manifest.json' -or -not $paths.Add($file.path)) {
            throw "Package manifest contains a duplicate or reserved file: $($file.path)"
        }
        $total += $file.length
        if ($total -gt $script:MaximumExpandedBytes) { throw 'Package exceeds the expanded-size limit.' }
    }
    foreach ($file in $files) {
        $parent = $file.path
        while ($parent.Contains('/')) {
            $parent = $parent.Substring(0, $parent.LastIndexOf('/'))
            if ($paths.Contains($parent)) { throw "A package file is also used as a directory: $parent" }
        }
    }
    $required = @('WarDogsDistanceCalculator.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll',
        'onnxruntime.dll', 'platforms/qwindows.dll', 'models/PP-OCRv6_rec_small.onnx')
    if ($RequireUpdater) { $required += @('Update.ps1', 'Qt6Network.dll', 'tls/qschannelbackend.dll') }
    foreach ($name in $required) {
        if (-not $paths.Contains($name)) { throw "Required package file is missing: $name" }
    }
    return $manifest
}

function Get-WardogsBoundedStreamHash {
    param([IO.Stream]$Stream, [long]$ExpectedLength, [IO.Stream]$Destination)
    $sha = [Security.Cryptography.SHA256]::Create()
    $buffer = New-Object byte[] 1048576
    [long]$read = 0
    try {
        while (($count = $Stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $read += $count
            if ($read -gt $ExpectedLength -or $read -gt $script:MaximumFileBytes) {
                throw 'ZIP entry exceeds its declared length or the allowed size.'
            }
            [void]$sha.TransformBlock($buffer, 0, $count, $buffer, 0)
            if ($Destination) { $Destination.Write($buffer, 0, $count) }
        }
        if ($read -ne $ExpectedLength) { throw 'ZIP entry has an unexpected length.' }
        [void]$sha.TransformFinalBlock($buffer, 0, 0)
        return [BitConverter]::ToString($sha.Hash).Replace('-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}

function Assert-WardogsExecutable {
    param([string]$Path, [string]$ExpectedVersion)
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 256 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Application is not a Windows executable.' }
        $stream.Position = 0x3c
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length - 26) { throw 'Application PE header is invalid.' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw 'Application is not a Windows x64 executable.'
        }
        $stream.Position = $offset + 24
        if ($reader.ReadUInt16() -ne 0x20b) { throw 'Application is not a PE32+ executable.' }
    } finally { $reader.Dispose(); $stream.Dispose() }
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    if ($info.ProductName -cne 'WARDOGS Fire Control' -or $info.FileVersion -cne $ExpectedVersion -or
            $info.ProductVersion -cne $ExpectedVersion) { throw 'Application product or binary version does not match the release.' }
}

function Open-WardogsVerifiedArchive {
    param([object]$Context)
    [void](Assert-WardogsPathWithoutReparse $Context.Archive)
    if (-not (Test-Path -LiteralPath $Context.Archive -PathType Leaf) -or
            (Get-Item -LiteralPath $Context.Archive).Length -gt $script:MaximumExpandedBytes) {
        throw 'Downloaded archive is missing or exceeds the size limit.'
    }
    # Hash and read the same locked file handle, preventing an archive swap between those operations.
    $archiveStream = [IO.File]::Open($Context.Archive, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $actualHash = [BitConverter]::ToString($sha.ComputeHash($archiveStream)).Replace('-', '').ToLowerInvariant()
        if ($actualHash -ine $Context.Sha256) { throw 'Downloaded archive SHA-256 does not match the GitHub release.' }
        $archiveStream.Position = 0
        $zip = [IO.Compression.ZipArchive]::new($archiveStream, [IO.Compression.ZipArchiveMode]::Read, $false)
    } catch { $archiveStream.Dispose(); throw }
    finally { $sha.Dispose() }
    try {
        if ($zip.Entries.Count -lt 2 -or $zip.Entries.Count -gt $script:MaximumFiles + 1) {
            throw 'Downloaded archive file count is invalid.'
        }
        $entries = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
        [long]$expanded = 0
        foreach ($entry in $zip.Entries) {
            [void](Assert-WardogsPackagePath $entry.FullName)
            $attributes = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$entry.ExternalAttributes), 0)
            $unixType = ($attributes -shr 16) -band 0xf000
            if (($unixType -ne 0 -and $unixType -ne 0x8000) -or ($attributes -band 0x410) -ne 0 -or
                    $entry.Length -gt $script:MaximumFileBytes -or $entry.Length -lt 0 -or
                    $entries.ContainsKey($entry.FullName)) { throw "Invalid or duplicate ZIP entry: $($entry.FullName)" }
            $entries.Add($entry.FullName, $entry)
            $expanded += $entry.Length
            if ($expanded -gt $script:MaximumExpandedBytes) { throw 'ZIP exceeds the expanded-size limit.' }
        }
        if (-not $entries.ContainsKey('package-manifest.json')) { throw 'Downloaded archive has no package manifest.' }
        $manifestEntry = $entries['package-manifest.json']
        if ($manifestEntry.Length -gt $script:MaximumManifestBytes) { throw 'Package manifest exceeds the size limit.' }
        $manifestStream = $manifestEntry.Open()
        $manifestBuffer = [IO.MemoryStream]::new()
        try {
            [void](Get-WardogsBoundedStreamHash $manifestStream $manifestEntry.Length $manifestBuffer)
            $json = [Text.UTF8Encoding]::new($false, $true).GetString($manifestBuffer.ToArray())
        } finally { $manifestStream.Dispose(); $manifestBuffer.Dispose() }
        $manifest = Read-WardogsManifest $json $Context.Version -RequireUpdater
        if ($entries.Count -ne @($manifest.files).Count + 1) { throw 'ZIP includes files not listed in its package manifest.' }
        foreach ($file in $manifest.files) {
            if (-not $entries.ContainsKey($file.path) -or $entries[$file.path].FullName -cne $file.path -or
                    $entries[$file.path].Length -ne $file.length) { throw "ZIP file is missing or mismatched: $($file.path)" }
            $inputStream = $entries[$file.path].Open()
            try {
                if ((Get-WardogsBoundedStreamHash $inputStream $file.length) -ine $file.sha256) {
                    throw "ZIP file SHA-256 mismatch: $($file.path)"
                }
            } finally { $inputStream.Dispose() }
        }
        return [pscustomobject]@{ Zip = $zip; Entries = $entries; Manifest = $manifest; ManifestJson = $json }
    } catch { $zip.Dispose(); throw }
}

function Assert-WardogsStagedPackage {
    param([object]$Context, [object]$Verified)
    Assert-WardogsDirectoryTree $Context.Stage
    $allowed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    [void]$allowed.Add('package-manifest.json')
    foreach ($file in $Verified.Manifest.files) {
        $path = Get-WardogsChildPath $Context.Stage $file.path
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
                (Get-Item -LiteralPath $path).Length -ne $file.length -or
                (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $file.sha256) {
            throw "Prepared file is missing or has changed: $($file.path)"
        }
        [void]$allowed.Add($file.path)
    }
    foreach ($file in Get-ChildItem -LiteralPath $Context.Stage -File -Force -Recurse) {
        $relative = $file.FullName.Substring($Context.Stage.Length + 1).Replace('\', '/')
        if (-not $allowed.Contains($relative)) { throw "Unexpected prepared file: $relative" }
    }
    $manifestPath = Join-Path $Context.Stage 'package-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
            [IO.File]::ReadAllText($manifestPath, [Text.Encoding]::UTF8) -cne $Verified.ManifestJson) {
        throw 'Prepared package manifest has changed.'
    }
    Assert-WardogsExecutable (Join-Path $Context.Stage 'WarDogsDistanceCalculator.exe') $Context.Version
}

function Invoke-WardogsPrepare {
    param([object]$Context)
    if (Test-Path -LiteralPath $Context.Stage) { throw 'A fresh work directory is required for preparation.' }
    $verified = Open-WardogsVerifiedArchive $Context
    try {
        New-Item -ItemType Directory -Path $Context.Stage | Out-Null
        foreach ($file in $verified.Manifest.files) {
            $path = Get-WardogsChildPath $Context.Stage $file.path
            New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($path)) -Force | Out-Null
            $inputStream = $verified.Entries[$file.path].Open()
            $outputStream = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try {
                if ((Get-WardogsBoundedStreamHash $inputStream $file.length $outputStream) -ine $file.sha256) {
                    throw "Extracted file SHA-256 mismatch: $($file.path)"
                }
            } finally { $inputStream.Dispose(); $outputStream.Dispose() }
        }
        [IO.File]::WriteAllText((Join-Path $Context.Stage 'package-manifest.json'), $verified.ManifestJson, [Text.UTF8Encoding]::new($false))
        Assert-WardogsStagedPackage $Context $verified
        Write-WardogsJson (Join-Path $Context.Work 'prepared.json') ([ordered]@{
            state = 'prepared'; version = $Context.Version; archive_sha256 = $Context.Sha256
            install_directory = $Context.Install; file_count = @($verified.Manifest.files).Count
        })
    } finally { $verified.Zip.Dispose() }
}

function Get-WardogsInstallPlan {
    param([object]$Context, [object]$Manifest)
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $plan = [Collections.Generic.List[object]]::new()
    foreach ($file in @($Manifest.files | Sort-Object path)) {
        $target = Get-WardogsChildPath $Context.Install $file.path
        if (Test-Path -LiteralPath $target -PathType Container) { throw "A directory blocks a package file: $($file.path)" }
        $ancestor = [IO.Path]::GetDirectoryName($target)
        while ($ancestor -ine $Context.Install) {
            if (Test-Path -LiteralPath $ancestor -PathType Leaf) { throw "A file blocks a package directory: $ancestor" }
            $ancestor = [IO.Path]::GetDirectoryName($ancestor)
        }
        [void]$paths.Add($file.path)
        $plan.Add([pscustomobject]@{ path = $file.path; action = 'replace'; existed = (Test-Path -LiteralPath $target -PathType Leaf) })
    }
    $oldManifestPath = Get-WardogsChildPath $Context.Install 'package-manifest.json'
    if (Test-Path -LiteralPath $oldManifestPath -PathType Container) { throw 'A directory blocks the installed manifest.' }
    if (-not (Test-Path -LiteralPath $oldManifestPath -PathType Leaf)) {
        throw 'This installation has no package manifest. Install a complete portable GitHub package first.'
    }
    if (Test-Path -LiteralPath $oldManifestPath -PathType Leaf) {
        if ((Get-Item -LiteralPath $oldManifestPath).Length -gt $script:MaximumManifestBytes) { throw 'Installed manifest exceeds the size limit.' }
        $old = Read-WardogsManifest ([IO.File]::ReadAllText($oldManifestPath, [Text.Encoding]::UTF8))
        if ([Version]$old.version -ge [Version]$Context.Version) { throw 'The update must be newer than the installed package.' }
        foreach ($file in $old.files) {
            if ($paths.Contains($file.path)) { continue }
            $target = Get-WardogsChildPath $Context.Install $file.path
            # Only remove obsolete files that still match the old package. User replacements remain untouched.
            if ((Test-Path -LiteralPath $target -PathType Leaf) -and
                    (Get-Item -LiteralPath $target).Length -eq $file.length -and
                    (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ieq $file.sha256) {
                $plan.Add([pscustomobject]@{ path = $file.path; action = 'remove'; existed = $true })
            }
        }
    }
    $plan.Add([pscustomobject]@{ path = 'package-manifest.json'; action = 'replace'; existed = (Test-Path -LiteralPath $oldManifestPath -PathType Leaf) })
    return $plan.ToArray()
}

function Start-WardogsApplication {
    param([object]$Context)
    Start-Process -FilePath (Join-Path $Context.Install 'WarDogsDistanceCalculator.exe') -WorkingDirectory $Context.Install -ErrorAction Stop | Out-Null
}

function Invoke-WardogsInstall {
    param([object]$Context, [int]$ParentProcessId, [switch]$SkipRestart)
    if (-not $SkipRestart -and $ParentProcessId -le 0) { throw 'A positive parent process ID is required.' }
    if (Test-Path -LiteralPath $Context.Backup) { throw 'Installation was already attempted in this work directory.' }
    $receiptPath = Join-Path $Context.Work 'prepared.json'
    if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf) -or (Get-Item -LiteralPath $receiptPath).Length -gt 16KB) {
        throw 'The update has not been prepared.'
    }
    $receipt = [IO.File]::ReadAllText($receiptPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($receipt.state -cne 'prepared' -or $receipt.version -cne $Context.Version -or
            $receipt.archive_sha256 -ine $Context.Sha256 -or $receipt.install_directory -ine $Context.Install) {
        throw 'Preparation receipt does not match this installation.'
    }
    $verified = Open-WardogsVerifiedArchive $Context
    $parentExited = $false
    $restartHandled = $false
    try {
        Assert-WardogsStagedPackage $Context $verified
        [void](Get-WardogsInstallPlan $Context $verified.Manifest)
        $cancelPath = Join-Path $Context.Work 'cancel-install'
        if (Test-Path -LiteralPath $cancelPath) { throw 'Installation was cancelled before shutdown.' }
        Write-WardogsJson (Join-Path $Context.Work 'install-ready.json') ([ordered]@{ state = 'ready'; parent_id = $ParentProcessId })
        if ($ParentProcessId -gt 0) {
            $parent = $null
            try { $parent = [Diagnostics.Process]::GetProcessById($ParentProcessId) } catch [ArgumentException] { }
            if ($parent) {
                try {
                    $deadline = [DateTime]::UtcNow.AddSeconds(60)
                    while (-not $parent.WaitForExit(200)) {
                        if (Test-Path -LiteralPath $cancelPath) { throw 'Installation was cancelled before shutdown.' }
                        if ([DateTime]::UtcNow -ge $deadline) { throw 'The application did not exit within 60 seconds.' }
                    }
                } finally { $parent.Dispose() }
            }
        }
        if (Test-Path -LiteralPath $cancelPath) { throw 'Installation was cancelled before file replacement.' }
        if ($ParentProcessId -gt 0) {
            $approvalPath = Join-Path $Context.Work 'install-approved.json'
            [void](Assert-WardogsPathWithoutReparse $approvalPath)
            if (-not (Test-Path -LiteralPath $approvalPath -PathType Leaf) -or
                    (Get-Item -LiteralPath $approvalPath).Length -gt 16KB) { throw 'Installation shutdown was not approved.' }
            $approval = [IO.File]::ReadAllText($approvalPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
            if (-not $approval.PSObject.Properties['state'] -or -not $approval.PSObject.Properties['parent_id'] -or
                    $approval.state -cne 'approved' -or $approval.parent_id -ne $ParentProcessId) {
                throw 'Installation shutdown approval does not match the parent.'
            }
        }
        $parentExited = $true
        Assert-WardogsDirectoryTree $Context.Install
        Assert-WardogsStagedPackage $Context $verified
        $plan = @(Get-WardogsInstallPlan $Context $verified.Manifest)
        New-Item -ItemType Directory -Path $Context.Backup | Out-Null
        foreach ($entry in $plan) {
            if ($entry.existed) {
                $target = Get-WardogsChildPath $Context.Install $entry.path
                $backupPath = Get-WardogsChildPath $Context.Backup $entry.path
                New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($backupPath)) -Force | Out-Null
                Copy-Item -LiteralPath $target -Destination $backupPath -ErrorAction Stop
                if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ine
                        (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash) { throw "Backup verification failed: $($entry.path)" }
            }
        }
        $journal = [ordered]@{ state = 'installing'; version = $Context.Version; operations = $plan; applied = @() }
        $journalPath = Join-Path $Context.Work 'transaction.json'
        Write-WardogsJson $journalPath $journal
        $applied = [Collections.Generic.List[object]]::new()
        try {
            foreach ($entry in $plan) {
                $target = Get-WardogsChildPath $Context.Install $entry.path
                New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
                # Record intent before the first write so a partially failed copy is also restored.
                $applied.Add($entry)
                $journal.applied = @($applied | ForEach-Object { $_.path })
                Write-WardogsJson $journalPath $journal
                if ($entry.action -eq 'remove') {
                    Remove-Item -LiteralPath $target -Force -ErrorAction Stop
                } else {
                    Copy-Item -LiteralPath (Get-WardogsChildPath $Context.Stage $entry.path) -Destination $target -Force -ErrorAction Stop
                }
            }
            foreach ($file in $verified.Manifest.files) {
                $target = Get-WardogsChildPath $Context.Install $file.path
                if ((Get-Item -LiteralPath $target).Length -ne $file.length -or
                        (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ine $file.sha256) { throw "Installed file verification failed: $($file.path)" }
            }
            Assert-WardogsExecutable (Join-Path $Context.Install 'WarDogsDistanceCalculator.exe') $Context.Version
            $journal.state = 'installed'
            Write-WardogsJson $journalPath $journal
            Write-WardogsJson (Join-Path $Context.Work 'installed.json') ([ordered]@{ state = 'installed'; version = $Context.Version })
            # Launch is the final fallible action: never roll files back underneath a running new executable.
            if (-not $SkipRestart) { Start-WardogsApplication $Context }
        } catch {
            # This catch owns all recovery from the first possible write onward.
            # Set this before recovery/logging can fail, so the outer catch never starts a mixed installation.
            $restartHandled = $true
            $installFailure = $_.Exception.Message
            $rollbackFailures = [Collections.Generic.List[string]]::new()
            for ($index = $applied.Count - 1; $index -ge 0; --$index) {
                $entry = $applied[$index]
                try {
                    $target = Get-WardogsChildPath $Context.Install $entry.path
                    if ($entry.existed) {
                        $backupPath = Get-WardogsChildPath $Context.Backup $entry.path
                        # A locked destination can reject the write without changing it.
                        # Avoid trying to overwrite that already-intact original during rollback.
                        if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or
                                (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ine
                                (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash) {
                            Copy-Item -LiteralPath $backupPath -Destination $target -Force -ErrorAction Stop
                        }
                        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ine
                                (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash) { throw 'Restored backup hash does not match.' }
                    } elseif (Test-Path -LiteralPath $target -PathType Leaf) {
                        Remove-Item -LiteralPath $target -Force -ErrorAction Stop
                    }
                } catch { $rollbackFailures.Add("$($entry.path): $($_.Exception.Message)") }
            }
            $journal.state = if ($rollbackFailures.Count -eq 0) { 'rolled-back' } else { 'rollback-incomplete' }
            $journal.error = $installFailure
            $journal.rollback_errors = @($rollbackFailures)
            $journalFailure = ''
            try {
                Write-WardogsJson $journalPath $journal
                if (Test-Path -LiteralPath (Join-Path $Context.Work 'installed.json')) {
                    Write-WardogsJson (Join-Path $Context.Work 'installed.json') ([ordered]@{ state = $journal.state; version = $Context.Version })
                }
            } catch { $journalFailure = ' Recovery receipt could not be written: ' + $_.Exception.Message }
            if ($rollbackFailures.Count -eq 0 -and -not $SkipRestart) {
                $restartHandled = $true
                try { Start-WardogsApplication $Context } catch { $rollbackFailures.Add("Restart: $($_.Exception.Message)") }
            }
            # Incomplete rollback requires manual recovery, rather than launching a mixed installation.
            if ($rollbackFailures.Count -gt 0) { $restartHandled = $true }
            $suffix = if ($rollbackFailures.Count -gt 0) { ' Rollback errors: ' + ($rollbackFailures -join '; ') } else { ' Previous files restored.' }
            throw ($installFailure + $suffix + $journalFailure)
        }
    } catch {
        if ($parentExited -and -not $SkipRestart -and -not $restartHandled) {
            $originalFailure = $_.Exception.Message
            try { Start-WardogsApplication $Context }
            catch { throw ($originalFailure + ' Original application restart failed: ' + $_.Exception.Message) }
        }
        throw
    } finally { $verified.Zip.Dispose() }
}

if ($MyInvocation.InvocationName -ne '.') {
    $context = $null
    try {
        if (-not $Mode) { throw 'Mode must be Prepare or Install.' }
        $context = Get-WardogsContext $InstallDirectory $WorkDirectory $ArchiveSha256 $Version
        if ($Mode -eq 'Prepare') { Invoke-WardogsPrepare $context }
        else { Invoke-WardogsInstall $context $ParentId -SkipRestart:$NoRestart }
        exit 0
    } catch {
        # Write diagnostics only after the work path itself has passed validation.
        if ($context) {
            try {
                [void](Assert-WardogsPathWithoutReparse $context.Work)
                [IO.File]::WriteAllText((Join-Path $context.Work 'error.log'), $_.Exception.ToString(), [Text.UTF8Encoding]::new($false))
            } catch { }
        }
        Write-Error $_
        exit 1
    }
}
