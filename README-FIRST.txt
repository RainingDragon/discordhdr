DiscordHDRFix v1.1.0 DEV HOST
================================

This build combines the path tracer and hot-swappable HDR correction engine.

Install once. Then normal experiments do NOT require rebuilding.

Default:
  Mode = Observe only
  Trace = ON
  SDR white = 460
  Input max = 1000

Use:
  Vencord Toolbox -> Show Native Dev Host Status

Caller rules can be edited live in the plugin:

  callerRva,format|any,metadata(any|null|nonnull),action

Example:
  0x3fd467,24,null,hdr10

Actions:
  preserve
  sdr
  hdr10
  scrgb
  metadata

Changes reload in about 250 ms.
