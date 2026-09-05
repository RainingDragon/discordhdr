param([string]$VencordPath = "")
& (Join-Path $PSScriptRoot "Install-DiscordHDRFix.ps1") -VencordPath $VencordPath
exit $LASTEXITCODE
