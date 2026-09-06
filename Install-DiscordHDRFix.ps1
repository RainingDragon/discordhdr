param(
    [string]$VencordPath = ""
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    Write-Host ""
    Write-Host "ERROR: $Message" -ForegroundColor Red
    Write-Host ""
    exit 1
}

function Is-VencordRoot([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $false
    }

    return (Test-Path (Join-Path $Path "package.json")) -and
           (Test-Path (Join-Path $Path "src"))
}

$ArtifactRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$NativeSource = Join-Path $ArtifactRoot "native"
$PluginSource = Join-Path $ArtifactRoot "vencord\DiscordHDRFix"

$Injector = Join-Path $NativeSource "DiscordHDRFix.Injector.exe"
$Dll = Join-Path $NativeSource "DiscordHDRFix.Native.dll"

if (-not (Test-Path $Injector)) {
    Fail "Missing $Injector. Use the compiled GitHub Actions artifact."
}

if (-not (Test-Path $Dll)) {
    Fail "Missing $Dll. Use the compiled GitHub Actions artifact."
}

$Discord = Get-Process -Name "Discord", "DiscordCanary", "DiscordPTB" -ErrorAction SilentlyContinue
if ($Discord) {
    Fail "Discord is running. Fully exit it from the system tray first."
}

if (-not $env:LOCALAPPDATA) {
    Fail "LOCALAPPDATA is unavailable."
}

$InstallDir = Join-Path $env:LOCALAPPDATA "DiscordHDRFix"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

Copy-Item -Force $Injector (Join-Path $InstallDir "DiscordHDRFix.Injector.exe")
Copy-Item -Force $Dll (Join-Path $InstallDir "DiscordHDRFix.Native.dll")

$ConfigPath = Join-Path $InstallDir "host.cfg"

if (-not (Test-Path $ConfigPath)) {
@"
enabled=1
trace=1
mode=observe
sdr_white=460
input_max=1000
custom_primaries=-1
custom_transfer=-1
custom_metadata=0
rules=
"@ | Set-Content -Encoding ascii $ConfigPath
}

$candidates = @()
if ($VencordPath) { $candidates += $VencordPath }
$candidates += (Get-Location).Path
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
    Write-Host "Vencord source checkout not auto-detected." -ForegroundColor Yellow
    $entered = Read-Host "Paste Vencord root path"

    if ($entered -and (Is-VencordRoot $entered)) {
        $VencordRoot = (Resolve-Path $entered).Path
    } else {
        Fail "Vencord root not found."
    }
}

$PluginDest = Join-Path $VencordRoot "src\userplugins\DiscordHDRFix"

if (Test-Path $PluginDest) {
    Remove-Item -Recurse -Force $PluginDest
}

Copy-Item -Recurse -Force $PluginSource $PluginDest

if (-not (Get-Command pnpm -ErrorAction SilentlyContinue)) {
    Fail "pnpm is not installed or not on PATH."
}

Push-Location $VencordRoot

try {
    & pnpm build
    if ($LASTEXITCODE -ne 0) {
        Fail "pnpm build failed."
    }

    & pnpm inject
    if ($LASTEXITCODE -ne 0) {
        Fail "pnpm inject failed."
    }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "DiscordHDRFix v1.2.3 UI polish installed." -ForegroundColor Green
Write-Host "Start Discord and enable the DiscordHDRFix plugin."
Write-Host ""
Write-Host "Default mode is Observe only."
Write-Host "Settings and caller rules hot-reload without restarting Discord."
