DiscordHDRFix v1.0.1 AUTO TEST
================================

This is NOT the final friend/brother release yet.

Purpose:
  Validate that Discord's own WumpusFrame.is_source_hdr bit can automatically
  switch between:

  SDR -> no HDR tone mapping
  HDR -> Discord HDR tone mapping + correct source color interpretation

Install:
  1. Fully exit Discord.
  2. Run:
       Set-ExecutionPolicy -Scope Process Bypass
       .\Install-DiscordHDRFix.ps1
  3. Start Discord.
  4. Set DiscordHDRFix:
       Correction enabled = ON
       Detection mode = Automatic
       SDR white = 460
       Input max = 1000

Test:
  A. Native HDR game
  B. Slay the Spire 2 with Windows AutoHDR OFF

Use Vencord Toolbox -> Show Native HDR Fix Status after each test.
