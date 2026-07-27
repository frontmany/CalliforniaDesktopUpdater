# End-to-end test of updater.exe against throwaway directories: builds a fake
# "installed app", a staged update signed with the dev key, runs apply, and
# checks the swap + manifest + cleanup. Needs the dev signing key (the same
# one whose public part is embedded via src/public_key.hpp).
#
# Usage:
#   .\scripts\integration-test.ps1
#   .\scripts\integration-test.ps1 -UpdaterExe .\build\Release\updater.exe

[CmdletBinding()]
param(
    [string]$UpdaterExe = "",
    [string]$PrivateKeyFile = "C:\projects\CallsBackend\Updates\keys\manifest-private.pem"
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $UpdaterExe) { $UpdaterExe = Join-Path $RepoRoot "build\Release\updater.exe" }
if (-not (Test-Path $UpdaterExe)) { throw "updater.exe not found: $UpdaterExe (build first)" }
if (-not (Test-Path $PrivateKeyFile)) { throw "signing key not found: $PrivateKeyFile" }

$openssl = (Get-Command openssl -ErrorAction SilentlyContinue).Source
if (-not $openssl) {
    $gitOpenssl = "C:\Program Files\Git\mingw64\bin\openssl.exe"
    if (Test-Path $gitOpenssl) { $openssl = $gitOpenssl } else { throw "openssl not found" }
}

$work = Join-Path $env:TEMP "updater-integration"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
$install = Join-Path $work "install"
$staging = Join-Path $work "staging"
New-Item -ItemType Directory -Force $install, $staging | Out-Null

function Write-Bytes([string]$Path, [string]$Content) {
    $dir = Split-Path $Path -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Content, [System.Text.UTF8Encoding]::new($false))
}

function Get-Sha([string]$Path) {
    (Get-FileHash $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function New-Manifest([string]$Version, [string]$Dir, [string[]]$Files) {
    $entries = foreach ($file in $Files) {
        $full = Join-Path $Dir $file
        $rel = $file -replace "\\", "/"
        "    { `"path`": `"$rel`", `"sha256`": `"$(Get-Sha $full)`", `"size`": $((Get-Item $full).Length) }"
    }
    @"
{
  "schema": 1,
  "key_id": "calls-updates-1",
  "version": "$Version",
  "platform": "win-x64",
  "channel": "stable",
  "min_supported_version": "0.1.0",
  "created_at": "2026-01-01T00:00:00Z",
  "notes": "",
  "files": [
$($entries -join ",`n")
  ]
}
"@ -replace "`r`n", "`n"
}

# --- Old install: Client.exe + updater.exe + keep.dll + old.dll -------------
# Client.exe must be a real (non-running) binary so the lock probe works.
Copy-Item $UpdaterExe (Join-Path $install "Client.exe")
Copy-Item $UpdaterExe (Join-Path $install "updater.exe")
Write-Bytes (Join-Path $install "keep.dll") "keep-v1"
Write-Bytes (Join-Path $install "old.dll") "old"
Write-Bytes (Join-Path $install "sub\changed.dll") "changed-v1"
$oldManifest = New-Manifest "0.1.0" $install @("Client.exe", "updater.exe", "keep.dll", "old.dll", "sub\changed.dll")
Write-Bytes (Join-Path $install "manifest.json") $oldManifest

# --- Staged update: changed.dll replaced, new.dll added, old.dll removed ----
Write-Bytes (Join-Path $staging "sub\changed.dll") "changed-v2"
Write-Bytes (Join-Path $staging "new.dll") "new"
Copy-Item (Join-Path $install "Client.exe") (Join-Path $staging "Client.exe")
Copy-Item (Join-Path $install "updater.exe") (Join-Path $staging "updater.exe")
# keep.dll intentionally NOT staged (unchanged -> not in diff ops).
$newManifestFiles = @("Client.exe", "updater.exe", "keep.dll", "new.dll", "sub\changed.dll")
# keep.dll hash must come from the install (not staged); build entries manually:
$entries = @(
    @{ p = "Client.exe";      f = Join-Path $staging "Client.exe" },
    @{ p = "updater.exe";     f = Join-Path $staging "updater.exe" },
    @{ p = "keep.dll";        f = Join-Path $install "keep.dll" },
    @{ p = "new.dll";         f = Join-Path $staging "new.dll" },
    @{ p = "sub/changed.dll"; f = Join-Path $staging "sub\changed.dll" }
) | ForEach-Object {
    "    { `"path`": `"$($_.p)`", `"sha256`": `"$(Get-Sha $_.f)`", `"size`": $((Get-Item $_.f).Length) }"
}
$newManifest = @"
{
  "schema": 1,
  "key_id": "calls-updates-1",
  "version": "0.2.0",
  "platform": "win-x64",
  "channel": "stable",
  "min_supported_version": "0.1.0",
  "created_at": "2026-01-02T00:00:00Z",
  "notes": "integration test",
  "files": [
$($entries -join ",`n")
  ]
}
"@ -replace "`r`n", "`n"
$manifestPath = Join-Path $staging "manifest.json"
Write-Bytes $manifestPath $newManifest

# Sign with the dev key.
$sigBin = Join-Path $work "manifest.sig.bin"
& $openssl pkeyutl -sign -rawin -inkey $PrivateKeyFile -in $manifestPath -out $sigBin
if ($LASTEXITCODE -ne 0) { throw "signing failed" }
[System.IO.File]::WriteAllText((Join-Path $staging "manifest.sig"),
    [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($sigBin)),
    [System.Text.UTF8Encoding]::new($false))

# --- Run apply (parent = short-lived process the updater waits out) ---------
# updater.exe is a GUI-subsystem binary: `&` would not wait -> Start-Process -Wait.
$parent = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", "ping -n 2 127.0.0.1 >nul" -PassThru -WindowStyle Hidden
$run = Start-Process -FilePath $UpdaterExe -ArgumentList @(
    "apply", "--install-dir", "`"$install`"", "--staging-dir", "`"$staging`"",
    "--parent-pid", $parent.Id, "--relaunch-exe", "Client.exe", "--no-relaunch",
    "--log", "`"$(Join-Path $work 'updater.log')`"") -Wait -PassThru
$exit = $run.ExitCode

# --- Assertions -------------------------------------------------------------
$failures = 0
function Assert([bool]$Condition, [string]$What) {
    if ($Condition) { Write-Host "  ok   $What" }
    else { Write-Host "  FAIL $What" -ForegroundColor Red; $script:failures++ }
}

Write-Host "updater exit code: $exit"
Assert ($exit -eq 0) "exit code 0"
Assert ((Get-Content (Join-Path $install "sub\changed.dll") -Raw) -eq "changed-v2") "changed.dll replaced"
Assert ((Get-Content (Join-Path $install "new.dll") -Raw) -eq "new") "new.dll added"
Assert (-not (Test-Path (Join-Path $install "old.dll"))) "old.dll removed"
Assert ((Get-Content (Join-Path $install "keep.dll") -Raw) -eq "keep-v1") "keep.dll untouched"
Assert (-not (Test-Path (Join-Path $install ".update"))) ".update cleaned"
Assert (-not (Test-Path $staging)) "staging cleaned"
$installedManifest = Get-Content (Join-Path $install "manifest.json") -Raw
Assert ($installedManifest -eq $newManifest) "manifest.json committed"

# --- Tamper check: corrupt signature must be rejected ------------------------
New-Item -ItemType Directory -Force $staging | Out-Null
Write-Bytes (Join-Path $staging "manifest.json") $newManifest
Write-Bytes (Join-Path $staging "manifest.sig") ([Convert]::ToBase64String([byte[]](1..64)))
Copy-Item (Join-Path $install "Client.exe") (Join-Path $staging "Client.exe")
$parent = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", "exit" -PassThru -WindowStyle Hidden
$run = Start-Process -FilePath $UpdaterExe -ArgumentList @(
    "apply", "--install-dir", "`"$install`"", "--staging-dir", "`"$staging`"",
    "--parent-pid", $parent.Id, "--relaunch-exe", "Client.exe", "--no-relaunch",
    "--log", "`"$(Join-Path $work 'updater.log')`"") -Wait -PassThru
Assert ($run.ExitCode -eq 4) "bad signature rejected (exit 4)"

if ($failures -eq 0) {
    Write-Host "`nintegration test passed" -ForegroundColor Green
    exit 0
}
Write-Host "`n$failures assertion(s) failed (log: $work\updater.log)" -ForegroundColor Red
exit 1
