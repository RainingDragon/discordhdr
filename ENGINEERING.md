# DiscordHDRFix v1.0.0 engineering notes

Target discord_voice.node SHA-256:

54d452eefb5bd20f022dca1f93e3a92cf641e14283761720e4276f3fb0585db9

Renderer callsite:

- Video Hook renderer CALL: 0x003fd462
- Shared renderer wrapper:  0x0052cad0

At the wrapper entry, the fourth argument (`r9`) becomes the renderer's source
descriptor. The renderer reads:

```text
descriptor + 0x178 : DXGI_FORMAT u32
descriptor + 0x17c : gamut / primaries enum
descriptor + 0x17d : transfer-function enum
```

The enum strings and renderer branches establish:

```text
primaries 0 = Rec709
primaries 1 = Rec2020
primaries 2 = Arc

transfer 0 = Linear
transfer 1 = sRGB
transfer 2 = SMPTE ST 2084 / PQ
```

The renderer also explicitly branches on DXGI formats 10 and 24 for HDR paths:

```text
10 = DXGI_FORMAT_R16G16B16A16_FLOAT
24 = DXGI_FORMAT_R10G10B10A2_UNORM
```

v0.7 replaces the original renderer CALL with a nearby relay that jumps to a
typed x64 hook function. The hook:

1. Records the source format/primaries/transfer.
2. Optionally changes only the stack-local source descriptor's primaries and
   transfer bytes.
3. Supplies the 12-byte HDR metadata object if enabled.
4. Calls Discord's original renderer wrapper with every other argument intact.

No texture copies, CPU readback, extra encoder, mirror window or audio changes.


## v1.0.0 finalization

Final defaults:

```text
source color mode: Auto HDR by DXGI format
SDR white: 460
input max luminance: 1000
```

The source descriptor primaries/transfer override is now scoped to the single renderer
call: the original bytes are restored immediately after Discord's renderer returns.
This prevents the diagnostic override from leaking into persistent Discord state and
makes live mode changes reversible.

The plugin still forces the proven routing:

```text
hdrCaptureMode = never
useVideoHook = true
useGraphicsCapture = false
useGraphicsCaptureApiLevel = 0
```
