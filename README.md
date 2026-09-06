# DiscordHDRFix v1.2.2 — Stream UI Fix

v1.2.1 could hide the Discord HDR Fix row entirely when the runtime
MediaEngine source hook did not receive the active Go Live source.

v1.2.2 fixes that in two ways:

1. Restores the proven webpack `setGoLiveSource()` / `clearDesktopSource()`
   commit hooks from v1.2.0.
2. Keeps the v1.2.1 runtime MediaEngine hook as a second fallback.

The Discord HDR Fix row now appears whenever the active stream menu is detected,
even while application identity is still resolving.

If source resolution has not happened yet, the row displays:

```text
Discord HDR Fix
Detecting active stream…
```

Hover/click remains usable and shows a nonblocking diagnostic state instead of
silently omitting the feature.

Status now also reports:

```text
Candidate sources cached
Runtime connections hooked
```

All v1.2.1 behavior remains:

- Discord-themed hover quick menu
- click-to-open non-modal editor
- outside-click dismiss
- per-app profile manager in plugin settings
- customizable RenoDX / ReShade profiles
- HWND -> PID executable resolution
