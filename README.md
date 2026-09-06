# DiscordHDRFix v1.0.1 — Automatic HDR/SDR Detection Test

This is the test build for the last major behavior needed before a final
brother-ready release: **automatic SDR bypass and HDR correction**.

## What changed

The exact analyzed Windows `discord_voice.node` exposes a boolean field:

```text
WumpusFrame::is_source_hdr
offset: +0x1da
```

At the Video Hook renderer callsite the frame pointer is still in `r12`, so the
native relay can classify every frame without looking at image content and
without assuming that a 10-bit texture is necessarily HDR.

Automatic policy:

```text
is_source_hdr = false
  -> leave Discord's SDR path alone
  -> NO HDR metadata injection
  -> NO gamut/transfer override

is_source_hdr = true
  + R10G10B10A2_UNORM
  -> Rec.2020 + ST.2084/PQ
  -> inject HDR metadata (default 460 / 1000)

is_source_hdr = true
  + R16G16B16A16_FLOAT
  -> Rec.709 + Linear/scRGB
  -> inject HDR metadata

is_source_hdr = true
  + unknown format
  -> preserve Discord's source color enums
  -> inject the missing HDR metadata
```

## Why this is a test build

We have proven the two individual behaviors visually:

- Native HDR: Rec.2020 + PQ + metadata looks substantially correct.
- SDR: disabling HDR metadata injection fixes the darker/gray SDR stream.

This build tests whether Discord's `WumpusFrame::is_source_hdr` byte reliably
distinguishes those two cases in Video Hook.

## Test 1 — native HDR game

Settings:

```text
Correction enabled: ON
Detection mode: Automatic
SDR white: 460
Input max: 1000
```

Expected native status:

```text
last_source_is_hdr: true
last_decision: hdr10_rec2020_pq    (for a 10-bit HDR10 game)
hdr_frames_corrected: increasing
hdr_metadata_injected: increasing
```

The image should look like the working manual Rec.2020 + PQ result.

## Test 2 — Slay the Spire 2, Windows AutoHDR OFF

Use the same Automatic settings.

Expected native status:

```text
last_source_is_hdr: false
last_decision: sdr_bypass
sdr_frames_bypassed: increasing
```

`hdr_metadata_injected` should stop increasing while only that SDR stream is
being rendered.

The stream should look like the previously successful manual combination:
tone mapping OFF + preserve source metadata.

## Manual escape hatches

The Vencord setting still offers:

```text
Automatic
Force SDR / no tone mapping
Force HDR10 / Rec.2020 + PQ
Force scRGB / Rec.709 + Linear
```

These are diagnostic overrides, not intended for normal use.

## Target build

Exact `discord_voice.node` SHA-256:

```text
54d452eefb5bd20f022dca1f93e3a92cf641e14283761720e4276f3fb0585db9
```

The patch also verifies the Video Hook register setup, renderer callsite, and an
independent machine-code read of `[WumpusFrame + 0x1da]`. It fails closed if
those signatures change.
