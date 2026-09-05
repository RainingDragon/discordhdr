# DiscordHDRFix v0.6.1 engineering notes

Target discord_voice.node SHA-256:

54d452eefb5bd20f022dca1f93e3a92cf641e14283761720e4276f3fb0585db9

Relevant RVAs:

- Video Hook sets ninth renderer argument to null: 0x003fd443
- Video Hook CALL to shared renderer wrapper: 0x003fd462
- Return after that CALL: 0x003fd467
- Shared renderer wrapper: 0x0052cad0

Exact metadata consumption in the renderer:

- 0x0052d02b: reads float at metadata + 0x00
- 0x0052d246: reads float at metadata + 0x04
- 0x0052d2c2: reads byte  at metadata + 0x08

Discord's own builder at 0x005c3440:

- 0x005c34d5: writes float +0x00
- 0x005c34d9: writes float +0x04
- 0x005c34de: sets CL = 1
- 0x005c34e0: writes CL to byte +0x08

Its failure/fallback route initializes state to 2.

Therefore v0.6.1 explicitly supplies state=1 and 3 bytes of zero padding.

Patch strategy remains minimal:

1. Verify exact build and callsite signatures.
2. Replace only the Video Hook renderer CALL with a nearby relay.
3. Relay replaces only the ninth stack argument when enabled.
4. Tail-jump into Discord's untouched shared renderer wrapper.
5. Preserve Discord capture, audio, encoder, bitrate and Go Live pipeline.
