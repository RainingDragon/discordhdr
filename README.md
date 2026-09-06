# DiscordHDRFix v1.1.0 — Hot-Reload Dev Host

This replaces the one-build-per-experiment workflow.

The DLL installs one stable detour on Discord's shared renderer and then polls:

```text
%LOCALAPPDATA%\DiscordHDRFix\host.cfg
```

every ~250 ms.

Changing Vencord settings rewrites that file atomically. The native host applies
the new behavior immediately; no Discord restart, stream restart, C++ rebuild,
or GitHub Actions run is required for normal HDR/SDR experiments.

## What is built into the host

### Shared-renderer tracer

The host records recent caller RVAs plus:

- source DXGI format
- original primaries
- original transfer
- original HDR-metadata pointer null/non-null
- selected action

This covers the v1.0.2 path-tracing goal.

### Live actions

Runtime modes:

```text
Observe only
Caller rules
Force SDR / preserve
Force HDR10 / Rec.2020 + PQ
Force scRGB / Rec.709 + Linear
Metadata only
```

HDR metadata uses the live `SDR white` and `Input max` settings.

### Live caller rules

Rule syntax:

```text
callerRva,format|any,metadata(any|null|nonnull),action
```

Actions:

```text
preserve
sdr
hdr10
scrgb
metadata
```

Separate rules with semicolons.

Example:

```text
0x3fd467,24,null,hdr10;0x4abcde,any,any,preserve
```

The host restores temporary primaries/transfer changes immediately after
Discord's renderer returns.

## Safety changes

Unlike the v1.0.1 caller-specific relay:

- no WumpusFrame register assumption
- shared renderer ABI only
- source reads/writes guarded with SEH
- unknown Discord build fails closed
- Observe mode is the default

## When another native rebuild is still required

Only if:

- Discord updates `discord_voice.node`
- the renderer ABI changes
- we need a fundamentally new hook point
- we need a new kind of native observation/action

Normal caller matching, tracing, HDR10/scRGB selection, metadata injection,
white/peak tuning, and bypass rules are all hot-swappable.
