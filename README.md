# DiscordHDRFix v0.5 — GitHub cloud build

This repository is configured so **GitHub Actions compiles the Windows native
probe for you**. You do not need Visual Studio, CMake, MSVC, or Windows Build
Tools installed locally.

## Fastest way to build

1. Create a new GitHub repository.
2. Put the contents of this folder at the repository root.
3. Commit/push the files to `main`.
4. Open the repository's **Actions** tab.
5. Open **Build DiscordHDRFix Windows x64**.
6. If a run did not start automatically, click **Run workflow**.
7. When it finishes, open the run and download the artifact:

```text
DiscordHDRFix-v0.5-windows-x64
```

The artifact contains the compiled:

```text
DiscordHDRFix.Injector.exe
DiscordHDRFix.Native.dll
```

plus the matching Vencord plugin and an install script.

## Why this build is still a probe

v0.4 established that forcing Discord to D3D11 Video Hook avoids the blown-out
Graphics Capture output, but Video Hook is dull/gray and reports zero HDR frames.

v0.5 validates the native GPU frame seam immediately before Discord's hardware
encoder. It hooks `cc_encoder_add_frame_native(...)` in pass-through mode and
counts frames. It does **not** alter the frame yet.

If the status reports:

```text
hook_installed = true
frames_seen = increasing
```

then the next build can insert the D3D11 HDR→SDR tone-map pass at that seam.
