param(
    [string]$VencordPath = "",
    [switch]$KeepNativeConfig
)

$ErrorActionPreference = "Stop"

function Is-VencordRoot([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    return (Test-Path (Join-Path $Path "package.json")) -and
           (Test-Path (Join-Path $Path "src"))
}

$Discord = Get-Process -Name "Discord", "DiscordCanary", "DiscordPTB" -ErrorAction SilentlyContinue
if ($Discord) {
    throw "Discord is running. Fully exit Discord before uninstalling DiscordHDRFix."
}

$ArtifactRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

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

if ($VencordRoot) {
    $PluginDest = Join-Path $VencordRoot "src\userplugins\DiscordHDRFix"
    if (Test-Path $PluginDest) {
        Remove-Item -Recurse -Force $PluginDest
        Write-Host "Removed Vencord DiscordHDRFix plugin."
    }

    if (Get-Command pnpm -ErrorAction SilentlyContinue) {
        Push-Location $VencordRoot
        try {
            & pnpm build
            if ($LASTEXITCODE -ne 0) { throw "pnpm build failed." }
            & pnpm inject
            if ($LASTEXITCODE -ne 0) { throw "pnpm inject failed." }
        }
        finally {
            Pop-Location
        }
    } else {
        Write-Host "pnpm not found; rebuild/reinject Vencord manually."
    }
} else {
    Write-Host "Vencord source checkout not found. Remove src\userplugins\DiscordHDRFix manually if needed."
}

if ($env:LOCALAPPDATA) {
    $NativeDir = Join-Path $env:LOCALAPPDATA "DiscordHDRFix"
    if (Test-Path $NativeDir) {
        if ($KeepNativeConfig) {
            Remove-Item -Force (Join-Path $NativeDir "DiscordHDRFix.Injector.exe") -ErrorAction SilentlyContinue
            Remove-Item -Force (Join-Path $NativeDir "DiscordHDRFix.Native.dll") -ErrorAction SilentlyContinue
            Write-Host "Removed native binaries; preserved tone-map.cfg."
        } else {
            Remove-Item -Recurse -Force $NativeDir
            Write-Host "Removed native files and configuration."
        }
    }
}

Write-Host "DiscordHDRFix uninstalled." -ForegroundColor Green
