# DiscordHDRFix v0.6.1

This build corrects the native HDR metadata layout used by the v0.6
Video Hook experiment.

The luminance settings remain runtime-editable through Vencord:

- SDR white level
- Input maximum luminance

The new native metadata state is fixed to the value (`1`) used by Discord's
own valid HDR metadata builder.

This target is intentionally locked to the analyzed discord_voice.node build.
If Discord updates the binary/signatures, the native patch refuses to install.
