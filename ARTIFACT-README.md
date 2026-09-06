# DiscordHDRFix v1.0.1 Auto Test — compiled artifact

Do not hand this to another user as the final release yet. This build validates
automatic HDR/SDR detection.

## Install

Fully exit Discord, then:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\Install-DiscordHDRFix.ps1
```

Replace/rebuild the Vencord plugin if the installer does not find your Vencord
checkout automatically.

## Required settings

```text
Correction enabled: ON
Detection mode: Automatic
SDR white: 460
Input max: 1000
```

## Test both

1. Native HDR game.
2. Slay the Spire 2 with Windows AutoHDR OFF.

After each stream, use:

```text
Vencord Toolbox
-> Show Native HDR Fix Status
```

Send the full status.
