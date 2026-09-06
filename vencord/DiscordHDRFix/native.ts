import { execFile, spawn } from "child_process";
import { access, mkdir, readFile, readdir, rename, stat, writeFile } from "fs/promises";
import { constants } from "fs";
import { tmpdir } from "os";
import { join } from "path";
import { promisify } from "util";

const execFileAsync = promisify(execFile);

function installDir(): string {
    const localAppData = process.env.LOCALAPPDATA;
    if (!localAppData) throw new Error("LOCALAPPDATA is unavailable.");
    return join(localAppData, "DiscordHDRFix");
}

function injectorPath(): string { return join(installDir(), "DiscordHDRFix.Injector.exe"); }
function nativeDllPath(): string { return join(installDir(), "DiscordHDRFix.Native.dll"); }
function configPath(): string { return join(installDir(), "host.cfg"); }
function profilesPath(): string { return join(installDir(), "profiles.json"); }

async function checkNativeFiles() {
    try {
        await access(injectorPath(), constants.X_OK);
        await access(nativeDllPath(), constants.R_OK);
        return { ok: true, injector: injectorPath(), dll: nativeDllPath() };
    } catch (error) {
        return { ok: false, injector: injectorPath(), dll: nativeDllPath(), error: String(error) };
    }
}

export async function nativeFilesPresent(_event: Electron.IpcMainInvokeEvent) {
    return checkNativeFiles();
}

export async function writeHostConfig(
    _event: Electron.IpcMainInvokeEvent,
    enabled: boolean,
    traceEnabled: boolean,
    hostMode: string,
    sdrWhiteLevel: number,
    inputMaxLuminance: number,
    customPrimaries: string,
    customTransfer: string,
    customMetadata: string,
    rules: string
) {
    await mkdir(installDir(), { recursive: true });

    const white = Math.min(1000, Math.max(40, Number(sdrWhiteLevel) || 460));
    const peak = Math.min(10000, Math.max(100, Number(inputMaxLuminance) || 1000));

    const safeMode = [
        "observe", "rules", "force_sdr", "force_hdr10", "force_scrgb",
        "metadata_only", "custom"
    ].includes(hostMode) ? hostMode : "observe";

    const primariesMap: Record<string, number> = { preserve: -1, rec709: 0, rec2020: 1, arc: 2 };
    const transferMap: Record<string, number> = { preserve: -1, linear: 0, srgb: 1, pq: 2 };
    const metadataMap: Record<string, number> = { preserve: 0, none: 1, inject: 2 };

    const safeRules = String(rules ?? "").replace(/[\r\n]+/g, " ").trim();
    const text = [
        `enabled=${enabled ? 1 : 0}`,
        `trace=${traceEnabled ? 1 : 0}`,
        `mode=${safeMode}`,
        `sdr_white=${white}`,
        `input_max=${peak}`,
        `custom_primaries=${primariesMap[customPrimaries] ?? -1}`,
        `custom_transfer=${transferMap[customTransfer] ?? -1}`,
        `custom_metadata=${metadataMap[customMetadata] ?? 0}`,
        `rules=${safeRules}`,
        ""
    ].join("\n");

    const finalPath = configPath();
    const tempPath = `${finalPath}.tmp`;
    await writeFile(tempPath, text, "utf8");
    await rename(tempPath, finalPath);

    return { ok: true, path: finalPath };
}

export async function startNativeFix(_event: Electron.IpcMainInvokeEvent) {
    const files = await checkNativeFiles();
    if (!files.ok) return files;

    const child = spawn(
        files.injector!,
        ["--auto", "--dll", files.dll!, "--wait", "300"],
        { detached: true, windowsHide: true, stdio: "ignore" }
    );
    child.unref();

    return { ok: true, helperPid: child.pid, injector: files.injector, dll: files.dll };
}

export async function readNativeStatus(_event: Electron.IpcMainInvokeEvent) {
    const dir = tmpdir();
    const names = (await readdir(dir)).filter(name => /^DiscordHDRFix-devhost-\d+\.json$/i.test(name));
    if (names.length === 0) return null;

    const entries = await Promise.all(names.map(async name => {
        const path = join(dir, name);
        const info = await stat(path);
        return { path, mtimeMs: info.mtimeMs };
    }));
    entries.sort((a, b) => b.mtimeMs - a.mtimeMs);

    const raw = await readFile(entries[0].path, "utf8");
    try {
        return { statusFile: entries[0].path, ...JSON.parse(raw) };
    } catch {
        return { statusFile: entries[0].path, parseError: true, raw };
    }
}

export async function readProfiles(_event: Electron.IpcMainInvokeEvent) {
    await mkdir(installDir(), { recursive: true });
    try {
        const parsed = JSON.parse(await readFile(profilesPath(), "utf8"));
        if (!parsed || typeof parsed !== "object") throw new Error("profiles.json root is not an object");
        return parsed;
    } catch (error: any) {
        if (error?.code === "ENOENT") return { version: 1, apps: {} };
        return { version: 1, apps: {}, loadError: String(error) };
    }
}

export async function writeProfiles(_event: Electron.IpcMainInvokeEvent, database: unknown) {
    await mkdir(installDir(), { recursive: true });
    const finalPath = profilesPath();
    const tempPath = `${finalPath}.tmp`;
    await writeFile(tempPath, JSON.stringify(database, null, 2) + "\n", "utf8");
    await rename(tempPath, finalPath);
    return { ok: true, path: finalPath };
}

function psSingleQuote(value: string): string {
    return `'${value.replace(/'/g, "''")}'`;
}

export async function resolveStreamIdentity(
    _event: Electron.IpcMainInvokeEvent,
    sourcePid: number | null,
    sourceId: string | null,
    titleHint: string | null
) {
    const pid = Number.isFinite(Number(sourcePid)) ? Math.max(0, Math.trunc(Number(sourcePid))) : 0;
    const id = String(sourceId ?? "");
    const title = String(titleHint ?? "");

    // Runs only after Discord commits the actual Go Live source. It never enumerates apps.
    const script = `
$ErrorActionPreference = 'SilentlyContinue'
$resolvedPid = ${pid}
$sourceId = ${psSingleQuote(id)}

if (($resolvedPid -le 0) -and ($sourceId -match '^window:([^:]+):')) {
    $rawHandle = $matches[1]
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DiscordHDRFixWindowOwner {
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
}
'@ -ErrorAction SilentlyContinue

    [Int64]$hwndValue = 0
    if ($rawHandle.StartsWith('0x')) {
        $hwndValue = [Convert]::ToInt64($rawHandle.Substring(2), 16)
    } else {
        [Int64]::TryParse($rawHandle, [ref]$hwndValue) | Out-Null
    }

    if ($hwndValue -ne 0) {
        [uint32]$ownerPid = 0
        [DiscordHDRFixWindowOwner]::GetWindowThreadProcessId([IntPtr]$hwndValue, [ref]$ownerPid) | Out-Null
        if ($ownerPid -gt 0) { $resolvedPid = [int]$ownerPid }
    }
}

$result = [ordered]@{ pid = $resolvedPid; exeName = $null; exePath = $null; processName = $null }
if ($resolvedPid -gt 0) {
    $p = Get-Process -Id $resolvedPid -ErrorAction SilentlyContinue
    if ($p) {
        $result.processName = $p.ProcessName
        try { $result.exePath = $p.Path } catch {}
        if ($result.exePath) { $result.exeName = [IO.Path]::GetFileName($result.exePath) }
        elseif ($p.ProcessName) { $result.exeName = "$($p.ProcessName).exe" }
    }
}
$result | ConvertTo-Json -Compress
`;

    try {
        const { stdout } = await execFileAsync(
            "powershell.exe",
            ["-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script],
            { windowsHide: true, timeout: 5000, maxBuffer: 1024 * 1024 }
        );
        return { ok: true, sourcePid: pid || null, sourceId: id || null, titleHint: title || null, ...JSON.parse(String(stdout).trim()) };
    } catch (error) {
        return { ok: false, sourcePid: pid || null, sourceId: id || null, titleHint: title || null, error: String(error) };
    }
}
