# DiscordHDRFix v1.0.0 compiled Windows artifact

This is the package intended to hand to another user.

## Quick install

Fully exit Discord, then run:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\Install-DiscordHDRFix.ps1
```

The installer:

1. Installs `DiscordHDRFix.Injector.exe` and `DiscordHDRFix.Native.dll` to
   `%LOCALAPPDATA%\DiscordHDRFix`.
2. Creates the tested defaults on a clean install:
   - Auto HDR by DXGI format
   - SDR white 460
   - Input max 1000
3. Copies the Vencord userplugin into `src\userplugins\DiscordHDRFix`.
4. Runs `pnpm build`.
5. Runs `pnpm inject`.

If Vencord cannot be auto-detected:

```powershell
.\Install-DiscordHDRFix.ps1 -VencordPath "C:\path\to\Vencord"
```

## Uninstall

Fully exit Discord:

```powershell
.\Uninstall-DiscordHDRFix.ps1
```

## Expected automatic mapping

```text
R10G10B10A2_UNORM  -> Rec.2020 + ST.2084 / PQ
R16G16B16A16_FLOAT -> Rec.709 + Linear / scRGB
other formats       -> preserve Discord metadata
```

## Important

The native patch is version-locked to the analyzed `discord_voice.node` and fails closed
on an unknown Discord build.
