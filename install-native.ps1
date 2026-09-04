$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Source = Join-Path $Root "native"

if (-not $env:LOCALAPPDATA) {
    throw "LOCALAPPDATA is not available."
}

$Install = Join-Path $env:LOCALAPPDATA "DiscordHDRFix"

$Injector = Join-Path $Source "DiscordHDRFix.Injector.exe"
$Dll = Join-Path $Source "DiscordHDRFix.Native.dll"

if (-not (Test-Path $Injector)) {
    throw "Missing $Injector"
}

if (-not (Test-Path $Dll)) {
    throw "Missing $Dll"
}

New-Item -ItemType Directory -Force -Path $Install | Out-Null

Copy-Item -Force $Injector (Join-Path $Install "DiscordHDRFix.Injector.exe")
Copy-Item -Force $Dll (Join-Path $Install "DiscordHDRFix.Native.dll")

Write-Host ""
Write-Host "DiscordHDRFix native probe installed to:"
Write-Host "  $Install"
Write-Host ""
Write-Host "Files:"
Get-ChildItem $Install | Where-Object {
    $_.Name -in @("DiscordHDRFix.Injector.exe", "DiscordHDRFix.Native.dll")
} | Format-Table Name, Length, LastWriteTime
Write-Host ""
Write-Host "Now copy vencord\DiscordHDRFix into Vencord\src\userplugins\DiscordHDRFix"
Write-Host "and run pnpm build / pnpm inject as usual."
