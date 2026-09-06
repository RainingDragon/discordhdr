DiscordHDRFix v1.2.1 DISCORD-STYLE UI
=========================================

New stream-menu UX:

  HOVER Discord HDR Fix
    -> quick profile flyout

  CLICK Discord HDR Fix
    -> full non-modal per-app editor

  CLICK ELSEWHERE IN DISCORD
    -> editor closes

No browser-native white <select> dropdowns are used in the stream menu.

RenoDX / ReShade is now customizable per application.

Vencord -> Plugins -> DiscordHDRFix also contains a full per-app profile manager.

Stream identity now accepts window:<HWND> source ids without requiring a second
colon, so profiles should resolve to real executables instead of raw window IDs.
