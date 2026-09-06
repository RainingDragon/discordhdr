# DiscordHDRFix v1.2.0 — Per-Application Profiles

This build moves DiscordHDRFix from one global correction to a stream-scoped profile system.

## Stream-scoped logging

DiscordHDRFix does **not** create profiles for every installed, running, or available application.

Discord may construct `DesktopSource` objects while showing the screen-share picker. Those are only cached in memory. A profile is created or updated only when Discord actually commits the selected source through `setGoLiveSource()`.

Profiles are stored at:

```text
%LOCALAPPDATA%\DiscordHDRFix\profiles.json
```

## Default behavior

Every newly streamed application starts as **Automatic**.

```text
format 28 / R8G8B8A8
  -> SDR / no HDR metadata

format 24 / R10G10B10A2
  -> Native HDR10 / Rec.2020 + PQ / 460 + 1000

format 10 / R16G16B16A16_FLOAT
  -> scRGB / Rec.709 + Linear / 460 + 1000

unknown
  -> Observe / preserve
```

An application override takes precedence over Automatic and affects only that application.

## Per-application presets

```text
Automatic
SDR
Native HDR10
scRGB
RenoDX / ReShade
Custom
```

The initial RenoDX/ReShade preset is the current Dawnwalker near-match:

```text
Rec.2020
sRGB
Inject HDR metadata
SDR white 360
Input max 200
```

This does not change native-HDR games because it is stored per application.

## Controls in Discord's stream UI

While an application is actively being streamed, open Discord's normal stream-settings / Change Windows popout. DiscordHDRFix inserts a small **Discord HDR Fix** section into that popout.

It displays the active application, detected source format, selected profile, and applied correction. The profile can be changed live without stopping Go Live.

Custom exposes:

```text
Primaries
Transfer
HDR metadata policy
SDR white
Input max
```

`Reset this application to Automatic` returns only that application to automatic classification.

## Application identity

When Discord exposes `DesktopSource.sourcePid`, DiscordHDRFix caches it. The PID is not logged until that source is actually streamed.

At the confirmed stream commit, the Vencord native helper resolves the executable name/path. If Discord did not expose a PID, the Windows source-id HWND is used with `GetWindowThreadProcessId` as a fallback.
