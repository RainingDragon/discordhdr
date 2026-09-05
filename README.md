# DiscordHDRFix v1.0.0

DiscordHDRFix fixes washed-out / blown-out HDR game streams in Discord Go Live while
keeping Discord's normal capture, per-game soundshare, encoder, bitrate/FPS controls,
preview and Go Live UI.

## Final pipeline

```text
HDR game
  ↓
Discord D3D11 Video Hook
  ↓
DiscordHDRFix
  ├─ supplies the HDR metadata Video Hook omits
  └─ corrects source gamut / transfer interpretation
  ↓
Discord's existing native HDR renderer / tone mapper
  ↓
Discord's existing encoder
  ↓
Go Live
```

No mirror window, CPU frame readback, second capture session, second encoder, or audio proxy.

## Root cause found

On the tested Windows Discord build, Video Hook delivered an HDR-capable
`DXGI_FORMAT_R10G10B10A2_UNORM` source but identified it to Discord's renderer as:

```text
Rec.709 + sRGB
```

The tested HDR game looked substantially correct when interpreted as:

```text
Rec.2020 + SMPTE ST.2084 / PQ
```

v1.0.0 defaults to **Auto HDR by DXGI format**:

```text
R10G10B10A2_UNORM  → Rec.2020 + ST.2084/PQ
R16G16B16A16_FLOAT → Rec.709  + Linear (scRGB)
other formats       → preserve Discord metadata
```

Recommended tone-map defaults from testing:

```text
SDR white: 460
Input max luminance: 1000
```

These remain live-editable from the Vencord plugin.

## Safety / compatibility

The native patch is locked to the exact analyzed `discord_voice.node` build:

```text
SHA-256:
54d452eefb5bd20f022dca1f93e3a92cf641e14283761720e4276f3fb0585db9
```

The DLL also verifies PE metadata and exact instruction signatures before patching.
If Discord changes the target binary, the patch refuses to install rather than guessing.

## Build

GitHub Actions builds the Windows x64 DLL and injector. The downloadable Actions artifact
contains the compiled native files, Vencord plugin and one-command installer.

## Install from the Actions artifact

Fully exit Discord, extract the artifact, then:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\Install-DiscordHDRFix.ps1
```

If needed:

```powershell
.\Install-DiscordHDRFix.ps1 -VencordPath "C:\path\to\Vencord"
```

See `README-FIRST.txt` in the compiled artifact.
