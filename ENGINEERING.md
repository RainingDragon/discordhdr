# Engineering notes — v0.5

Evidence used for this probe:

- Discord's RustEncoder has two frame submission paths:
  - `cc_encoder_add_frame_i420(...)` for CPU/I420 frames.
  - `cc_encoder_add_frame_native(...)` when a `CcFrameHandle` has a GPU frame and
    the selected encoder supports native GPU input.
- `WumpusFrameBuffer` exposes a `CcFrameHandle` and Discord checks
  `cc_video_frame_has_gpu_frame(...)` before choosing the native path.
- Discord's native capture profile separately carries:
  `useVideoHook`, `graphicsCaptureMaxApi`, stale-frame timeouts, and
  `hdrCaptureMode`.

v0.5 intentionally patches the imported C FFI symbol by name rather than using a
hard-coded RVA. If the Windows module imports the same symbol, this is much more
resilient to Discord updates.

Limitations:

- The public reverse-engineering dump was produced from Discord's Linux
  `discord_voice.node`, where debug symbols were available.
- The exact Windows import layout must therefore be verified empirically.
- No pixel mutation is attempted until the function ABI and lifetime are proven
  on the target Windows build.
