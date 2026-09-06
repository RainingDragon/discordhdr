# DiscordHDRFix v1.0.1 automatic detection engineering notes

## Exact target

`discord_voice.node` SHA-256:

```text
54d452eefb5bd20f022dca1f93e3a92cf641e14283761720e4276f3fb0585db9
```

## Existing Video Hook callsite

```text
RVA 0x003fcdca: mov r12, rdx
...
RVA 0x003fd443: mov qword ptr [rsp+0x40], 0
RVA 0x003fd454: lea r9, [rsp+0x318]
RVA 0x003fd462: call renderer
RVA 0x003fd467: return address
```

Thus at the patched CALL relay entry:

```text
r12       = WumpusFrame*
r9        = renderer source descriptor
[rsp+48h] = ninth renderer arg (HDR metadata) after CALL pushed return address
```

## `is_source_hdr`

The WumpusFrame serde field set includes:

```text
cursor
surface
region
timestamp
capture_subtype
is_source_hdr
```

The final bool field resolves to:

```text
WumpusFrame + 0x1da
```

An independent consumer in the exact binary reads it at RVA `0x005ddaa1`:

```asm
movzx eax, byte ptr [r13+0x1da]
```

v1.0.1 verifies that exact instruction before patching.

## Direct assembly relay

Unlike v1.0.0's typed C++ renderer hook, v1.0.1 uses a small MASM leaf relay so
the original Video Hook `r12` register is available directly.

The relay:

1. Records `is_source_hdr`.
2. In Automatic mode, bypasses SDR completely.
3. Corrects only HDR frames.
4. Tail-jumps to Discord's existing renderer.
5. Does not change RSP and does not make another call.

No capture copy, CPU readback, extra encoder, mirror window, or audio changes.
