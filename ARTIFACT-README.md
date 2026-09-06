# DiscordHDRFix v1.2.2 Stream UI Fix

Install normally, then stream an application and open Discord's Go Live menu.

You should now always see:

```text
Discord HDR Fix  >
```

If Discord has not yet reported the selected source it will temporarily say:

```text
Detecting active stream…
```

This build uses both the v1.2.0 direct Go Live source hook and the v1.2.1 runtime
MediaEngine hook, so the row no longer depends on only one source-detection path.
