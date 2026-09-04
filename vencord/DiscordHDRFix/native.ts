/*
 * Vencord native helper for DiscordHDRFix v0.5.
 *
 * Runs in Discord's Electron main process. It only launches the small
 * injector and reads the probe status written into the user's temp folder.
 */

import { spawn } from "child_process";
import { access, readFile, readdir, stat } from "fs/promises";
import { constants } from "fs";
import { tmpdir } from "os";
import { join } from "path";

function installDir(): string {
    const localAppData = process.env.LOCALAPPDATA;
    if (!localAppData)
        throw new Error("LOCALAPPDATA is not available.");

    return join(localAppData, "DiscordHDRFix");
}

function injectorPath(): string {
    return join(installDir(), "DiscordHDRFix.Injector.exe");
}

function probeDllPath(): string {
    return join(installDir(), "DiscordHDRFix.Native.dll");
}

async function checkNativeFiles() {
    try {
        await access(injectorPath(), constants.X_OK);
        await access(probeDllPath(), constants.R_OK);
        return {
            ok: true,
            injector: injectorPath(),
            dll: probeDllPath()
        };
    } catch (error) {
        return {
            ok: false,
            injector: injectorPath(),
            dll: probeDllPath(),
            error: String(error)
        };
    }
}

export async function nativeFilesPresent(_event: Electron.IpcMainInvokeEvent) {
    return checkNativeFiles();
}

export async function startProbe(_event: Electron.IpcMainInvokeEvent) {
    const files = await checkNativeFiles();
    if (!files.ok)
        return files;

    // Detached helper: it waits for a Discord.exe process that has
    // discord_voice.node loaded, injects the pass-through DLL, then exits.
    const child = spawn(files.injector!, [
        "--auto",
        "--dll", files.dll!,
        "--wait", "300"
    ], {
        detached: true,
        windowsHide: true,
        stdio: "ignore"
    });

    child.unref();

    return {
        ok: true,
        helperPid: child.pid,
        injector: files.injector,
        dll: files.dll
    };
}

export async function readProbeStatus(_event: Electron.IpcMainInvokeEvent) {
    const dir = tmpdir();
    const names = (await readdir(dir))
        .filter(name => /^DiscordHDRFix-probe-\d+\.json$/i.test(name));

    if (names.length === 0)
        return null;

    const entries = await Promise.all(names.map(async name => {
        const path = join(dir, name);
        const info = await stat(path);
        return { name, path, mtimeMs: info.mtimeMs };
    }));

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
