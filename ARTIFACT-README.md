# DiscordHDRFix v0.7 Windows x64

## Install

Fully exit Discord, then:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\install-native.ps1
```

Replace your Vencord userplugin with:

```text
vencord\DiscordHDRFix
```

Then:

```powershell
pnpm build
pnpm inject
```

Fully restart Discord.

## First v0.7 test

Keep the luminance values that currently give the closest brightness. For the
current test system that may be:

```text
SDR white: 600
Input max: 460
```

Set:

```text
Source color mode: Preserve Discord source metadata
```

Start Go Live, wait a few seconds, then open:

```text
Vencord Toolbox
→ Show Native HDR Fix Status
```

Send the full status. The important new fields are:

```text
source_format_name
original_primaries_name
original_transfer_name
effective_primaries_name
effective_transfer_name
```

Once those are known, source-color modes can be switched live without
restarting the stream.
