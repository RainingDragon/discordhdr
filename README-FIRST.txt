DiscordHDRFix v1.3.0 RENDERER COMPATIBILITY
==========================================

This build restores native HDR correction for the Sep 28, 2026
Discord voice module, and introduces guarded renderer discovery.

The native DLL validates:
- unique 28-byte renderer ABI signature;
- Windows unwind function boundary;
- ninth HDR-metadata argument layout;
- downstream DXGI format and transfer/primaries fields;
- at least two validated renderer call sites.

Unknown or incompatible modules are rejected; the DLL never blindly
patches a previous version's RVA.

The old analyzed voice module remains supported via its exact build check.

No application profiles are deleted. Native HDR defaults stay 460/1000.
Dawnwalker / RenoDX settings remain per-application.

This is source code for GitHub Actions, not a compiled Windows DLL.
Build artifact: DiscordHDRFix-v1.3.0-compatibility-windows-x64

DiscordHDRFix v1.2.3 UI POLISH
=================================

Fixes the v1.2.2 visual regressions:

  - HDR Fix is constrained to one compact 40 px menu row
  - inserted next to Discord's actual Stream Quality / Change Stream item
  - hover/editor surfaces are forced opaque
  - text colors are copied from Discord's real menu with readable fallbacks
  - full editor reduced to 348 px
  - outside-click dismiss remains non-modal
