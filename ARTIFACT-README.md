# DiscordHDRFix v1.2.1 Discord UI

Install as usual with Discord fully closed:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\Install-DiscordHDRFix.ps1
```

## Test

1. Start Discord and stream an application.
2. Open Discord's normal Go Live stream menu.
3. You should see a single `Discord HDR Fix` row rather than a large embedded form.
4. Hover it for quick profile selection.
5. Click it for the full non-modal per-app editor.
6. Click elsewhere in Discord; the full editor should close.
7. Open Vencord -> Plugins -> DiscordHDRFix settings to edit profiles there too.

RenoDX / ReShade is customizable per application. Its 360 / 200 starting values
do not affect native HDR profiles.
