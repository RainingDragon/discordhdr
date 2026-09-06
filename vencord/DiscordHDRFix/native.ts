import { spawn } from "child_process";
import { access, mkdir, readFile, readdir, rename, stat, writeFile } from "fs/promises";
import { constants } from "fs";
import { tmpdir } from "os";
import { join } from "path";

function installDir(): string {
    const localAppData = process.env.LOCALAPPDATA;

    if (!localAppData)
        throw new Error("LOCALAPPDATA is unavailable.");

    return join(localAppData, "DiscordHDRFix");
}

function injectorPath(): string {
    return join(installDir(), "DiscordHDRFix.Injector.exe");
}

function nativeDllPath(): string {
    return join(installDir(), "DiscordHDRFix.Native.dll");
}

function configPath(): string {
    return join(installDir(), "host.cfg");
}

async function checkNativeFiles() {
    try {
        await access(injectorPath(), constants.X_OK);
        await access(nativeDllPath(), constants.R_OK);

        return {
            ok: true,
            injector: injectorPath(),
            dll: nativeDllPath()
        };
    } catch (error) {
        return {
            ok: false,
            injector: injectorPath(),
            dll: nativeDllPath(),
            error: String(error)
        };
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
    rules: string
) {
    await mkdir(installDir(), { recursive: true });

    const white = Math.min(
        1000,
        Math.max(40, Number(sdrWhiteLevel) || 460)
    );

    const peak = Math.min(
        10000,
        Math.max(100, Number(inputMaxLuminance) || 1000)
    );

    const safeMode = [
        "observe",
        "rules",
        "force_sdr",
        "force_hdr10",
        "force_scrgb",
        "metadata_only"
    ].includes(hostMode)
        ? hostMode
        : "observe";

    const safeRules = String(rules ?? "")
        .replace(/[\r\n]+/g, " ")
        .trim();

    const text = [
        `enabled=${enabled ? 1 : 0}`,
        `trace=${traceEnabled ? 1 : 0}`,
        `mode=${safeMode}`,
        `sdr_white=${white}`,
        `input_max=${peak}`,
        `rules=${safeRules}`,
        ""
    ].join("\n");

    const finalPath = configPath();
    const tempPath = `${finalPath}.tmp`;

    await writeFile(tempPath, text, "utf8");
    await rename(tempPath, finalPath);

    return {
        ok: true,
        path: finalPath,
        enabled,
        traceEnabled,
        hostMode: safeMode,
        sdrWhiteLevel: white,
        inputMaxLuminance: peak,
        rules: safeRules
    };
}

export async function startNativeFix(_event: Electron.IpcMainInvokeEvent) {
    const files = await checkNativeFiles();

    if (!files.ok)
        return files;

    const child = spawn(
        files.injector!,
        [
            "--auto",
            "--dll", files.dll!,
            "--wait", "300"
        ],
        {
            detached: true,
            windowsHide: true,
            stdio: "ignore"
        }
    );

    child.unref();

    return {
        ok: true,
        helperPid: child.pid,
        injector: files.injector,
        dll: files.dll
    };
}

export async function readNativeStatus(_event: Electron.IpcMainInvokeEvent) {
    const dir = tmpdir();

    const names = (await readdir(dir))
        .filter(name => /^DiscordHDRFix-devhost-\d+\.json$/i.test(name));

    if (names.length === 0)
        return null;

    const entries = await Promise.all(
        names.map(async name => {
            const path = join(dir, name);
            const info = await stat(path);

            return {
                name,
                path,
                mtimeMs: info.mtimeMs
            };
        })
    );

    entries.sort((a, b) => b.mtimeMs - a.mtimeMs);

    const newest = entries[0];
    const raw = await readFile(newest.path, "utf8");

    try {
        return {
            statusFile: newest.path,
            ...JSON.parse(raw)
        };
    } catch {
        return {
            statusFile: newest.path,
            parseError: true,
            raw
        };
    }
}
