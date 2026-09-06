# DiscordHDRFix v1.1.0 Dev Host — compiled artifact

## Install

Fully exit Discord:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\Install-DiscordHDRFix.ps1
```

If needed:

```powershell
.\Install-DiscordHDRFix.ps1 -VencordPath "C:\path\to\Vencord"
```

## First run

Leave:

```text
Mode: Observe only
Trace: ON
SDR white: 460
Input max: 1000
Rules: empty
```

Start a stream, wait several seconds, then:

```text
Vencord Toolbox
-> Show Native Dev Host Status
```

The status lists recent renderer caller RVAs.

After we identify a caller, you can add/change a rule in Vencord and press
`Apply Dev Host Settings`. The DLL reloads it in roughly 250 ms.

No rebuild or Discord restart is required.
