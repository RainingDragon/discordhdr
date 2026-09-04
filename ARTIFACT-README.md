# DiscordHDRFix v0.5 Windows x64 artifact

This artifact was compiled by GitHub Actions on a Windows Server 2022 runner using
the Visual Studio 2022 x64 toolchain.

## Contents

```text
native/
  DiscordHDRFix.Injector.exe
  DiscordHDRFix.Native.dll

vencord/
  DiscordHDRFix/
    index.ts
    native.ts

install-native.ps1
SHA256SUMS.txt
```

## Install the native probe

From PowerShell inside this artifact folder:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\install-native.ps1
```

That only copies the two precompiled native files to:

```text
%LOCALAPPDATA%\DiscordHDRFix\
```

No compiler, CMake, Visual Studio, or Build Tools are needed on your PC.

## Install/update the Vencord plugin

Copy:

```text
vencord\DiscordHDRFix
```

to:

```text
Vencord\src\userplugins\DiscordHDRFix
```

Then use your normal Vencord commands:

```powershell
pnpm build
pnpm inject
```

Fully restart Discord.

## v0.5 test settings

Keep:

```text
HDR mode: Force SDR / never
```

The plugin also forces:

```text
useVideoHook = true
useGraphicsCapture = false
useGraphicsCaptureApiLevel = 0
```

Start Go Live, let it run for several seconds, then use:

```text
Vencord Toolbox
→ Show Native Frame Probe Status
```

The target result is:

```json
{
  "hook_installed": true,
  "frames_seen": 600,
  "mode": "pass-through",
  "error": ""
}
```

`frames_seen` should continue increasing.

v0.5 intentionally does not modify colors yet. It verifies the Windows native
pre-encode hook before the HDR→SDR shader is added.
