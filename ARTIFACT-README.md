# DiscordHDRFix v1.2.0 Per-App Profiles

## Install

Fully exit Discord:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\Install-DiscordHDRFix.ps1
```

Then start Discord and enable `DiscordHDRFix`.

## Normal use

Do nothing. Every application starts in **Automatic** mode.

DiscordHDRFix logs only applications you actually stream.

## Override one application

While streaming, open Discord's normal Stream Settings / Change Windows popout. A **Discord HDR Fix** section should appear with:

```text
Automatic
SDR
Native HDR10
scRGB
RenoDX / ReShade
Custom
```

The choice is saved only for the current application and applies live.

For Dawnwalker/RenoDX, use `RenoDX / ReShade` as the starting profile. It uses the current near-match of Rec.2020 + sRGB + injected metadata at 360 / 200 without changing the native-HDR 460 / 1000 baseline for other games.
