[CmdletBinding()]
param([string]$ExecutablePath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$helper = Join-Path $sourceRoot 'tools\Update.ps1'
if (-not $ExecutablePath) { $ExecutablePath = Join-Path $sourceRoot 'out\package\WarDogsDistanceCalculator.exe' }
$ExecutablePath = [IO.Path]::GetFullPath($ExecutablePath)
if (-not (Test-Path -LiteralPath $ExecutablePath -PathType Leaf)) { throw 'Pass the built EXE using -ExecutablePath.' }
$testVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($ExecutablePath).FileVersion
. $helper
Assert-WardogsExecutable $ExecutablePath $testVersion
$testRoot = Join-Path $sourceRoot ('out\update-installer-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$script:Passed = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Fails {
    param([scriptblock]$Action, [string]$Expected)
    $failed = $false
    try { & $Action } catch {
        $failed = $true
        if ($Expected -and $_.Exception.Message -notmatch [Regex]::Escape($Expected)) {
            throw "Unexpected error, expected '$Expected': $($_.Exception.Message)"
        }
    }
    if (-not $failed) { throw 'Operation unexpectedly succeeded.' }
}

function Write-TestFile {
    param([string]$Root, [string]$Relative, [string]$Value)
    $path = Join-Path $Root $Relative.Replace('/', '\')
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($path)) -Force | Out-Null
    [IO.File]::WriteAllText($path, $Value, [Text.UTF8Encoding]::new($false))
}

function New-TestManifest {
    param([string]$Root, [switch]$Legacy)
    $files = @(Get-ChildItem -LiteralPath $Root -File -Recurse | Where-Object { $_.Name -ne 'package-manifest.json' } |
        Sort-Object FullName | ForEach-Object {
            [ordered]@{
                path = $_.FullName.Substring($Root.Length + 1).Replace('\', '/')
                length = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        })
    $manifest = [ordered]@{
        product = 'WARDOGS Fire Control'; platform = 'Windows x64'; version = $(if ($Legacy) { '0.0.0' } else { $testVersion })
        configuration = 'Release'; files = $files
    }
    if (-not $Legacy) { $manifest.terrain_data_included = $false }
    Write-WardogsJson (Join-Path $Root 'package-manifest.json') $manifest
}

function New-TestFixture {
    param([string]$Name)
    $root = Join-Path $testRoot $Name
    $install = Join-Path $root 'Application folder'
    $candidate = Join-Path $root 'candidate'
    $work = Join-Path $install ('.wardogs-update-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $install, $candidate, $work -Force | Out-Null
    $required = @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll', 'onnxruntime.dll',
        'platforms/qwindows.dll', 'tls/qschannelbackend.dll', 'models/PP-OCRv6_rec_small.onnx',
        'a-first.dll', 'z-locked.dll', 'terrain-packs/TERRAIN_DATA_NOTICE.md')
    foreach ($directory in @($candidate, $install)) {
        Copy-Item -LiteralPath $ExecutablePath -Destination (Join-Path $directory 'WarDogsDistanceCalculator.exe')
        Copy-Item -LiteralPath $helper -Destination (Join-Path $directory 'Update.ps1')
        foreach ($name in $required) {
            Write-TestFile $directory $name $(if ($directory -eq $candidate) { "new: $name" } else { "old: $name" })
        }
    }
    Write-TestFile $install 'obsolete.dll' 'obsolete original'
    Write-TestFile $install 'obsolete-user.dll' 'original package file'
    New-TestManifest $install -Legacy
    Write-TestFile $install 'obsolete-user.dll' 'user modified library'
    Write-TestFile $install 'settings.ini' 'user preferences'
    Write-TestFile $install 'terrain-packs/local.wdt' 'user height data'
    Write-TestFile $install 'unknown/custom.txt' 'unlisted user file'
    Write-TestFile $candidate 'new-only.dll' 'new dependency'
    New-TestManifest $candidate
    return [pscustomobject]@{ Root = $root; Install = $install; Work = $work; Candidate = $candidate; Context = $null }
}

function Complete-TestArchive {
    param([object]$Fixture)
    $archive = Join-Path $Fixture.Work 'download.zip'
    # Windows .NET Framework CreateFromDirectory uses backslash entry names;
    # released ZIPs use canonical slash names from the current CI runtime.
    $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $Fixture.Candidate -File -Recurse) {
            $relative = $file.FullName.Substring($Fixture.Candidate.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $relative, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $zip.Dispose() }
    $Fixture.Context = Get-WardogsContext $Fixture.Install $Fixture.Work (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash $testVersion
}

function Add-TestZipEntry {
    param([object]$Fixture, [string]$Name, [int]$Attributes = 0)
    $zip = [IO.Compression.ZipFile]::Open($Fixture.Context.Archive, [IO.Compression.ZipArchiveMode]::Update)
    try {
        $entry = $zip.CreateEntry($Name)
        $entry.ExternalAttributes = $Attributes
        $stream = $entry.Open()
        try { $bytes = [Text.Encoding]::UTF8.GetBytes('unexpected'); $stream.Write($bytes, 0, $bytes.Length) }
        finally { $stream.Dispose() }
    } finally { $zip.Dispose() }
    $Fixture.Context.Sha256 = (Get-FileHash -LiteralPath $Fixture.Context.Archive -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-Test {
    param([string]$Name, [scriptblock]$Action)
    & $Action
    $script:Passed++
    Write-Output "PASS $Name"
}

function Start-TestHelper {
    param([object]$Fixture, [string]$HelperMode, [int]$TestParentId)
    $values = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $helper, '-Mode', $HelperMode,
        '-InstallDirectory', $Fixture.Install, '-WorkDirectory', $Fixture.Work,
        '-ArchiveSha256', $Fixture.Context.Sha256, '-Version', $testVersion,
        '-ParentId', [string]$TestParentId, '-NoRestart')
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $info.Arguments = (@($values | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start()) { throw 'Test helper could not be started.' }
    return $process
}

Invoke-Test 'prepare and full install preserve preferences, terrain, custom files and replacements' {
    $fixture = New-TestFixture 'success'
    Complete-TestArchive $fixture
    Invoke-WardogsPrepare $fixture.Context
    Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'a-first.dll')) -eq 'old: a-first.dll') 'Prepare changed the installed application.'
    Invoke-WardogsInstall $fixture.Context 0 -SkipRestart
    Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'a-first.dll')) -eq 'new: a-first.dll') 'DLL was not updated.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'settings.ini')) -eq 'user preferences') 'Settings were changed.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'terrain-packs/local.wdt')) -eq 'user height data') 'Height data changed.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'unknown/custom.txt')) -eq 'unlisted user file') 'Custom files changed.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'obsolete-user.dll')) -eq 'user modified library') 'Modified obsolete library was deleted.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $fixture.Install 'obsolete.dll'))) 'Original obsolete library was not removed.'
    Assert-True (Test-Path -LiteralPath (Join-Path $fixture.Install 'new-only.dll') -PathType Leaf) 'New library was not installed.'
    Assert-True (Test-Path -LiteralPath (Join-Path $fixture.Work 'backup/obsolete.dll') -PathType Leaf) 'Old files were not retained for recovery.'
    $journal = [IO.File]::ReadAllText((Join-Path $fixture.Work 'transaction.json')) | ConvertFrom-Json
    Assert-True ($journal.state -eq 'installed') 'Successful transaction receipt is missing.'
}

Invoke-Test 'locked destination restores previously changed files and original manifest' {
    $fixture = New-TestFixture 'rollback'
    Complete-TestArchive $fixture
    Invoke-WardogsPrepare $fixture.Context
    $oldManifest = [IO.File]::ReadAllText((Join-Path $fixture.Install 'package-manifest.json'))
    $locked = [IO.File]::Open((Join-Path $fixture.Install 'z-locked.dll'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { Assert-Fails { Invoke-WardogsInstall $fixture.Context 0 -SkipRestart } 'Previous files restored.' }
    finally { $locked.Dispose() }
    Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'a-first.dll')) -eq 'old: a-first.dll') 'A previously updated DLL was not restored.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'package-manifest.json')) -ceq $oldManifest) 'Old manifest was not restored.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $fixture.Install 'new-only.dll'))) 'A new file was not removed on rollback.'
    Assert-True (Test-Path -LiteralPath (Join-Path $fixture.Install 'obsolete.dll')) 'Obsolete file was not restored.'
    $journal = [IO.File]::ReadAllText((Join-Path $fixture.Work 'transaction.json')) | ConvertFrom-Json
    Assert-True ($journal.state -eq 'rolled-back') 'Rollback was not recorded.'
}

Invoke-Test 'archive digest mismatch fails before extraction' {
    $fixture = New-TestFixture 'digest'
    Complete-TestArchive $fixture
    $fixture.Context.Sha256 = '0' * 64
    Assert-Fails { Invoke-WardogsPrepare $fixture.Context } 'SHA-256'
    Assert-True (-not (Test-Path -LiteralPath $fixture.Context.Stage)) 'Untrusted archive was extracted.'
}

Invoke-Test 'missing manifest fails before extraction' {
    $fixture = New-TestFixture 'missing-manifest'
    Remove-Item -LiteralPath (Join-Path $fixture.Candidate 'package-manifest.json')
    Complete-TestArchive $fixture
    Assert-Fails { Invoke-WardogsPrepare $fixture.Context } 'no package manifest'
}

Invoke-Test 'wrong release version fails before extraction' {
    $fixture = New-TestFixture 'wrong-version'
    Complete-TestArchive $fixture
    $fixture.Context.Version = '999.0.0'
    Assert-Fails { Invoke-WardogsPrepare $fixture.Context } 'release version'
}

Invoke-Test 'manifest hash mismatch fails before extraction' {
    $fixture = New-TestFixture 'file-hash'
    Write-TestFile $fixture.Candidate 'a-first.dll' 'tampered dependency'
    Complete-TestArchive $fixture
    Assert-Fails { Invoke-WardogsPrepare $fixture.Context } 'mismatched'
}

foreach ($invalidPath in @('../outside.txt', '/absolute.txt', 'C:/outside.txt', 'folder/back\slash.txt',
        'CON.txt', 'CONIN$', 'CONOUT$', ('COM' + [char]0x00b9 + '.txt'), ('LPT' + [char]0x00b2),
        'folder/name. ', 'settings.ini', 'terrain-packs/map.wdt')) {
    $capturedPath = $invalidPath
    Invoke-Test ("invalid ZIP path $capturedPath") {
        $fixture = New-TestFixture ('bad-path-' + $script:Passed)
        Complete-TestArchive $fixture
        Add-TestZipEntry $fixture $capturedPath
        Assert-Fails { Invoke-WardogsPrepare $fixture.Context } 'package path'
        Assert-True (-not (Test-Path -LiteralPath $fixture.Context.Stage)) 'Unsafe archive was extracted.'
    }
}

Invoke-Test 'case-insensitive duplicate ZIP entry is rejected' {
    $fixture = New-TestFixture 'duplicate'
    Complete-TestArchive $fixture
    Add-TestZipEntry $fixture 'A-FIRST.DLL'
    Assert-Fails { Invoke-WardogsPrepare $fixture.Context } 'duplicate ZIP entry'
}

Invoke-Test 'unlisted ZIP file is rejected' {
    $fixture = New-TestFixture 'unlisted'
    Complete-TestArchive $fixture
    Add-TestZipEntry $fixture 'unexpected.txt'
    Assert-Fails { Invoke-WardogsPrepare $fixture.Context } 'not listed'
}

Invoke-Test 'ZIP symbolic link attributes are rejected' {
    $fixture = New-TestFixture 'zip-link'
    Complete-TestArchive $fixture
    $symlinkAttributes = [BitConverter]::ToInt32([BitConverter]::GetBytes([uint32]2684354560), 0)
    Add-TestZipEntry $fixture 'symlink.txt' $symlinkAttributes
    Assert-Fails { Invoke-WardogsPrepare $fixture.Context } 'Invalid or duplicate ZIP entry'
}

Invoke-Test 'prepared files are revalidated before readiness or installation' {
    $fixture = New-TestFixture 'staged-tamper'
    Complete-TestArchive $fixture
    Invoke-WardogsPrepare $fixture.Context
    Write-TestFile $fixture.Context.Stage 'a-first.dll' 'tampered staged content'
    Assert-Fails { Invoke-WardogsInstall $fixture.Context 0 -SkipRestart } 'has changed'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $fixture.Work 'install-ready.json'))) 'Installer declared itself ready after tampering.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'a-first.dll')) -eq 'old: a-first.dll') 'Rejected staged update changed the app.'
}

Invoke-Test 'cancellation before parent exit leaves installation unchanged' {
    $fixture = New-TestFixture 'cancel'
    Complete-TestArchive $fixture
    Invoke-WardogsPrepare $fixture.Context
    Write-TestFile $fixture.Work 'cancel-install' 'cancel'
    Assert-Fails { Invoke-WardogsInstall $fixture.Context $PID -SkipRestart } 'cancelled'
    Assert-True (-not (Test-Path -LiteralPath $fixture.Context.Backup)) 'Cancelled update created transaction backups.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'a-first.dll')) -eq 'old: a-first.dll') 'Cancelled update changed the app.'
}

Invoke-Test 'work directory must be a direct child with the private update name' {
    $fixture = New-TestFixture 'work-path'
    Assert-Fails { Get-WardogsContext $fixture.Install $fixture.Root ('0' * 64) $testVersion } 'direct child'
}

Invoke-Test 'same or older version cannot be installed' {
    $fixture = New-TestFixture 'not-newer'
    Complete-TestArchive $fixture
    Invoke-WardogsPrepare $fixture.Context
    $manifestPath = Join-Path $fixture.Install 'package-manifest.json'
    $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    $manifest.version = $testVersion
    Write-WardogsJson $manifestPath $manifest
    Assert-Fails { Invoke-WardogsInstall $fixture.Context 0 -SkipRestart } 'must be newer'
    Assert-True (-not (Test-Path -LiteralPath $fixture.Context.Backup)) 'A downgrade entered a transaction.'
}

Invoke-Test 'detached helper requires final approval after parent exit' {
    $fixture = New-TestFixture 'approval-required'
    Complete-TestArchive $fixture
    Invoke-WardogsPrepare $fixture.Context
    $unusedProcessId = [int]::MaxValue
    Assert-Fails { Invoke-WardogsInstall $fixture.Context $unusedProcessId -SkipRestart } 'not approved'
    Assert-True (-not (Test-Path -LiteralPath $fixture.Context.Backup)) 'Unapproved shutdown changed files.'
    Write-WardogsJson (Join-Path $fixture.Work 'install-approved.json') ([ordered]@{ state = 'approved'; parent_id = $unusedProcessId })
    Invoke-WardogsInstall $fixture.Context $unusedProcessId -SkipRestart
    Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'a-first.dll')) -eq 'new: a-first.dll') 'Approved update did not install.'
}

Invoke-Test 'actual helper command records prepare errors without launching the app' {
    $fixture = New-TestFixture 'command-error'
    Complete-TestArchive $fixture
    $fixture.Context.Sha256 = '0' * 64
    $process = Start-TestHelper $fixture 'Prepare' 0
    try {
        Assert-True ($process.WaitForExit(15000)) 'Prepare helper did not complete.'
        Assert-True ($process.ExitCode -ne 0) 'Prepare helper incorrectly reported success.'
        $errorPath = Join-Path $fixture.Work 'error.log'
        Assert-True (Test-Path -LiteralPath $errorPath -PathType Leaf) 'Prepare error log is missing.'
        Assert-True ([IO.File]::ReadAllText($errorPath).Contains('SHA-256')) 'Prepare error log does not identify the digest failure.'
    } finally { $process.Dispose() }
}

Invoke-Test 'actual detached helper emits readiness then respects cancellation while parent stays open' {
    $fixture = New-TestFixture 'command-cancel'
    Complete-TestArchive $fixture
    Invoke-WardogsPrepare $fixture.Context
    $process = Start-TestHelper $fixture 'Install' $PID
    try {
        $readyPath = Join-Path $fixture.Work 'install-ready.json'
        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        while (-not (Test-Path -LiteralPath $readyPath)) {
            if ($process.HasExited) { throw ('Installer failed before readiness: ' + $process.StandardError.ReadToEnd()) }
            if ([DateTime]::UtcNow -ge $deadline) { throw 'Installer did not declare itself ready.' }
            Start-Sleep -Milliseconds 50
        }
        $ready = [IO.File]::ReadAllText($readyPath) | ConvertFrom-Json
        Assert-True ($ready.state -eq 'ready' -and $ready.parent_id -eq $PID) 'Readiness receipt does not match the test parent.'
        Write-TestFile $fixture.Work 'cancel-install' 'cancel'
        Assert-True ($process.WaitForExit(10000)) 'Cancelled helper did not exit.'
        Assert-True ($process.ExitCode -ne 0) 'Cancelled helper reported success.'
        Assert-True (Test-Path -LiteralPath (Join-Path $fixture.Work 'error.log')) 'Cancellation error log is missing.'
        Assert-True (-not (Test-Path -LiteralPath $fixture.Context.Backup)) 'Cancelled detached helper modified installation.'
        Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Install 'a-first.dll')) -eq 'old: a-first.dll') 'Cancelled detached helper changed a package file.'
    } finally {
        if (-not $process.HasExited) { Write-TestFile $fixture.Work 'cancel-install' 'cancel'; [void]$process.WaitForExit(5000) }
        $process.Dispose()
    }
}

Invoke-Test 'installed manifest cannot claim user preferences or height data' {
    $fixture = New-TestFixture 'unsafe-old'
    Complete-TestArchive $fixture
    Invoke-WardogsPrepare $fixture.Context
    $manifestPath = Join-Path $fixture.Install 'package-manifest.json'
    $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    $manifest.files[0].path = 'settings.ini'
    Write-WardogsJson $manifestPath $manifest
    Assert-Fails { Invoke-WardogsInstall $fixture.Context 0 -SkipRestart } 'Protected'
    Assert-True (-not (Test-Path -LiteralPath $fixture.Context.Backup)) 'Unsafe old manifest entered a transaction.'
}

Invoke-Test 'junctions in installation directories are rejected' {
    $fixture = New-TestFixture 'junction'
    $target = Join-Path $fixture.Root 'junction-target'
    New-Item -ItemType Directory -Path $target | Out-Null
    New-Item -ItemType Junction -Path (Join-Path $fixture.Install 'junction-link') -Target $target | Out-Null
    Assert-Fails { Get-WardogsContext $fixture.Install $fixture.Work ('0' * 64) $testVersion } 'Reparse'
}

Write-Output "Installer tests passed: $script:Passed. Fixtures and recovery evidence: $testRoot"
