# DiscordHDRFix v1.2.3 — Stream UI Polish

This revision fixes the layout/styling problems visible in v1.2.2.

## Fixes

### Compact stream-menu row

The Discord HDR Fix button is now hard-limited to a single 40 px menu row.

Instead of appending into an arbitrary popout container, the plugin now finds
Discord's existing `Stream Quality` / `Change Stream` item and inserts the HDR
Fix row beside those real menu items.

This prevents the button from stretching over or covering the rest of the Go
Live menu.

### Opaque flyouts

The hover quick menu and click editor no longer depend only on Discord CSS
variables after being moved under `document.body`.

At runtime the plugin copies the actual computed background and text color from
Discord's existing stream menu and applies those values directly to the HDR Fix
flyouts.

Fallbacks are also explicitly opaque:

```text
surface: #111214
text:    #dbdee1
muted:   #949ba4
```

so a missing/transparent Discord variable cannot produce a transparent editor
or black-on-black text.

### Smaller full editor

The click editor is reduced to 348 px wide and stays non-modal. It still closes
on outside click and does not install a blocking backdrop.

All existing per-app, Automatic, RenoDX/ReShade and plugin-settings behavior is
unchanged.
