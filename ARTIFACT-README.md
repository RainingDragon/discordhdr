# DiscordHDRFix v1.3.0 — Renderer Compatibility

Builds a validated renderer signature scanner for the current Sep 28,
2026 Discord voice module. Retains the previous exact-build fallback.
If a future Discord update changes the renderer's ABI or metadata layout,
the DLL refuses to hook instead of guessing.

Install from the **compiled GitHub Actions artifact** with Discord exited.
The existing per-application profiles.json is preserved. To verify a stream,
open Vencord Toolbox -> Copy DiscordHDRFix Status and look for:

- hook_installed: true
- discovery_method: validated-abi-signature
- renderer_wrapper_rva: 0x5d2d30 (for the uploaded module)
- verified_renderer_callsites: 3
- total_modified_calls increasing on a corrected stream

Rendering changes have not been tested in a live Discord process yet.

---

# DiscordHDRFix v1.2.3 UI Polish

This build specifically fixes:

- HDR Fix button covering the Go Live menu
- black/dark text on dark Discord surfaces
- transparent hover/full-editor flyouts
- oversized full editor

Expected UX:

```text
Stop Streaming
Change Stream
Stream Quality
Discord HDR Fix   >
Share Stream Audio
Report Problem
```

Hover `Discord HDR Fix` for quick presets.
Click it for the 348 px non-modal advanced editor.
