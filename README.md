# DiscordHDRFix v0.7

v0.7 keeps the complete HDR metadata injection from v0.6.1 and adds direct
visibility/control over the source texture color interpretation that Discord's
renderer uses on the Video Hook path.

Recovered source descriptor fields in the exact analyzed Windows build:

```text
+0x178  DXGI_FORMAT
+0x17c  gamut / primaries: 0=Rec709, 1=Rec2020, 2=Arc
+0x17d  transfer:          0=Linear, 1=sRGB, 2=SMPTE ST 2084/PQ
```

The plugin exposes live source-color overrides. No rebuild or restream is
required when switching between them.

Start with **Preserve Discord source metadata**, start Go Live, and inspect
**Show Native HDR Fix Status**. The status reports the original/effective
format, primaries and transfer function.

For common HDR surfaces:

```text
DXGI_FORMAT 10  R16G16B16A16_FLOAT   -> usually Rec709 + Linear (scRGB)
DXGI_FORMAT 24  R10G10B10A2_UNORM    -> commonly Rec2020 + ST2084/PQ
```

The `Auto HDR by DXGI format` mode applies exactly those two mappings and leaves
other formats unchanged.
