import { definePluginSettings } from "@api/Settings";
import { Logger } from "@utils/Logger";
import definePlugin, { OptionType, PluginNative } from "@utils/types";
import { MediaEngineStore } from "@webpack/common";

const logger = new Logger("DiscordHDRFix");
const Native = VencordNative.pluginHelpers.DiscordHDRFix as PluginNative<typeof import("./native")>;

type ProfileKind = "auto" | "sdr" | "hdr10" | "scrgb" | "renodx" | "custom";
type Primaries = "preserve" | "rec709" | "rec2020" | "arc";
type Transfer = "preserve" | "linear" | "srgb" | "pq";
type MetadataPolicy = "preserve" | "none" | "inject";

interface CustomValues {
    primaries: Primaries;
    transfer: Transfer;
    metadata: MetadataPolicy;
    sdrWhite: number;
    inputMax: number;
}

interface AppProfile {
    key: string;
    exeName: string | null;
    exePath: string | null;
    displayName: string;
    firstSeen: string;
    lastSeen: string;
    lastSourceId: string | null;
    kind: ProfileKind;
    custom: CustomValues;
}

interface ProfileDatabase {
    version: 1;
    apps: Record<string, AppProfile>;
    loadError?: string;
}

interface CandidateSource {
    id: string;
    sourcePid: number | null;
    title: string | null;
    seenAt: number;
}

interface ActiveStream {
    sourceId: string;
    sourcePid: number | null;
    profileKey: string;
    displayName: string;
    exeName: string | null;
    exePath: string | null;
}

interface AppliedConfig {
    mode: string;
    white: number;
    peak: number;
    primaries: Primaries;
    transfer: Transfer;
    metadata: MetadataPolicy;
    reason: string;
}

const captureState = {
    patchCalls: 0,
    lastSeenAt: null as number | null,
    originalHdr: undefined as unknown,
    effectiveHdr: undefined as unknown,
    originalGraphicsCapture: undefined as unknown,
    effectiveGraphicsCapture: undefined as unknown,
    originalGraphicsApi: undefined as unknown,
    effectiveGraphicsApi: undefined as unknown,
    originalVideoHook: undefined as unknown,
    effectiveVideoHook: undefined as unknown
};

const candidateSources = new Map<string, CandidateSource>();
let profiles: ProfileDatabase = { version: 1, apps: {} };
let activeStream: ActiveStream | null = null;
let nativeStatus: any = null;
let lastAppliedFingerprint = "";
let statusPollTimer: number | null = null;
let popoutTimer: number | null = null;
let connectionHookTimer: number | null = null;
let sourceActivationSerial = 0;
let rendererCallsAtActivation = -1;

const hookedConnections = new Map<any, { setGoLiveSource?: Function; clearDesktopSource?: Function; }>();

const DEFAULT_CUSTOM: CustomValues = {
    primaries: "rec2020",
    transfer: "srgb",
    metadata: "inject",
    sdrWhite: 360,
    inputMax: 200
};

const settings = definePluginSettings({
    enabled: {
        type: OptionType.BOOLEAN,
        description: "Enable DiscordHDRFix. Every newly streamed application starts in Automatic mode.",
        default: true,
        restartNeeded: false,
        onChange: () => void applyActiveProfile(true)
    },
    traceEnabled: {
        type: OptionType.BOOLEAN,
        description: "Keep shared-renderer tracing on for diagnostics and automatic classification.",
        default: true,
        restartNeeded: false,
        onChange: () => void applyActiveProfile(true)
    },
    showStreamPopoutControls: {
        type: OptionType.BOOLEAN,
        description: "Show DiscordHDRFix controls inside Discord's active stream settings / Change Windows popout.",
        default: true,
        restartNeeded: false
    }
});

function clampWhite(value: unknown, fallback = 460): number {
    const number = Number(value);
    return Number.isFinite(number) ? Math.min(1000, Math.max(40, number)) : fallback;
}

function clampPeak(value: unknown, fallback = 1000): number {
    const number = Number(value);
    return Number.isFinite(number) ? Math.min(10000, Math.max(100, number)) : fallback;
}

function nowIso(): string { return new Date().toISOString(); }

function cleanDisplayName(value: unknown): string | null {
    const text = String(value ?? "").trim();
    return text ? text.slice(0, 180) : null;
}

function stableFallbackKey(title: string | null, sourceId: string): string {
    const base = (title || sourceId || "unknown-stream")
        .toLowerCase()
        .replace(/[^a-z0-9._-]+/g, "-")
        .replace(/^-+|-+$/g, "")
        .slice(0, 120);
    return `stream:${base || "unknown"}`;
}

function defaultProfile(key: string, displayName: string, exeName: string | null, exePath: string | null, sourceId: string): AppProfile {
    const now = nowIso();
    return {
        key, exeName, exePath, displayName,
        firstSeen: now, lastSeen: now, lastSourceId: sourceId || null,
        kind: "auto",
        custom: { ...DEFAULT_CUSTOM }
    };
}

async function loadProfiles(): Promise<void> {
    try {
        const loaded = await Native.readProfiles();
        if (loaded && typeof loaded === "object" && loaded.apps) {
            profiles = { version: 1, apps: loaded.apps, loadError: loaded.loadError };
        }
    } catch (error) {
        logger.error("Failed to load per-application profiles:", error);
    }
}

async function saveProfiles(): Promise<void> {
    try { await Native.writeProfiles(profiles); }
    catch (error) { logger.error("Failed to save per-application profiles:", error); }
}

function getActiveProfile(): AppProfile | null {
    return activeStream ? profiles.apps[activeStream.profileKey] ?? null : null;
}

function describeFormat(status: any): string {
    if (!status) return "waiting for renderer";
    const format = Number(status.last_format);
    if (format === 24) return "10-bit R10G10B10A2";
    if (format === 10) return "FP16 R16G16B16A16";
    if (format === 28) return "8-bit R8G8B8A8";
    if (Number.isFinite(format) && format >= 0) return `DXGI format ${format}`;
    return "not observed";
}

function automaticConfig(status: any): AppliedConfig {
    const rendererCalls = Number(status?.total_renderer_calls);

    if (activeStream && rendererCallsAtActivation >= 0 &&
        (!Number.isFinite(rendererCalls) || rendererCalls <= rendererCallsAtActivation)) {
        return {
            mode: "observe", white: 460, peak: 1000,
            primaries: "preserve", transfer: "preserve", metadata: "preserve",
            reason: "Automatic: waiting for frames from the newly selected application"
        };
    }

    const format = Number(status?.last_format);

    if (format === 28) return {
        mode: "force_sdr", white: 460, peak: 1000,
        primaries: "preserve", transfer: "preserve", metadata: "none",
        reason: "Automatic: 8-bit source -> SDR bypass"
    };

    if (format === 24) return {
        mode: "force_hdr10", white: 460, peak: 1000,
        primaries: "rec2020", transfer: "pq", metadata: "inject",
        reason: "Automatic: 10-bit source -> native HDR10 default"
    };

    if (format === 10) return {
        mode: "force_scrgb", white: 460, peak: 1000,
        primaries: "rec709", transfer: "linear", metadata: "inject",
        reason: "Automatic: FP16 source -> scRGB default"
    };

    return {
        mode: "observe", white: 460, peak: 1000,
        primaries: "preserve", transfer: "preserve", metadata: "preserve",
        reason: "Automatic: source not classified yet"
    };
}

function profileConfig(profile: AppProfile, status: any): AppliedConfig {
    switch (profile.kind) {
    case "auto": return automaticConfig(status);
    case "sdr": return {
        mode: "force_sdr", white: 460, peak: 1000,
        primaries: "preserve", transfer: "preserve", metadata: "none",
        reason: "Per-app override: SDR"
    };
    case "hdr10": return {
        mode: "force_hdr10", white: 460, peak: 1000,
        primaries: "rec2020", transfer: "pq", metadata: "inject",
        reason: "Per-app override: Native HDR10"
    };
    case "scrgb": return {
        mode: "force_scrgb", white: 460, peak: 1000,
        primaries: "rec709", transfer: "linear", metadata: "inject",
        reason: "Per-app override: scRGB"
    };
    case "renodx": return {
        mode: "custom", white: 360, peak: 200,
        primaries: "rec2020", transfer: "srgb", metadata: "inject",
        reason: "Per-app override: RenoDX / ReShade"
    };
    case "custom":
    default:
        return {
            mode: "custom",
            white: clampWhite(profile.custom?.sdrWhite, 460),
            peak: clampPeak(profile.custom?.inputMax, 1000),
            primaries: profile.custom?.primaries ?? "preserve",
            transfer: profile.custom?.transfer ?? "preserve",
            metadata: profile.custom?.metadata ?? "preserve",
            reason: "Per-app override: Custom"
        };
    }
}

async function applyConfig(config: AppliedConfig, force = false): Promise<void> {
    const fingerprint = JSON.stringify({ enabled: settings.store.enabled, trace: settings.store.traceEnabled, ...config });
    if (!force && fingerprint === lastAppliedFingerprint) return;
    lastAppliedFingerprint = fingerprint;

    try {
        await Native.writeHostConfig(
            settings.store.enabled,
            settings.store.traceEnabled,
            config.mode,
            config.white,
            config.peak,
            config.primaries,
            config.transfer,
            config.metadata,
            ""
        );
    } catch (error) {
        logger.error("Failed to apply active application profile:", error);
    }
}

async function applyActiveProfile(force = false): Promise<void> {
    if (!settings.store.enabled) {
        await applyConfig({
            mode: "observe", white: 460, peak: 1000,
            primaries: "preserve", transfer: "preserve", metadata: "preserve",
            reason: "DiscordHDRFix disabled"
        }, force);
        return;
    }

    const profile = getActiveProfile();
    if (!profile) {
        await applyConfig({
            mode: "observe", white: 460, peak: 1000,
            primaries: "preserve", transfer: "preserve", metadata: "preserve",
            reason: "No active streamed application"
        }, force);
        return;
    }

    await applyConfig(profileConfig(profile, nativeStatus), force);
}

async function startNativeHost(): Promise<void> {
    try {
        const result = await Native.startNativeFix();
        if (!result?.ok) logger.error("Native dev host launch failed:", result);
    } catch (error) {
        logger.error("Failed to launch native dev host:", error);
    }
}

function sourceTitle(source: any): string | null {
    return cleanDisplayName(source?.name ?? source?.title ?? source?.applicationName ?? source?.windowTitle ?? null);
}

function sourcePid(source: any): number | null {
    const value = Number(source?.sourcePid ?? source?.pid ?? source?.processId ?? 0);
    return Number.isFinite(value) && value > 0 ? Math.trunc(value) : null;
}

// Candidate metadata may be observed while Discord is only showing available sources.
// It is cached in RAM and NEVER persisted here.
function cacheCandidateSource(source: any): any {
    const id = String(source?.id ?? "");
    if (!id) return source;

    candidateSources.set(id, { id, sourcePid: sourcePid(source), title: sourceTitle(source), seenAt: Date.now() });

    if (candidateSources.size > 256) {
        const cutoff = Date.now() - 5 * 60_000;
        for (const [key, value] of candidateSources) {
            if (value.seenAt < cutoff) candidateSources.delete(key);
        }
    }

    return source;
}

async function activateActualStream(sourceId: string, directPid: number | null = null, directTitle: string | null = null): Promise<void> {
    if (!sourceId) return;

    const serial = ++sourceActivationSerial;
    const candidate = candidateSources.get(sourceId);
    const pid = directPid ?? candidate?.sourcePid ?? null;
    const title = directTitle ?? candidate?.title ?? null;

    let identity: any = null;
    try { identity = await Native.resolveStreamIdentity(pid, sourceId, title); }
    catch (error) { logger.warn("Could not resolve streamed process identity:", error); }

    if (serial !== sourceActivationSerial) return;

    const exeName = cleanDisplayName(identity?.exeName);
    const exePath = cleanDisplayName(identity?.exePath);
    const key = exePath
        ? `path:${exePath.toLowerCase()}`
        : (exeName ? `exe:${exeName.toLowerCase()}` : stableFallbackKey(title, sourceId));
    const displayName = title ?? exeName ?? cleanDisplayName(identity?.processName) ?? sourceId;

    const profile = profiles.apps[key] ?? defaultProfile(key, displayName, exeName, exePath, sourceId);
    profile.exeName = exeName ?? profile.exeName;
    profile.exePath = exePath ?? profile.exePath;
    profile.displayName = displayName || profile.displayName;
    profile.lastSeen = nowIso();
    profile.lastSourceId = sourceId;
    profiles.apps[key] = profile;

    rendererCallsAtActivation = Number(nativeStatus?.total_renderer_calls);
    if (!Number.isFinite(rendererCallsAtActivation)) rendererCallsAtActivation = -1;

    activeStream = {
        sourceId,
        sourcePid: Number(identity?.pid) > 0 ? Number(identity.pid) : pid,
        profileKey: key,
        displayName: profile.displayName,
        exeName: profile.exeName,
        exePath: profile.exePath
    };

    // ONLY confirmed Go Live sources are persisted.
    await saveProfiles();
    lastAppliedFingerprint = "";
    await applyActiveProfile(true);
    refreshStreamPopoutPanel();
}

function confirmGoLiveOptions(options: any): any {
    const description = options?.desktopDescription ?? options?.desktopSource ?? null;
    const id = String(description?.id ?? "");
    if (id) void activateActualStream(id, sourcePid(description), sourceTitle(description));
    return options;
}

function streamEnded(): void {
    activeStream = null;
    rendererCallsAtActivation = -1;
    sourceActivationSerial++;
    lastAppliedFingerprint = "";
    void applyActiveProfile(true);
    removeInjectedPanels();
}

function hookMediaConnections(): void {
    let engine: any;

    try {
        engine = MediaEngineStore.getMediaEngine();
    } catch {
        return;
    }

    const connections = engine?.connections;
    if (!connections || typeof connections[Symbol.iterator] !== "function") return;

    for (const connection of connections as Iterable<any>) {
        if (!connection || hookedConnections.has(connection)) continue;

        const originals: { setGoLiveSource?: Function; clearDesktopSource?: Function; } = {};

        if (typeof connection.setGoLiveSource === "function") {
            const original = connection.setGoLiveSource;
            originals.setGoLiveSource = original;

            connection.setGoLiveSource = function (options: any) {
                confirmGoLiveOptions(options);
                return original.call(this, options);
            };
        }

        if (typeof connection.clearDesktopSource === "function") {
            const original = connection.clearDesktopSource;
            originals.clearDesktopSource = original;

            connection.clearDesktopSource = function (...args: any[]) {
                streamEnded();
                return original.apply(this, args);
            };
        }

        hookedConnections.set(connection, originals);
    }
}

function unhookMediaConnections(): void {
    for (const [connection, originals] of hookedConnections) {
        try {
            if (originals.setGoLiveSource) connection.setGoLiveSource = originals.setGoLiveSource;
            if (originals.clearDesktopSource) connection.clearDesktopSource = originals.clearDesktopSource;
        } catch {}
    }

    hookedConnections.clear();
}

async function updateActiveProfile(patch: Partial<AppProfile>): Promise<void> {
    const profile = getActiveProfile();
    if (!profile) return;
    Object.assign(profile, patch);
    profile.lastSeen = nowIso();
    profiles.apps[profile.key] = profile;
    await saveProfiles();
    lastAppliedFingerprint = "";
    await applyActiveProfile(true);
    refreshStreamPopoutPanel();
}

async function updateActiveCustom(patch: Partial<CustomValues>): Promise<void> {
    const profile = getActiveProfile();
    if (!profile) return;
    profile.kind = "custom";
    profile.custom = { ...DEFAULT_CUSTOM, ...profile.custom, ...patch };
    profile.lastSeen = nowIso();
    profiles.apps[profile.key] = profile;
    await saveProfiles();
    lastAppliedFingerprint = "";
    await applyActiveProfile(true);
    refreshStreamPopoutPanel();
}

function profileKindLabel(kind: ProfileKind): string {
    return ({ auto: "Automatic", sdr: "SDR", hdr10: "Native HDR10", scrgb: "scRGB", renodx: "RenoDX / ReShade", custom: "Custom" } as Record<ProfileKind, string>)[kind];
}

function escapeHtml(value: unknown): string {
    return String(value ?? "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/\"/g, "&quot;");
}

function option(value: string, label: string, selected: string): string {
    return `<option value="${value}"${value === selected ? " selected" : ""}>${escapeHtml(label)}</option>`;
}

function makeSelect(label: string, id: string, selected: string, values: Array<[string, string]>): string {
    return `
        <label style="display:block;margin-top:8px;font-size:12px;color:var(--text-muted);">${escapeHtml(label)}</label>
        <select data-hdrfix-control="${id}" style="width:100%;margin-top:4px;padding:7px;border-radius:4px;background:var(--input-background);color:var(--text-normal);border:1px solid var(--background-modifier-accent);">
            ${values.map(([value, text]) => option(value, text, selected)).join("")}
        </select>`;
}

function streamPanelHtml(profile: AppProfile): string {
    const config = profileConfig(profile, nativeStatus);
    const profileSelect = makeSelect("Profile", "kind", profile.kind, [
        ["auto", "Automatic"], ["sdr", "SDR"], ["hdr10", "Native HDR10"],
        ["scrgb", "scRGB"], ["renodx", "RenoDX / ReShade"], ["custom", "Custom"]
    ]);

    const custom = profile.kind === "custom" ? `
        ${makeSelect("Primaries", "primaries", profile.custom.primaries, [["preserve", "Preserve"], ["rec709", "Rec.709"], ["rec2020", "Rec.2020"], ["arc", "Arc"]])}
        ${makeSelect("Transfer", "transfer", profile.custom.transfer, [["preserve", "Preserve"], ["linear", "Linear"], ["srgb", "sRGB"], ["pq", "ST.2084 / PQ"]])}
        ${makeSelect("HDR metadata", "metadata", profile.custom.metadata, [["preserve", "Preserve Discord metadata"], ["none", "No HDR metadata"], ["inject", "Inject metadata"]])}
        <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px;">
            <label style="font-size:12px;color:var(--text-muted);">SDR white
                <input data-hdrfix-control="white" type="number" min="40" max="1000" step="1" value="${profile.custom.sdrWhite}" style="box-sizing:border-box;width:100%;margin-top:4px;padding:7px;border-radius:4px;background:var(--input-background);color:var(--text-normal);border:1px solid var(--background-modifier-accent);" />
            </label>
            <label style="font-size:12px;color:var(--text-muted);">Input max
                <input data-hdrfix-control="peak" type="number" min="100" max="10000" step="1" value="${profile.custom.inputMax}" style="box-sizing:border-box;width:100%;margin-top:4px;padding:7px;border-radius:4px;background:var(--input-background);color:var(--text-normal);border:1px solid var(--background-modifier-accent);" />
            </label>
        </div>` : "";

    return `
        <div style="font-weight:600;font-size:14px;margin-bottom:3px;">Discord HDR Fix</div>
        <div style="font-size:12px;color:var(--text-muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${escapeHtml(profile.displayName)}${profile.exeName ? ` · ${escapeHtml(profile.exeName)}` : ""}</div>
        ${profileSelect}
        <div style="margin-top:8px;padding:7px;border-radius:4px;background:var(--background-secondary);font-size:11px;line-height:1.35;color:var(--text-muted);">
            <div><b>Detected:</b> ${escapeHtml(describeFormat(nativeStatus))}</div>
            <div><b>Applied:</b> ${escapeHtml(config.reason)}</div>
        </div>
        ${custom}
        <button data-hdrfix-control="reset" style="width:100%;margin-top:8px;padding:7px;border:0;border-radius:4px;background:var(--button-secondary-background);color:var(--text-normal);cursor:pointer;">Reset this application to Automatic</button>`;
}

function bindPanelEvents(panel: HTMLElement): void {
    const profile = getActiveProfile();
    if (!profile) return;

    panel.querySelector<HTMLSelectElement>('[data-hdrfix-control="kind"]')?.addEventListener("change", event => {
        void updateActiveProfile({ kind: (event.currentTarget as HTMLSelectElement).value as ProfileKind });
    });
    panel.querySelector<HTMLSelectElement>('[data-hdrfix-control="primaries"]')?.addEventListener("change", event => {
        void updateActiveCustom({ primaries: (event.currentTarget as HTMLSelectElement).value as Primaries });
    });
    panel.querySelector<HTMLSelectElement>('[data-hdrfix-control="transfer"]')?.addEventListener("change", event => {
        void updateActiveCustom({ transfer: (event.currentTarget as HTMLSelectElement).value as Transfer });
    });
    panel.querySelector<HTMLSelectElement>('[data-hdrfix-control="metadata"]')?.addEventListener("change", event => {
        void updateActiveCustom({ metadata: (event.currentTarget as HTMLSelectElement).value as MetadataPolicy });
    });
    panel.querySelector<HTMLInputElement>('[data-hdrfix-control="white"]')?.addEventListener("change", event => {
        void updateActiveCustom({ sdrWhite: clampWhite((event.currentTarget as HTMLInputElement).value, profile.custom.sdrWhite) });
    });
    panel.querySelector<HTMLInputElement>('[data-hdrfix-control="peak"]')?.addEventListener("change", event => {
        void updateActiveCustom({ inputMax: clampPeak((event.currentTarget as HTMLInputElement).value, profile.custom.inputMax) });
    });
    panel.querySelector<HTMLButtonElement>('[data-hdrfix-control="reset"]')?.addEventListener("click", () => {
        void updateActiveProfile({ kind: "auto" });
    });
}

function isVisible(element: HTMLElement): boolean {
    const style = getComputedStyle(element);
    if (style.display === "none" || style.visibility === "hidden") return false;
    const rect = element.getBoundingClientRect();
    return rect.width > 180 && rect.height > 80 && rect.width < 1000 && rect.height < 1400;
}

function findStreamSettingsContainer(): HTMLElement | null {
    const terms = ["change windows", "change window", "change application", "change applications", "stream quality", "stream settings"];
    const candidates = Array.from(document.querySelectorAll<HTMLElement>('[role="dialog"], [role="menu"], [class*="popout"], [class*="Popout"], [class*="modal"], [class*="Modal"]'))
        .filter(element => {
            if (!isVisible(element)) return false;
            const text = (element.innerText || "").toLowerCase().slice(0, 6000);
            return terms.some(term => text.includes(term));
        });

    candidates.sort((a, b) => {
        const ar = a.getBoundingClientRect();
        const br = b.getBoundingClientRect();
        return ar.width * ar.height - br.width * br.height;
    });
    return candidates[0] ?? null;
}

function removeInjectedPanels(): void {
    document.querySelectorAll<HTMLElement>('[data-discord-hdr-fix-profile-panel="1"]').forEach(element => element.remove());
}

function refreshStreamPopoutPanel(): void {
    if (!settings.store.showStreamPopoutControls) { removeInjectedPanels(); return; }
    const profile = getActiveProfile();
    if (!profile) { removeInjectedPanels(); return; }
    const target = findStreamSettingsContainer();
    if (!target) return;

    let panel = target.querySelector<HTMLElement>(':scope > [data-discord-hdr-fix-profile-panel="1"]');
    if (!panel) {
        panel = document.createElement("div");
        panel.dataset.discordHdrFixProfilePanel = "1";
        panel.style.cssText = "margin:10px 8px;padding:10px;border-radius:8px;background:var(--background-primary);border:1px solid var(--background-modifier-accent);color:var(--text-normal)";
        target.appendChild(panel);
    }

    // Do not tear down a focused select/input while the user is editing it.
    if (panel.contains(document.activeElement)) return;

    const renderKey = JSON.stringify({
        key: profile.key,
        kind: profile.kind,
        custom: profile.custom,
        format: nativeStatus?.last_format,
        calls: nativeStatus?.total_renderer_calls,
        action: nativeStatus?.last_action
    });

    if (panel.dataset.discordHdrFixRenderKey !== renderKey) {
        panel.innerHTML = streamPanelHtml(profile);
        panel.dataset.discordHdrFixRenderKey = renderKey;
        bindPanelEvents(panel);
    }
}

async function pollNativeStatus(): Promise<void> {
    try { nativeStatus = await Native.readNativeStatus(); }
    catch { nativeStatus = null; }
    if (getActiveProfile()?.kind === "auto") await applyActiveProfile();
    refreshStreamPopoutPanel();
}

function fmt(value: unknown): string {
    if (value === undefined) return "<not observed>";
    return typeof value === "string" ? JSON.stringify(value) : String(value);
}

function combinedStatus(): string {
    const profile = getActiveProfile();
    const config = profile ? profileConfig(profile, nativeStatus) : null;
    return [
        "DiscordHDRFix v1.2.0 per-application profiles",
        "-----------------------------------------------",
        `Active stream:                 ${activeStream?.displayName ?? "<none>"}`,
        `Active executable:             ${activeStream?.exeName ?? "<unresolved>"}`,
        `Profile key:                   ${activeStream?.profileKey ?? "<none>"}`,
        `Profile:                       ${profile ? profileKindLabel(profile.kind) : "<none>"}`,
        `Detected format:               ${describeFormat(nativeStatus)}`,
        `Applied mode:                  ${config?.mode ?? "observe"}`,
        `Applied reason:                ${config?.reason ?? "No active stream"}`,
        `Logged streamed applications:  ${Object.keys(profiles.apps).length}`,
        "",
        "Capture routing",
        `HDR:                           ${fmt(captureState.originalHdr)} -> ${fmt(captureState.effectiveHdr)}`,
        `Graphics Capture:              ${fmt(captureState.originalGraphicsCapture)} -> ${fmt(captureState.effectiveGraphicsCapture)}`,
        `Graphics Capture API:          ${fmt(captureState.originalGraphicsApi)} -> ${fmt(captureState.effectiveGraphicsApi)}`,
        `Video Hook:                    ${fmt(captureState.originalVideoHook)} -> ${fmt(captureState.effectiveVideoHook)}`,
        "",
        "Native",
        JSON.stringify(nativeStatus, null, 2)
    ].join("\n");
}

export default definePlugin({
    name: "DiscordHDRFix",
    description: "Corrects Discord Video Hook HDR capture with automatic and per-streamed-application profiles.",
    authors: [{ name: "Discord HDR Fix", id: 0n }],
    tags: ["Developers", "Voice"],
    settings,

    patches: [
        {
            find: "graphicsCaptureStaleFrameTimeoutMs",
            all: true,
            replacement: [
                { match: /hdrCaptureMode:([A-Za-z_$][\w$]*)/g, replace: "hdrCaptureMode:$self.forceSdrMode($1)" },
                { match: /useGraphicsCapture:([A-Za-z_$][\w$]*(?:\(\))?|![01])/g, replace: "useGraphicsCapture:$self.forceGraphicsCaptureOff($1)", noWarn: true },
                { match: /useGraphicsCaptureApiLevel:([A-Za-z_$][\w$]*(?:\(\))?|[-]?\d+)/g, replace: "useGraphicsCaptureApiLevel:$self.forceGraphicsApiOff($1)", noWarn: true },
                { match: /useVideoHook:([A-Za-z_$][\w$]*(?:\(\))?|![01])/g, replace: "useVideoHook:$self.forceVideoHookOn($1)", noWarn: true }
            ]
        },
        {
            find: "sourcePid",
            all: true,
            replacement: [
                { match: /desktopSource:([A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)?)/g, replace: "desktopSource:$self.observeDesktopSource($1)", noWarn: true }
            ]
        },
    ],

    observeDesktopSource(source: any): any { return cacheCandidateSource(source); },

    forceSdrMode(original: unknown): "never" {
        captureState.patchCalls++;
        captureState.lastSeenAt = Date.now();
        captureState.originalHdr = original;
        captureState.effectiveHdr = "never";
        return "never";
    },
    forceGraphicsCaptureOff(original: unknown): boolean {
        captureState.originalGraphicsCapture = original;
        captureState.effectiveGraphicsCapture = false;
        captureState.lastSeenAt = Date.now();
        return false;
    },
    forceGraphicsApiOff(original: unknown): number {
        captureState.originalGraphicsApi = original;
        captureState.effectiveGraphicsApi = 0;
        captureState.lastSeenAt = Date.now();
        return 0;
    },
    forceVideoHookOn(original: unknown): boolean {
        captureState.originalVideoHook = original;
        captureState.effectiveVideoHook = true;
        captureState.lastSeenAt = Date.now();
        return true;
    },

    toolboxActions: {
        "Show DiscordHDRFix Status": () => alert(combinedStatus()),
        "Copy DiscordHDRFix Status": () => {
            const text = combinedStatus();
            void navigator.clipboard.writeText(text).catch(() => alert(text));
        },
        "Reset Active App to Automatic": () => void updateActiveProfile({ kind: "auto" }),
        "Show Logged Streamed Apps": () => {
            const rows = Object.values(profiles.apps)
                .sort((a, b) => b.lastSeen.localeCompare(a.lastSeen))
                .map(profile => `${profile.displayName} | ${profile.exeName ?? "unresolved"} | ${profileKindLabel(profile.kind)} | ${profile.lastSeen}`);
            alert(rows.length ? rows.join("\n") : "DiscordHDRFix has not logged any actually-streamed applications yet.");
        }
    },

    start(): void {
        void (async () => {
            await loadProfiles();
            await applyActiveProfile(true);
            await startNativeHost();

            hookMediaConnections();
            connectionHookTimer = window.setInterval(hookMediaConnections, 500);
            statusPollTimer = window.setInterval(() => void pollNativeStatus(), 500);
            popoutTimer = window.setInterval(refreshStreamPopoutPanel, 350);

            (window as any).DiscordHDRFix = {
                status: combinedStatus,
                profiles: () => profiles,
                activeStream: () => activeStream,
                nativeStatus: () => nativeStatus,
                apply: () => applyActiveProfile(true)
            };
        })();
    },

    stop(): void {
        if (statusPollTimer != null) clearInterval(statusPollTimer);
        if (popoutTimer != null) clearInterval(popoutTimer);
        if (connectionHookTimer != null) clearInterval(connectionHookTimer);
        statusPollTimer = null;
        popoutTimer = null;
        connectionHookTimer = null;
        unhookMediaConnections();
        removeInjectedPanels();
        delete (window as any).DiscordHDRFix;
    }
});
