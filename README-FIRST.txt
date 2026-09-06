DiscordHDRFix v1.2.0 PER-APPLICATION PROFILES
===============================================

Every newly streamed application starts Automatic.

DiscordHDRFix logs ONLY applications you actually stream, not every application
Discord shows in the picker.

Automatic:
  format 28 -> SDR
  format 24 -> Native HDR10 (460 / 1000)
  format 10 -> scRGB (460 / 1000)
  unknown   -> preserve / observe

Per-app profiles:
  Automatic
  SDR
  Native HDR10
  scRGB
  RenoDX / ReShade
  Custom

While streaming, open Discord's normal Stream Settings / Change Windows popout.
A Discord HDR Fix section should appear there.

Profiles:
  %LOCALAPPDATA%\DiscordHDRFix\profiles.json
