# DiscordHDRFix v1.1.0 engineering notes

## Stable hook

Target:

```text
discord_voice.node + 0x52cad0
```

The host verifies the exact PE timestamp, image size, and the 16-byte shared
renderer prologue before patching.

The detour jumps directly into a typed Microsoft x64 ABI hook with the same
nine-argument signature as Discord's renderer wrapper.

`_ReturnAddress()` therefore resolves the original native renderer caller RVA
without needing caller-specific register assumptions.

## Trampoline

The trampoline replays the exact 16-byte original renderer prologue and then
jumps to `renderer+16`.

The typed hook calls that trampoline, allowing the host to restore temporary
source color bytes after Discord's renderer returns.

## Source descriptor handling

Known renderer fields:

```text
+0x178 DXGI_FORMAT
+0x17c primaries/gamut
+0x17d transfer
```

Reads and writes are guarded with SEH. The shared renderer itself consumes
these fields, but the guard prevents a diagnostic rule from crashing Discord
if a future path supplies an unexpected object.

## Config hot reload

File:

```text
%LOCALAPPDATA%\DiscordHDRFix\host.cfg
```

Polled every 250 ms.

The Vencord helper writes via temp-file + atomic rename.

Published `RuntimeConfig` objects are immutable and atomically swapped. Old
snapshots are intentionally retained for the Discord process lifetime so a
render thread can never observe freed rule memory.

## Rule grammar

```text
callerRva,format|any,metadata(any|null|nonnull),action
```

Examples:

```text
0x3fd467,24,null,hdr10
0x4abcde,10,any,scrgb
0x123456,any,nonnull,preserve
```

Actions:

```text
preserve
sdr
hdr10
scrgb
metadata
```

## Current default

Observe only. This deliberately combines path discovery and correction into one
native build without prematurely guessing the AutoHDR caller.


## v1.1.1 flexible source interpretation

The renderer already exposes orthogonal source primaries and transfer enums at descriptor offsets `+0x17c` and `+0x17d`. v1.1.1 stops treating the existing presets as the only legal combinations.

Custom mode can independently preserve/override:

```text
primaries: preserve / Rec709 / Rec2020 / Arc
transfer:  preserve / Linear / sRGB / ST2084
metadata:  preserve / null / injected 12-byte metadata
```

This is still metadata/interpretation testing only; it does not add a new GPU pixel shader.
