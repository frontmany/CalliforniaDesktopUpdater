# End-to-end test of updater.exe against throwaway directories: builds a fake
# "installed app", a staged update signed with the release key, runs apply, and
# checks the swap + manifest + cleanup.
#
# The key has to be the one whose public half is embedded in src/public_key.hpp,
# because verifying against that constant is half of what is being tested. There
# is exactly one such key and it lives in the secrets checkout. The default used
# to point at Updates\keys\manifest-private.pem, which no longer matches the
# embedded constant, so every run stopped at the first apply with exit 4 and a
# log line about a bad signature -- indistinguishable from the updater actually
# being broken.
#
# Usage:
#   .\scripts\integration-test.ps1
#   .\scripts\integration-test.ps1 -UpdaterExe .\build\Release\updater.exe

[CmdletBinding()]
param(
    [string]$UpdaterExe = "",
    [string]$PrivateKeyFile = "C:\projects\.callifornia-secrets\updates-manifest-private.pem"
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
  "key_id": "callifornia-updates-1",
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

# --- Old install: Callifornia.exe + updater.exe + keep.dll + old.dll -------------
# Callifornia.exe must be a real (non-running) binary so the lock probe works.
Copy-Item $UpdaterExe (Join-Path $install "Callifornia.exe")
Copy-Item $UpdaterExe (Join-Path $install "updater.exe")
Write-Bytes (Join-Path $install "keep.dll") "keep-v1"
Write-Bytes (Join-Path $install "old.dll") "old"
Write-Bytes (Join-Path $install "sub\changed.dll") "changed-v1"
$oldManifest = New-Manifest "0.1.0" $install @("Callifornia.exe", "updater.exe", "keep.dll", "old.dll", "sub\changed.dll")
Write-Bytes (Join-Path $install "manifest.json") $oldManifest

# --- Staged update: changed.dll replaced, new.dll added, old.dll removed ----
Write-Bytes (Join-Path $staging "sub\changed.dll") "changed-v2"
Write-Bytes (Join-Path $staging "new.dll") "new"
Copy-Item (Join-Path $install "Callifornia.exe") (Join-Path $staging "Callifornia.exe")
Copy-Item (Join-Path $install "updater.exe") (Join-Path $staging "updater.exe")
# keep.dll intentionally NOT staged (unchanged -> not in diff ops).
$newManifestFiles = @("Callifornia.exe", "updater.exe", "keep.dll", "new.dll", "sub\changed.dll")
# keep.dll hash must come from the install (not staged); build entries manually:
$entries = @(
    @{ p = "Callifornia.exe";      f = Join-Path $staging "Callifornia.exe" },
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
  "key_id": "callifornia-updates-1",
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
    "--parent-pid", $parent.Id, "--relaunch-exe", "Callifornia.exe", "--no-relaunch",
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
Copy-Item (Join-Path $install "Callifornia.exe") (Join-Path $staging "Callifornia.exe")
$parent = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", "exit" -PassThru -WindowStyle Hidden
$run = Start-Process -FilePath $UpdaterExe -ArgumentList @(
    "apply", "--install-dir", "`"$install`"", "--staging-dir", "`"$staging`"",
    "--parent-pid", $parent.Id, "--relaunch-exe", "Callifornia.exe", "--no-relaunch",
    "--log", "`"$(Join-Path $work 'updater.log')`"") -Wait -PassThru
Assert ($run.ExitCode -eq 4) "bad signature rejected (exit 4)"

# --- A swap target somebody else still has open -----------------------------
# The failure this whole grace exists for. An update that skips a release drops
# files the app had mapped a moment earlier -- the media stack, the D3D
# compiler -- and a scanner re-reads exactly those the instant the process
# holding them dies. Only the relaunch executable ever got waited for, so one
# straggling handle on any of the others failed the update, identically on
# every retry. Here the handle is held for longer than the rename backoff alone
# would have survived and released while the grace is still running.
$lockWork = Join-Path $env:TEMP "updater-integration-lock"
if (Test-Path $lockWork) { Remove-Item $lockWork -Recurse -Force }
$lockInstall = Join-Path $lockWork "install"
$lockStaging = Join-Path $lockWork "staging"
New-Item -ItemType Directory -Force $lockInstall, $lockStaging | Out-Null

Copy-Item $UpdaterExe (Join-Path $lockInstall "Callifornia.exe")
Copy-Item $UpdaterExe (Join-Path $lockInstall "updater.exe")
Write-Bytes (Join-Path $lockInstall "locked.dll") "held-open"
Write-Bytes (Join-Path $lockInstall "manifest.json") `
    (New-Manifest "0.1.0" $lockInstall @("Callifornia.exe", "updater.exe", "locked.dll"))
# The new release drops locked.dll and changes nothing else: one Remove op, so
# the run reaches the swap with nothing else able to fail first.
$lockManifest = New-Manifest "0.2.0" $lockInstall @("Callifornia.exe", "updater.exe")
$lockManifestPath = Join-Path $lockStaging "manifest.json"
Write-Bytes $lockManifestPath $lockManifest
& $openssl pkeyutl -sign -rawin -inkey $PrivateKeyFile -in $lockManifestPath -out $sigBin
if ($LASTEXITCODE -ne 0) { throw "signing failed" }
[System.IO.File]::WriteAllText((Join-Path $lockStaging "manifest.sig"),
    [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($sigBin)),
    [System.Text.UTF8Encoding]::new($false))

# 18 seconds: past the ~14 s of rename backoff that used to be the only
# patience in the apply path, and inside the 30 s grace.
$holdSeconds = 18
$target = Join-Path $lockInstall "locked.dll"
$holder = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList @(
    "-NoProfile", "-Command",
    "`$h=[System.IO.File]::Open('$target',[System.IO.FileMode]::Open,[System.IO.FileAccess]::Read,[System.IO.FileShare]::None);" +
    "Start-Sleep -Seconds $holdSeconds; `$h.Close()")
Start-Sleep -Milliseconds 800  # let the handle actually be open before we start

$parent = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", "exit" -PassThru -WindowStyle Hidden
$lockLog = Join-Path $lockWork "updater.log"
$run = Start-Process -FilePath $UpdaterExe -ArgumentList @(
    "apply", "--install-dir", "`"$lockInstall`"", "--staging-dir", "`"$lockStaging`"",
    "--parent-pid", $parent.Id, "--relaunch-exe", "Callifornia.exe", "--no-relaunch",
    "--log", "`"$lockLog`"") -Wait -PassThru
$holder | Wait-Process -Timeout 30 -ErrorAction SilentlyContinue

Assert ($run.ExitCode -eq 0) "a swap target held open is waited for, not failed"
Assert (-not (Test-Path $target)) "locked.dll removed once its holder let go"
Assert (-not (Test-Path (Join-Path $lockInstall ".update"))) ".update cleaned after the wait"
$lockLogText = if (Test-Path $lockLog) { Get-Content $lockLog -Raw } else { "" }
Assert ($lockLogText -match "swap targets still open") "the wait is recorded in the log"
Assert ($lockLogText -match "all swap targets released") "and so is the release"

if ($failures -eq 0) {
    Write-Host "`nintegration test passed" -ForegroundColor Green
    exit 0
}
Write-Host "`n$failures assertion(s) failed (log: $work\updater.log)" -ForegroundColor Red
exit 1
