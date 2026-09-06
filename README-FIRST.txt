DiscordHDRFix v1.2.2 STREAM UI FIX
====================================

Fix:
  v1.2.1 could omit the Discord HDR Fix row if its runtime source hook did not
  resolve the active stream.

v1.2.2:
  - restores the proven direct setGoLiveSource hook
  - keeps the runtime MediaEngine hook as fallback
  - always shows the row when Discord's active stream menu is present
  - displays "Detecting active stream…" until identity resolves
