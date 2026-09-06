# DiscordHDRFix v1.1.1 — Flexible Source Interpretation

This build keeps the v1.1 shared-renderer dev host and adds one important capability:
**primaries, transfer function, and HDR metadata can now be controlled independently.**

That is specifically for sources such as RenoDX/ReShade where none of the coarse presets
(SDR, HDR10, scRGB, metadata-only) match the captured encoding.

## New live mode

Choose:

```text
Mode: Custom source interpretation
```

Then independently select:

```text
Primaries:
  Preserve
  Rec.709
  Rec.2020
  Arc

Transfer:
  Preserve
  Linear
  sRGB
  ST.2084 / PQ

HDR metadata:
  Preserve
  None
  Inject 460 / 1000
```

No stream restart is required when changing these settings.

The existing white/peak calibration remains:

```text
SDR white: 460
Input max: 1000
```

## Recommended Dawnwalker / RenoDX test sequence

Keep metadata on **Inject** and compare:

```text
Rec.2020 + sRGB
Rec.709  + PQ
Rec.2020 + Linear
Rec.709  + sRGB
```

Then repeat the best transfer/gamut combination with metadata **None** if needed.

The purpose is to determine whether RenoDX's captured 10-bit surface is using a
non-standard combination rather than assuming that `R10G10B10A2_UNORM` always means
Rec.2020/PQ.

Caller rules can also use action `custom`, which applies the current custom settings.
