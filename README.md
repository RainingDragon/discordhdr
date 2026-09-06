# DiscordHDRFix v1.2.1 — Discord-style Stream UI

This revision keeps the v1.2 per-application profile system and replaces the
browser-native form that was appended to Discord's stream popout.

## Stream menu behavior

While streaming, Discord's normal Go Live menu gains one compact row:

```text
Discord HDR Fix          >
Automatic · 10-bit HDR
```

### Hover

Hovering the row opens a lightweight quick-profile flyout:

```text
Automatic
SDR
Native HDR10
scRGB
RenoDX / ReShade
Custom
```

This is intended for fast profile changes without opening the full editor.

### Click

Clicking the row opens a non-modal side editor. It does not install a page-sized
overlay and it does not lock Discord. Clicking anywhere else in Discord closes
the editor.

The editor contains:

- active streamed application
- detected format
- applied correction
- profile preset
- RenoDX/ReShade or Custom primaries
- transfer function
- HDR metadata policy
- SDR white
- input maximum
- reset to Automatic

The flyouts use Discord theme variables, floating-background/elevation tokens,
compact menu spacing and short scale/fade transitions instead of native HTML
`<select>` controls.

## RenoDX / ReShade

The RenoDX / ReShade preset is now **a customizable per-application profile**.

Its initial values remain:

```text
Rec.2020
sRGB
Inject metadata
360 / 200
```

Changing those fields does not turn the app into some unrelated global custom
mode and does not affect other games.

## Plugin settings

Vencord's normal DiscordHDRFix plugin settings now include a profile manager for
every application that has actually been streamed.

The profile manager uses Discord/Vencord Select, TextInput and Button
components, and can:

- select a previously streamed app
- change its profile
- customize RenoDX/ReShade or Custom settings
- reset it to Automatic
- forget its profile

## Stream identity fix

Discord sometimes supplies capture IDs in the form:

```text
window:37293266
```

rather than a form containing another trailing colon. The HWND -> PID resolver
now accepts both variants, so future profiles should resolve to the actual
executable rather than displaying the raw `window:...` source id.

Existing unresolved profiles are migrated when the same source later resolves
to its executable.
