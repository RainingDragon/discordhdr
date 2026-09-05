param(
    [string]$VencordPath = "",
    [switch]$SkipVencordBuild
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    Write-Host ""
    Write-Host "ERROR: $Message" -ForegroundColor Red
    Write-Host ""
    exit 1
}

function Is-VencordRoot([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    return (Test-Path (Join-Path $Path "package.json")) -and
           (Test-Path (Join-Path $Path "src"))
}

$ArtifactRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$NativeSource = Join-Path $ArtifactRoot "native"
$PluginSource = Join-Path $ArtifactRoot "vencord\DiscordHDRFix"

$Injector = Join-Path $NativeSource "DiscordHDRFix.Injector.exe"
$Dll = Join-Path $NativeSource "DiscordHDRFix.Native.dll"

if (-not (Test-Path $Injector)) { Fail "Missing $Injector. Download the compiled GitHub Actions artifact, not the source ZIP." }
if (-not (Test-Path $Dll)) { Fail "Missing $Dll. Download the compiled GitHub Actions artifact, not the source ZIP." }
if (-not (Test-Path (Join-Path $PluginSource "index.ts"))) { Fail "Vencord plugin files are missing from this package." }

$Discord = Get-Process -Name "Discord", "DiscordCanary", "DiscordPTB" -ErrorAction SilentlyContinue
if ($Discord) {
    Fail "Discord is running. Fully exit Discord from the system tray, then run this installer again."
}

if (-not $env:LOCALAPPDATA) { Fail "LOCALAPPDATA is unavailable." }

$InstallDir = Join-Path $env:LOCALAPPDATA "DiscordHDRFix"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

Write-Host "Installing DiscordHDRFix v1.0.0 native files..." -ForegroundColor Cyan
Copy-Item -Force $Injector (Join-Path $InstallDir "DiscordHDRFix.Injector.exe")
Copy-Item -Force $Dll (Join-Path $InstallDir "DiscordHDRFix.Native.dll")

$ConfigPath = Join-Path $InstallDir "tone-map.cfg"
if (-not (Test-Path $ConfigPath)) {
    @"
enabled=1
sdr_white=460
input_max=1000
source_mode=1
"@ | Set-Content -Encoding ascii $ConfigPath
    Write-Host "Created recommended defaults: Auto HDR, SDR white 460, input max 1000."
} else {
    Write-Host "Existing HDR tuning config preserved."
}

if ($SkipVencordBuild) {
    Write-Host ""
    Write-Host "Native portion installed. Vencord build skipped by request." -ForegroundColor Yellow
    Write-Host "Plugin source: $PluginSource"
    exit 0
}

$candidates = @()
if ($VencordPath) { $candidates += $VencordPath }

$current = (Get-Location).Path
$candidates += $current
$candidates += (Join-Path $HOME "Vencord")
$candidates += (Join-Path $HOME "Downloads\Vencord")
$candidates += (Join-Path $HOME "Documents\Vencord")
$candidates += (Join-Path $HOME "Desktop\Vencord")
$candidates += (Join-Path (Split-Path $ArtifactRoot -Parent) "Vencord")

$VencordRoot = $null
foreach ($candidate in ($candidates | Select-Object -Unique)) {
    if (Is-VencordRoot $candidate) {
        $VencordRoot = (Resolve-Path $candidate).Path
        break
    }
}

if (-not $VencordRoot) {
    Write-Host ""
    Write-Host "I couldn't auto-detect your Vencord source checkout." -ForegroundColor Yellow
    Write-Host "Paste the Vencord folder path (the folder containing package.json), or press Enter to stop:"
    $entered = Read-Host "Vencord path"
    if ($entered -and (Is-VencordRoot $entered)) {
        $VencordRoot = (Resolve-Path $entered).Path
    } else {
        Write-Host ""
        Write-Host "Native files are installed, but the Vencord plugin was not installed."
        Write-Host "Re-run with:"
        Write-Host '  .\Install-DiscordHDRFix.ps1 -VencordPath "C:\path\to\Vencord"'
        exit 2
    }
}

$UserPlugins = Join-Path $VencordRoot "src\userplugins"
$PluginDest = Join-Path $UserPlugins "DiscordHDRFix"
New-Item -ItemType Directory -Force -Path $UserPlugins | Out-Null

if (Test-Path $PluginDest) {
    Remove-Item -Recurse -Force $PluginDest
}
Copy-Item -Recurse -Force $PluginSource $PluginDest

$Pnpm = Get-Command pnpm -ErrorAction SilentlyContinue
if (-not $Pnpm) {
    Fail "pnpm is not installed or not on PATH. The native files and plugin source were copied, but Vencord could not be rebuilt."
}

Write-Host ""
Write-Host "Building Vencord with DiscordHDRFix..." -ForegroundColor Cyan
Push-Location $VencordRoot
try {
    & pnpm build
    if ($LASTEXITCODE -ne 0) { Fail "pnpm build failed with exit code $LASTEXITCODE." }

    Write-Host ""
    Write-Host "Injecting Vencord..." -ForegroundColor Cyan
    & pnpm inject
    if ($LASTEXITCODE -ne 0) { Fail "pnpm inject failed with exit code $LASTEXITCODE." }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "DiscordHDRFix v1.0.0 installed successfully." -ForegroundColor Green
Write-Host ""
Write-Host "Next:"
Write-Host "  1. Start Discord."
Write-Host "  2. Enable the DiscordHDRFix plugin in Vencord if it is not already enabled."
Write-Host "  3. Stream an HDR game normally."
Write-Host ""
Write-Host "Default correction:"
Write-Host "  Auto HDR by DXGI format"
Write-Host "  SDR white: 460"
Write-Host "  Input max: 1000"
