# DiscordHDRFix v0.6.1 Windows x64

v0.6.1 corrects the HDR metadata object passed to Discord's own D3D11 renderer.

## What changed from v0.6

The exact Windows discord_voice.node build reads three metadata fields:

```text
+0x00  float
+0x04  float
+0x08  byte state
```

Discord's own valid-HDR builder writes `state = 1`.

v0.6.1 supplies the complete 12-byte object:

```cpp
struct HdrMetadata {
    float sdrWhiteLevel;
    float inputMaxLuminance;
    uint8_t state;       // always 1 for injected valid HDR metadata
    uint8_t padding[3];
};
```

The two luminance settings remain live-editable from the Vencord plugin.
No rebuild or restream is required when changing them.

## Install

Fully exit Discord.

From PowerShell in this artifact folder:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\install-native.ps1
```

Replace:

```text
Vencord\src\userplugins\DiscordHDRFix
```

with:

```text
vencord\DiscordHDRFix
```

Then from the Vencord root:

```powershell
pnpm build
pnpm inject
```

Fully restart Discord.

## Test

Use the values that looked best in v0.6 first. If those were:

```text
SDR white: 600
Input max: 460
```

start v0.6.1 with those same values so the only A/B variable is the metadata
state/layout fix.

Start Go Live and open:

```text
Vencord Toolbox
→ Show Native HDR Fix Status
```

Look for:

```json
{
  "version": "0.6.1",
  "hook_installed": true,
  "metadata_state": 1,
  "metadata_size": 12,
  "hdr_metadata_injected": 1000,
  "error": ""
}
```
