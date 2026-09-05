DiscordHDRFix v1.0.0
=======================

What it fixes
-------------
Discord Go Live can make HDR games look blown out, gray, or washed out to viewers.
DiscordHDRFix keeps Discord's normal Go Live capture/audio/encoder pipeline, but fixes
the HDR color interpretation before Discord's existing renderer/encoder.

Recommended behavior is automatic:
- 10-bit HDR10 surface -> Rec.2020 + ST.2084/PQ
- FP16 scRGB surface   -> Rec.709 + Linear
- Other surfaces       -> left unchanged

Default tone-map values:
- SDR white: 460
- Input max luminance: 1000

Requirements
------------
- Windows 64-bit Discord desktop
- Vencord source checkout already set up with pnpm
- The supported discord_voice.node build (the fix safely refuses to patch unknown builds)

Install
-------
1. Fully exit Discord from the system tray.
2. Extract this ZIP.
3. Right-click PowerShell in this folder and run:

   Set-ExecutionPolicy -Scope Process Bypass
   .\Install-DiscordHDRFix.ps1

4. If the script cannot find Vencord, give it the path, for example:

   .\Install-DiscordHDRFix.ps1 -VencordPath "C:\Vencord"

5. Start Discord and make sure the DiscordHDRFix Vencord plugin is enabled.
6. Stream HDR games normally.

The plugin starts the native fix automatically.

Tuning
------
The defaults are the final tested baseline. You normally do not need to change anything.

If desired, Vencord's DiscordHDRFix settings expose:
- SDR white level
- Input max luminance
- Source color mode

"Auto HDR by DXGI format" is recommended.

Uninstall
---------
Fully exit Discord, then run:

   .\Uninstall-DiscordHDRFix.ps1

Notes
-----
The viewer receives an SDR stream produced by Discord's own HDR renderer, so it will not
look pixel-identical to the HDR image on the broadcaster's monitor. Small differences in
near-black detail, vignettes, highlight rolloff, or local contrast are normal.

If Discord updates discord_voice.node, the native patch is intentionally fail-closed.
