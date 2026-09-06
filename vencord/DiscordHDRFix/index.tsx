import "./styles.css";

import { definePluginSettings } from "@api/Settings";
import { Logger } from "@utils/Logger";
import definePlugin, { OptionType, PluginNative } from "@utils/types";
import {
    Button,
    Forms,
    MediaEngineStore,
    React,
    Select,
    TextInput,
    useEffect,
    useState
} from "@webpack/common";

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
const hookedConnections = new Map<any, { setGoLiveSource?: Function; clearDesktopSource?: Function; }>();
const profileUiListeners = new Set<() => void>();

let profiles: ProfileDatabase = { version: 1, apps: {} };
let activeStream: ActiveStream | null = null;
let nativeStatus: any = null;
let lastAppliedFingerprint = "";
let statusPollTimer: number | null = null;
let popoutTimer: number | null = null;
let connectionHookTimer: number | null = null;
let sourceActivationSerial = 0;
let rendererCallsAtActivation = -1;

let streamMenuRow: HTMLElement | null = null;
let quickFlyout: HTMLElement | null = null;
let editorFlyout: HTMLElement | null = null;
let quickCloseTimer: number | null = null;
let globalDismissInstalled = false;

const DEFAULT_RENO_VALUES: CustomValues = {
    primaries: "rec2020",
    transfer: "srgb",
    metadata: "inject",
    sdrWhite: 360,
    inputMax: 200
};

const DEFAULT_CUSTOM: CustomValues = {
    primaries: "preserve",
    transfer: "preserve",
    metadata: "preserve",
    sdrWhite: 460,
    inputMax: 1000
};

const PROFILE_OPTIONS: Array<{ label: string; value: ProfileKind; }> = [
    { label: "Automatic", value: "auto" },
    { label: "SDR", value: "sdr" },
    { label: "Native HDR10", value: "hdr10" },
    { label: "scRGB", value: "scrgb" },
    { label: "RenoDX / ReShade", value: "renodx" },
    { label: "Custom", value: "custom" }
];

const PRIMARIES_OPTIONS: Array<{ label: string; value: Primaries; }> = [
    { label: "Preserve", value: "preserve" },
    { label: "Rec.709", value: "rec709" },
    { label: "Rec.2020", value: "rec2020" },
    { label: "Arc", value: "arc" }
];

const TRANSFER_OPTIONS: Array<{ label: string; value: Transfer; }> = [
    { label: "Preserve", value: "preserve" },
    { label: "Linear", value: "linear" },
    { label: "sRGB", value: "srgb" },
    { label: "ST.2084 / PQ", value: "pq" }
];

const METADATA_OPTIONS: Array<{ label: string; value: MetadataPolicy; }> = [
    { label: "Preserve Discord metadata", value: "preserve" },
    { label: "No HDR metadata", value: "none" },
    { label: "Inject HDR metadata", value: "inject" }
];

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
        description: "Add a Discord-style HDR Fix row to the normal Go Live stream menu.",
        default: true,
        restartNeeded: false,
        onChange: () => refreshStreamMenuUi()
    },
    profileManager: {
        type: OptionType.COMPONENT,
        component: SettingsProfileManager
    }
});

function notifyProfileUi(): void {
    for (const listener of profileUiListeners) {
        try { listener(); }
        catch {}
    }
}

function clampWhite(value: unknown, fallback = 460): number {
    const number = Number(value);
    return Number.isFinite(number) ? Math.min(1000, Math.max(40, number)) : fallback;
}

function clampPeak(value: unknown, fallback = 1000): number {
    const number = Number(value);
    return Number.isFinite(number) ? Math.min(10000, Math.max(100, number)) : fallback;
}

function nowIso(): string {
    return new Date().toISOString();
}

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

function defaultProfile(
    key: string,
    displayName: string,
    exeName: string | null,
    exePath: string | null,
    sourceId: string
): AppProfile {
    const now = nowIso();

    return {
        key,
        exeName,
        exePath,
        displayName,
        firstSeen: now,
        lastSeen: now,
        lastSourceId: sourceId || null,
        kind: "auto",
        custom: { ...DEFAULT_CUSTOM }
    };
}

async function loadProfiles(): Promise<void> {
    try {
        const loaded = await Native.readProfiles();

        if (loaded && typeof loaded === "object" && loaded.apps) {
            profiles = {
                version: 1,
                apps: loaded.apps,
                loadError: loaded.loadError
            };

            for (const profile of Object.values(profiles.apps)) {
                profile.custom = {
                    ...DEFAULT_CUSTOM,
                    ...profile.custom
                };
            }
        }
    } catch (error) {
        logger.error("Failed to load per-application profiles:", error);
    }

    notifyProfileUi();
}

async function saveProfiles(): Promise<void> {
    try {
        await Native.writeProfiles(profiles);
    } catch (error) {
        logger.error("Failed to save per-application profiles:", error);
    }

    notifyProfileUi();
}

function getActiveProfile(): AppProfile | null {
    return activeStream
        ? profiles.apps[activeStream.profileKey] ?? null
        : null;
}

function describeFormat(status: any): string {
    if (!status)
        return "Waiting for renderer";

    const format = Number(status.last_format);

    if (format === 24)
        return "10-bit R10G10B10A2";

    if (format === 10)
        return "FP16 R16G16B16A16";

    if (format === 28)
        return "8-bit R8G8B8A8";

    if (Number.isFinite(format) && format >= 0)
        return `DXGI format ${format}`;

    return "Not observed";
}

function automaticConfig(status: any): AppliedConfig {
    const rendererCalls = Number(status?.total_renderer_calls);

    if (
        activeStream &&
        rendererCallsAtActivation >= 0 &&
        (!Number.isFinite(rendererCalls) ||
            rendererCalls <= rendererCallsAtActivation)
    ) {
        return {
            mode: "observe",
            white: 460,
            peak: 1000,
            primaries: "preserve",
            transfer: "preserve",
            metadata: "preserve",
            reason: "Waiting for frames from the new application"
        };
    }

    const format = Number(status?.last_format);

    if (format === 28) {
        return {
            mode: "force_sdr",
            white: 460,
            peak: 1000,
            primaries: "preserve",
            transfer: "preserve",
            metadata: "none",
            reason: "Automatic · SDR"
        };
    }

    if (format === 24) {
        return {
            mode: "force_hdr10",
            white: 460,
            peak: 1000,
            primaries: "rec2020",
            transfer: "pq",
            metadata: "inject",
            reason: "Automatic · Native HDR10"
        };
    }

    if (format === 10) {
        return {
            mode: "force_scrgb",
            white: 460,
            peak: 1000,
            primaries: "rec709",
            transfer: "linear",
            metadata: "inject",
            reason: "Automatic · scRGB"
        };
    }

    return {
        mode: "observe",
        white: 460,
        peak: 1000,
        primaries: "preserve",
        transfer: "preserve",
        metadata: "preserve",
        reason: "Automatic · Unclassified"
    };
}

function profileConfig(profile: AppProfile, status: any): AppliedConfig {
    switch (profile.kind) {
    case "auto":
        return automaticConfig(status);

    case "sdr":
        return {
            mode: "force_sdr",
            white: 460,
            peak: 1000,
            primaries: "preserve",
            transfer: "preserve",
            metadata: "none",
            reason: "SDR override"
        };

    case "hdr10":
        return {
            mode: "force_hdr10",
            white: 460,
            peak: 1000,
            primaries: "rec2020",
            transfer: "pq",
            metadata: "inject",
            reason: "Native HDR10 override"
        };

    case "scrgb":
        return {
            mode: "force_scrgb",
            white: 460,
            peak: 1000,
            primaries: "rec709",
            transfer: "linear",
            metadata: "inject",
            reason: "scRGB override"
        };

    case "renodx":
        return {
            mode: "custom",
            white: clampWhite(profile.custom?.sdrWhite, DEFAULT_RENO_VALUES.sdrWhite),
            peak: clampPeak(profile.custom?.inputMax, DEFAULT_RENO_VALUES.inputMax),
            primaries: profile.custom?.primaries ?? DEFAULT_RENO_VALUES.primaries,
            transfer: profile.custom?.transfer ?? DEFAULT_RENO_VALUES.transfer,
            metadata: profile.custom?.metadata ?? DEFAULT_RENO_VALUES.metadata,
            reason: "RenoDX / ReShade override"
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
            reason: "Custom override"
        };
    }
}

async function applyConfig(
    config: AppliedConfig,
    force = false
): Promise<void> {
    const fingerprint = JSON.stringify({
        enabled: settings.store.enabled,
        trace: settings.store.traceEnabled,
        ...config
    });

    if (!force && fingerprint === lastAppliedFingerprint)
        return;

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
        logger.error(
            "Failed to apply active application profile:",
            error
        );
    }
}

async function applyActiveProfile(force = false): Promise<void> {
    if (!settings.store.enabled) {
        await applyConfig({
            mode: "observe",
            white: 460,
            peak: 1000,
            primaries: "preserve",
            transfer: "preserve",
            metadata: "preserve",
            reason: "DiscordHDRFix disabled"
        }, force);

        return;
    }

    const profile = getActiveProfile();

    if (!profile) {
        await applyConfig({
            mode: "observe",
            white: 460,
            peak: 1000,
            primaries: "preserve",
            transfer: "preserve",
            metadata: "preserve",
            reason: "No active stream"
        }, force);

        return;
    }

    await applyConfig(
        profileConfig(profile, nativeStatus),
        force
    );
}

async function startNativeHost(): Promise<void> {
    try {
        const result = await Native.startNativeFix();

        if (!result?.ok)
            logger.error(
                "Native dev host launch failed:",
                result
            );
    } catch (error) {
        logger.error(
            "Failed to launch native dev host:",
            error
        );
    }
}

function sourceTitle(source: any): string | null {
    return cleanDisplayName(
        source?.name
        ?? source?.title
        ?? source?.applicationName
        ?? source?.windowTitle
        ?? null
    );
}

function sourcePid(source: any): number | null {
    const value = Number(
        source?.sourcePid
        ?? source?.pid
        ?? source?.processId
        ?? 0
    );

    return Number.isFinite(value) && value > 0
        ? Math.trunc(value)
        : null;
}

function cacheCandidateSource(source: any): any {
    const id = String(source?.id ?? "");

    if (!id)
        return source;

    candidateSources.set(id, {
        id,
        sourcePid: sourcePid(source),
        title: sourceTitle(source),
        seenAt: Date.now()
    });

    if (candidateSources.size > 256) {
        const cutoff = Date.now() - 5 * 60_000;

        for (const [key, value] of candidateSources) {
            if (value.seenAt < cutoff)
                candidateSources.delete(key);
        }
    }

    return source;
}

async function activateActualStream(
    sourceId: string,
    directPid: number | null = null,
    directTitle: string | null = null
): Promise<void> {
    if (!sourceId)
        return;

    const serial = ++sourceActivationSerial;
    const candidate = candidateSources.get(sourceId);

    const pid =
        directPid
        ?? candidate?.sourcePid
        ?? null;

    const title =
        directTitle
        ?? candidate?.title
        ?? null;

    let identity: any = null;

    try {
        identity = await Native.resolveStreamIdentity(
            pid,
            sourceId,
            title
        );
    } catch (error) {
        logger.warn(
            "Could not resolve streamed process identity:",
            error
        );
    }

    if (serial !== sourceActivationSerial)
        return;

    const exeName =
        cleanDisplayName(identity?.exeName);

    const exePath =
        cleanDisplayName(identity?.exePath);

    const key = exePath
        ? `path:${exePath.toLowerCase()}`
        : (
            exeName
                ? `exe:${exeName.toLowerCase()}`
                : stableFallbackKey(
                    title,
                    sourceId
                )
        );

    const displayName =
        title
        ?? exeName
        ?? cleanDisplayName(
            identity?.processName
        )
        ?? sourceId;

    let profile = profiles.apps[key];

    // Migrate a previous unresolved "window:..." profile when the improved
    // HWND->PID resolver can now identify the real executable.
    if (!profile && (exeName || exePath)) {
        const fallbackEntry =
            Object.entries(profiles.apps)
                .find(([, candidateProfile]) =>
                    candidateProfile.lastSourceId === sourceId &&
                    candidateProfile.key.startsWith("stream:")
                );

        if (fallbackEntry) {
            const [oldKey, oldProfile] = fallbackEntry;

            delete profiles.apps[oldKey];

            profile = {
                ...oldProfile,
                key
            };
        }
    }

    profile ??= defaultProfile(
        key,
        displayName,
        exeName,
        exePath,
        sourceId
    );

    profile.exeName =
        exeName ?? profile.exeName;

    profile.exePath =
        exePath ?? profile.exePath;

    profile.displayName =
        displayName || profile.displayName;

    profile.lastSeen = nowIso();
    profile.lastSourceId = sourceId;

    profile.custom = {
        ...DEFAULT_CUSTOM,
        ...profile.custom
    };

    profiles.apps[key] = profile;

    rendererCallsAtActivation =
        Number(
            nativeStatus?.total_renderer_calls
        );

    if (!Number.isFinite(
            rendererCallsAtActivation
        )) {
        rendererCallsAtActivation = -1;
    }

    activeStream = {
        sourceId,
        sourcePid:
            Number(identity?.pid) > 0
                ? Number(identity.pid)
                : pid,
        profileKey: key,
        displayName: profile.displayName,
        exeName: profile.exeName,
        exePath: profile.exePath
    };

    await saveProfiles();

    lastAppliedFingerprint = "";

    await applyActiveProfile(true);

    refreshStreamMenuUi();
}

function confirmGoLiveOptions(options: any): any {
    const description =
        options?.desktopDescription
        ?? options?.desktopSource
        ?? null;

    const id =
        String(
            description?.id ?? ""
        );

    if (id) {
        void activateActualStream(
            id,
            sourcePid(description),
            sourceTitle(description)
        );
    }

    return options;
}

function streamEnded(): void {
    activeStream = null;
    rendererCallsAtActivation = -1;
    sourceActivationSerial++;

    lastAppliedFingerprint = "";

    void applyActiveProfile(true);

    closeAllHdrFixMenus();
    removeStreamMenuRow();
    notifyProfileUi();
}

function hookMediaConnections(): void {
    let engine: any;

    try {
        engine =
            MediaEngineStore.getMediaEngine();
    } catch {
        return;
    }

    const connections =
        engine?.connections;

    if (
        !connections ||
        typeof connections[Symbol.iterator] !==
            "function"
    ) {
        return;
    }

    for (
        const connection
        of connections as Iterable<any>
    ) {
        if (
            !connection ||
            hookedConnections.has(connection)
        ) {
            continue;
        }

        const originals: {
            setGoLiveSource?: Function;
            clearDesktopSource?: Function;
        } = {};

        if (
            typeof connection.setGoLiveSource ===
            "function"
        ) {
            const original =
                connection.setGoLiveSource;

            originals.setGoLiveSource =
                original;

            connection.setGoLiveSource =
                function (options: any) {
                    confirmGoLiveOptions(options);

                    return original.call(
                        this,
                        options
                    );
                };
        }

        if (
            typeof connection.clearDesktopSource ===
            "function"
        ) {
            const original =
                connection.clearDesktopSource;

            originals.clearDesktopSource =
                original;

            connection.clearDesktopSource =
                function (...args: any[]) {
                    streamEnded();

                    return original.apply(
                        this,
                        args
                    );
                };
        }

        hookedConnections.set(
            connection,
            originals
        );
    }
}

function unhookMediaConnections(): void {
    for (
        const [connection, originals]
        of hookedConnections
    ) {
        try {
            if (originals.setGoLiveSource) {
                connection.setGoLiveSource =
                    originals.setGoLiveSource;
            }

            if (originals.clearDesktopSource) {
                connection.clearDesktopSource =
                    originals.clearDesktopSource;
            }
        } catch {}
    }

    hookedConnections.clear();
}

function ensureRenoDefaults(
    profile: AppProfile
): void {
    const isUntouchedCustom =
        profile.custom?.primaries ===
            DEFAULT_CUSTOM.primaries &&
        profile.custom?.transfer ===
            DEFAULT_CUSTOM.transfer &&
        profile.custom?.metadata ===
            DEFAULT_CUSTOM.metadata &&
        profile.custom?.sdrWhite ===
            DEFAULT_CUSTOM.sdrWhite &&
        profile.custom?.inputMax ===
            DEFAULT_CUSTOM.inputMax;

    if (!profile.custom || isUntouchedCustom) {
        profile.custom = {
            ...DEFAULT_RENO_VALUES
        };
    }
}

async function setProfileKind(
    key: string,
    kind: ProfileKind
): Promise<void> {
    const profile = profiles.apps[key];

    if (!profile)
        return;

    if (kind === "renodx")
        ensureRenoDefaults(profile);

    if (kind === "custom" && !profile.custom) {
        profile.custom = {
            ...DEFAULT_CUSTOM
        };
    }

    profile.kind = kind;
    profile.lastSeen = nowIso();

    profiles.apps[key] = profile;

    await saveProfiles();

    if (
        activeStream?.profileKey === key
    ) {
        lastAppliedFingerprint = "";
        await applyActiveProfile(true);
    }

    refreshStreamMenuUi();
    notifyProfileUi();
}

async function updateProfileCustom(
    key: string,
    patch: Partial<CustomValues>
): Promise<void> {
    const profile = profiles.apps[key];

    if (!profile)
        return;

    profile.custom = {
        ...DEFAULT_CUSTOM,
        ...profile.custom,
        ...patch
    };

    profile.lastSeen = nowIso();

    profiles.apps[key] = profile;

    await saveProfiles();

    if (
        activeStream?.profileKey === key
    ) {
        lastAppliedFingerprint = "";
        await applyActiveProfile(true);
    }

    refreshStreamMenuUi();
    notifyProfileUi();
}

async function resetProfile(
    key: string
): Promise<void> {
    await setProfileKind(
        key,
        "auto"
    );
}

async function forgetProfile(
    key: string
): Promise<void> {
    if (!profiles.apps[key])
        return;

    delete profiles.apps[key];

    if (
        activeStream?.profileKey === key
    ) {
        const stream = activeStream;

        const replacement =
            defaultProfile(
                key,
                stream.displayName,
                stream.exeName,
                stream.exePath,
                stream.sourceId
            );

        profiles.apps[key] =
            replacement;
    }

    await saveProfiles();

    lastAppliedFingerprint = "";

    if (
        activeStream?.profileKey === key
    ) {
        await applyActiveProfile(true);
    }

    refreshStreamMenuUi();
    notifyProfileUi();
}

function profileKindLabel(
    kind: ProfileKind
): string {
    return PROFILE_OPTIONS
        .find(option =>
            option.value === kind
        )?.label ?? kind;
}

function activeConfigDescription(): string {
    const profile = getActiveProfile();

    return profile
        ? profileConfig(
            profile,
            nativeStatus
        ).reason
        : "No active stream";
}

function svgIcon(): string {
    return `
        <svg viewBox="0 0 24 24" aria-hidden="true">
            <path fill="currentColor"
                d="M4 5.5h16a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2h-5.1l-1.7 2.1a1.55 1.55 0 0 1-2.4 0l-1.7-2.1H4a2 2 0 0 1-2-2v-9a2 2 0 0 1 2-2Zm1.5 3v7h2v-2.4h2.2v2.4h2v-7h-2v2.7H7.5V8.5h-2Zm7.2 0v7h3.05c2.22 0 3.75-1.32 3.75-3.5s-1.53-3.5-3.75-3.5H12.7Zm2 1.8h.9c1.17 0 1.86.58 1.86 1.7 0 1.12-.69 1.7-1.86 1.7h-.9v-3.4Z"/>
        </svg>`;
}

function chevronSvg(): string {
    return `
        <svg viewBox="0 0 20 20" aria-hidden="true">
            <path fill="currentColor"
                d="m7.4 4.7 5.3 5.3-5.3 5.3 1.4 1.4 6.7-6.7-6.7-6.7-1.4 1.4Z"/>
        </svg>`;
}

function isVisible(
    element: HTMLElement
): boolean {
    const style =
        getComputedStyle(element);

    if (
        style.display === "none" ||
        style.visibility === "hidden"
    ) {
        return false;
    }

    const rect =
        element.getBoundingClientRect();

    return (
        rect.width > 180 &&
        rect.height > 80 &&
        rect.width < 1000 &&
        rect.height < 1400
    );
}

function findStreamSettingsContainer():
    HTMLElement | null {
    const terms = [
        "change windows",
        "change window",
        "change application",
        "change applications",
        "stream quality",
        "stream settings",
        "stop streaming"
    ];

    const candidates =
        Array.from(
            document.querySelectorAll<HTMLElement>(
                '[role="dialog"], [role="menu"], [class*="popout"], [class*="Popout"], [class*="modal"], [class*="Modal"]'
            )
        ).filter(element => {
            if (!isVisible(element))
                return false;

            const text =
                (element.innerText || "")
                    .toLowerCase()
                    .slice(0, 6000);

            return terms.some(term =>
                text.includes(term)
            );
        });

    candidates.sort((a, b) => {
        const ar =
            a.getBoundingClientRect();

        const br =
            b.getBoundingClientRect();

        return (
            ar.width * ar.height -
            br.width * br.height
        );
    });

    return candidates[0] ?? null;
}

function findMenuInsertionPoint(
    container: HTMLElement
): HTMLElement {
    const roleMenu =
        container.querySelector<HTMLElement>(
            '[role="menu"]'
        );

    return roleMenu ?? container;
}

function stopDiscordMenuPropagation(
    event: Event
): void {
    event.stopPropagation();
}

function removeStreamMenuRow(): void {
    streamMenuRow?.remove();
    streamMenuRow = null;
}

function positionFloatingElement(
    element: HTMLElement,
    anchorRect: DOMRect,
    preferredWidth: number
): void {
    const gap = 8;
    const viewportPadding = 12;

    let left =
        anchorRect.right + gap;

    if (
        left + preferredWidth >
        window.innerWidth -
            viewportPadding
    ) {
        left =
            anchorRect.left -
            preferredWidth -
            gap;
    }

    left = Math.max(
        viewportPadding,
        Math.min(
            left,
            window.innerWidth -
                preferredWidth -
                viewportPadding
        )
    );

    const rect =
        element.getBoundingClientRect();

    let top =
        anchorRect.top;

    if (
        top + rect.height >
        window.innerHeight -
            viewportPadding
    ) {
        top =
            window.innerHeight -
            rect.height -
            viewportPadding;
    }

    top = Math.max(
        viewportPadding,
        top
    );

    element.style.left =
        `${Math.round(left)}px`;

    element.style.top =
        `${Math.round(top)}px`;
}

function removeFlyoutAnimated(
    element: HTMLElement | null
): void {
    if (!element)
        return;

    element.classList.add(
        "vc-hdrfix-closing"
    );

    window.setTimeout(
        () => element.remove(),
        130
    );
}

function closeQuickFlyout(): void {
    if (quickCloseTimer != null) {
        clearTimeout(
            quickCloseTimer
        );

        quickCloseTimer = null;
    }

    const old = quickFlyout;
    quickFlyout = null;

    removeFlyoutAnimated(old);
}

function scheduleQuickClose(): void {
    if (quickCloseTimer != null)
        clearTimeout(
            quickCloseTimer
        );

    quickCloseTimer =
        window.setTimeout(
            closeQuickFlyout,
            180
        );
}

function closeEditorFlyout(): void {
    const old = editorFlyout;
    editorFlyout = null;

    removeFlyoutAnimated(old);
}

function closeAllHdrFixMenus(): void {
    closeQuickFlyout();
    closeEditorFlyout();
}

function installGlobalDismiss(): void {
    if (globalDismissInstalled)
        return;

    globalDismissInstalled = true;

    document.addEventListener(
        "pointerdown",
        event => {
            const target =
                event.target as Node | null;

            if (!target)
                return;

            if (
                streamMenuRow?.contains(target) ||
                quickFlyout?.contains(target) ||
                editorFlyout?.contains(target)
            ) {
                return;
            }

            closeAllHdrFixMenus();
        },
        true
    );

    document.addEventListener(
        "keydown",
        event => {
            if (event.key === "Escape")
                closeAllHdrFixMenus();
        },
        true
    );
}

function quickPresetButton(
    profile: AppProfile,
    option: {
        label: string;
        value: ProfileKind;
    }
): HTMLButtonElement {
    const button =
        document.createElement("button");

    button.type = "button";

    button.className =
        "vc-hdrfix-menu-option";

    button.dataset.selected =
        String(
            profile.kind === option.value
        );

    const text =
        document.createElement("span");

    text.textContent =
        option.label;

    button.appendChild(text);

    if (
        profile.kind === option.value
    ) {
        const check =
            document.createElement("span");

        check.className =
            "vc-hdrfix-check";

        check.textContent = "✓";

        button.appendChild(check);
    }

    button.addEventListener(
        "click",
        event => {
            event.preventDefault();
            event.stopPropagation();

            void setProfileKind(
                profile.key,
                option.value
            );

            closeQuickFlyout();
        }
    );

    return button;
}

function openQuickFlyout(): void {
    if (
        !streamMenuRow ||
        editorFlyout
    ) {
        return;
    }

    const profile =
        getActiveProfile();

    if (quickCloseTimer != null) {
        clearTimeout(
            quickCloseTimer
        );

        quickCloseTimer = null;
    }

    if (quickFlyout)
        return;

    const anchor =
        streamMenuRow
            .getBoundingClientRect();

    const flyout =
        document.createElement("div");

    flyout.className =
        "vc-hdrfix-flyout vc-hdrfix-quick";

    flyout.setAttribute(
        "role",
        "menu"
    );

    const header =
        document.createElement("div");

    header.className =
        "vc-hdrfix-flyout-header";

    header.textContent =
        profile?.displayName ?? "Discord HDR Fix";

    flyout.appendChild(header);

    if (profile) {
        for (
            const option
            of PROFILE_OPTIONS
        ) {
            flyout.appendChild(
                quickPresetButton(
                    profile,
                    option
                )
            );
        }
    } else {
        const waiting =
            document.createElement("div");

        waiting.className =
            "vc-hdrfix-waiting";

        waiting.textContent =
            "Waiting for Discord to report the active streamed application…";

        flyout.appendChild(waiting);
    }

    const footer =
        document.createElement("div");

    footer.className =
        "vc-hdrfix-quick-footer";

    footer.innerHTML =
        profile
            ? `<span>${describeFormat(nativeStatus)}</span><span>${activeConfigDescription()}</span>`
            : `<span>${describeFormat(nativeStatus)}</span><span>Source identity not resolved yet</span>`;

    flyout.appendChild(footer);

    flyout.addEventListener(
        "mouseenter",
        () => {
            if (
                quickCloseTimer != null
            ) {
                clearTimeout(
                    quickCloseTimer
                );

                quickCloseTimer =
                    null;
            }
        }
    );

    flyout.addEventListener(
        "mouseleave",
        scheduleQuickClose
    );

    document.body.appendChild(
        flyout
    );

    quickFlyout = flyout;

    positionFloatingElement(
        flyout,
        anchor,
        236
    );

    requestAnimationFrame(
        () =>
            flyout.classList.add(
                "vc-hdrfix-open"
            )
    );
}

function createChoiceGroup<T extends string>(
    title: string,
    options: Array<{
        label: string;
        value: T;
    }>,
    selected: T,
    onSelect: (value: T) => void
): HTMLElement {
    const section =
        document.createElement("div");

    section.className =
        "vc-hdrfix-editor-section";

    const label =
        document.createElement("div");

    label.className =
        "vc-hdrfix-section-label";

    label.textContent = title;

    section.appendChild(label);

    const grid =
        document.createElement("div");

    grid.className =
        "vc-hdrfix-choice-grid";

    for (const option of options) {
        const button =
            document.createElement("button");

        button.type = "button";

        button.className =
            "vc-hdrfix-choice";

        button.dataset.selected =
            String(
                option.value === selected
            );

        button.textContent =
            option.label;

        button.addEventListener(
            "click",
            event => {
                event.preventDefault();
                event.stopPropagation();

                onSelect(option.value);
            }
        );

        grid.appendChild(button);
    }

    section.appendChild(grid);

    return section;
}

function createNumberField(
    labelText: string,
    value: number,
    min: number,
    max: number,
    onCommit: (value: number) => void
): HTMLElement {
    const label =
        document.createElement("label");

    label.className =
        "vc-hdrfix-number-field";

    const caption =
        document.createElement("span");

    caption.textContent =
        labelText;

    const input =
        document.createElement("input");

    input.type = "number";
    input.min = String(min);
    input.max = String(max);
    input.step = "1";
    input.value = String(value);

    input.addEventListener(
        "pointerdown",
        stopDiscordMenuPropagation
    );

    input.addEventListener(
        "click",
        stopDiscordMenuPropagation
    );

    input.addEventListener(
        "change",
        () => {
            const numeric =
                Number(input.value);

            if (Number.isFinite(numeric))
                onCommit(numeric);
        }
    );

    label.appendChild(caption);
    label.appendChild(input);

    return label;
}

function rerenderEditorFlyout(): void {
    if (!editorFlyout)
        return;

    const focused =
        editorFlyout.contains(
            document.activeElement
        );

    if (focused)
        return;

    renderEditorContents(
        editorFlyout
    );
}

function renderEditorContents(
    panel: HTMLElement
): void {
    const profile =
        getActiveProfile();

    panel.innerHTML = "";

    if (!profile) {
        const header =
            document.createElement("div");

        header.className =
            "vc-hdrfix-editor-header";

        const titleWrap =
            document.createElement("div");

        titleWrap.className =
            "vc-hdrfix-editor-title-wrap";

        const title =
            document.createElement("div");

        title.className =
            "vc-hdrfix-editor-title";

        title.textContent =
            "Discord HDR Fix";

        const subtitle =
            document.createElement("div");

        subtitle.className =
            "vc-hdrfix-editor-subtitle";

        subtitle.textContent =
            "Detecting the active streamed application…";

        titleWrap.appendChild(title);
        titleWrap.appendChild(subtitle);

        const close =
            document.createElement("button");

        close.type = "button";
        close.className =
            "vc-hdrfix-close-button";
        close.textContent = "×";

        close.addEventListener(
            "click",
            event => {
                event.preventDefault();
                event.stopPropagation();
                closeEditorFlyout();
            }
        );

        header.appendChild(titleWrap);
        header.appendChild(close);
        panel.appendChild(header);

        const waiting =
            document.createElement("div");

        waiting.className =
            "vc-hdrfix-editor-waiting";

        waiting.innerHTML = `
            <strong>Stream source not resolved yet</strong>
            <span>The menu is hooked, but DiscordHDRFix has not received the active Go Live source. The direct and runtime source hooks are both enabled in this build.</span>
        `;

        panel.appendChild(waiting);
        return;
    }

    const header =
        document.createElement("div");

    header.className =
        "vc-hdrfix-editor-header";

    const titleWrap =
        document.createElement("div");

    titleWrap.className =
        "vc-hdrfix-editor-title-wrap";

    const title =
        document.createElement("div");

    title.className =
        "vc-hdrfix-editor-title";

    title.textContent =
        "Discord HDR Fix";

    const subtitle =
        document.createElement("div");

    subtitle.className =
        "vc-hdrfix-editor-subtitle";

    subtitle.textContent =
        profile.exeName
        ? `${profile.displayName} · ${profile.exeName}`
        : profile.displayName;

    titleWrap.appendChild(title);
    titleWrap.appendChild(subtitle);

    const close =
        document.createElement("button");

    close.type = "button";
    close.className =
        "vc-hdrfix-close-button";

    close.setAttribute(
        "aria-label",
        "Close"
    );

    close.textContent = "×";

    close.addEventListener(
        "click",
        event => {
            event.preventDefault();
            event.stopPropagation();
            closeEditorFlyout();
        }
    );

    header.appendChild(titleWrap);
    header.appendChild(close);

    panel.appendChild(header);

    const status =
        document.createElement("div");

    status.className =
        "vc-hdrfix-status-card";

    const config =
        profileConfig(
            profile,
            nativeStatus
        );

    status.innerHTML = `
        <div><span>Detected</span><b>${describeFormat(nativeStatus)}</b></div>
        <div><span>Applied</span><b>${config.reason}</b></div>
    `;

    panel.appendChild(status);

    panel.appendChild(
        createChoiceGroup(
            "Profile",
            PROFILE_OPTIONS,
            profile.kind,
            value => {
                void setProfileKind(
                    profile.key,
                    value
                ).then(
                    rerenderEditorFlyout
                );
            }
        )
    );

    if (
        profile.kind === "renodx" ||
        profile.kind === "custom"
    ) {
        const helper =
            document.createElement("div");

        helper.className =
            "vc-hdrfix-helper-text";

        helper.textContent =
            profile.kind === "renodx"
                ? "RenoDX / ReShade is a customizable per-game profile. These values apply only to this application."
                : "Custom values apply only to this application.";

        panel.appendChild(helper);

        panel.appendChild(
            createChoiceGroup(
                "Primaries",
                PRIMARIES_OPTIONS,
                profile.custom.primaries,
                value => {
                    void updateProfileCustom(
                        profile.key,
                        { primaries: value }
                    ).then(
                        rerenderEditorFlyout
                    );
                }
            )
        );

        panel.appendChild(
            createChoiceGroup(
                "Transfer",
                TRANSFER_OPTIONS,
                profile.custom.transfer,
                value => {
                    void updateProfileCustom(
                        profile.key,
                        { transfer: value }
                    ).then(
                        rerenderEditorFlyout
                    );
                }
            )
        );

        panel.appendChild(
            createChoiceGroup(
                "HDR metadata",
                METADATA_OPTIONS,
                profile.custom.metadata,
                value => {
                    void updateProfileCustom(
                        profile.key,
                        { metadata: value }
                    ).then(
                        rerenderEditorFlyout
                    );
                }
            )
        );

        const numbers =
            document.createElement("div");

        numbers.className =
            "vc-hdrfix-number-grid";

        numbers.appendChild(
            createNumberField(
                "SDR white",
                profile.custom.sdrWhite,
                40,
                1000,
                value => {
                    void updateProfileCustom(
                        profile.key,
                        {
                            sdrWhite:
                                clampWhite(
                                    value,
                                    profile.custom
                                        .sdrWhite
                                )
                        }
                    );
                }
            )
        );

        numbers.appendChild(
            createNumberField(
                "Input max",
                profile.custom.inputMax,
                100,
                10000,
                value => {
                    void updateProfileCustom(
                        profile.key,
                        {
                            inputMax:
                                clampPeak(
                                    value,
                                    profile.custom
                                        .inputMax
                                )
                        }
                    );
                }
            )
        );

        panel.appendChild(numbers);
    } else {
        const preset =
            document.createElement("div");

        preset.className =
            "vc-hdrfix-preset-summary";

        preset.textContent =
            `${config.primaries} · ${config.transfer} · ${config.metadata} · ${config.white}/${config.peak}`;

        panel.appendChild(preset);
    }

    const footer =
        document.createElement("div");

    footer.className =
        "vc-hdrfix-editor-footer";

    const reset =
        document.createElement("button");

    reset.type = "button";
    reset.className =
        "vc-hdrfix-secondary-button";

    reset.textContent =
        "Reset to Automatic";

    reset.addEventListener(
        "click",
        event => {
            event.preventDefault();
            event.stopPropagation();

            void resetProfile(
                profile.key
            ).then(
                rerenderEditorFlyout
            );
        }
    );

    footer.appendChild(reset);

    panel.appendChild(footer);
}

function openEditorFlyout(): void {
    const profile =
        getActiveProfile();

    if (!streamMenuRow)
        return;

    closeQuickFlyout();

    const anchor =
        streamMenuRow
            .getBoundingClientRect();

    if (editorFlyout) {
        positionFloatingElement(
            editorFlyout,
            anchor,
            382
        );

        return;
    }

    const panel =
        document.createElement("div");

    panel.className =
        "vc-hdrfix-flyout vc-hdrfix-editor";

    panel.setAttribute(
        "role",
        "dialog"
    );

    panel.setAttribute(
        "aria-label",
        "Discord HDR Fix settings"
    );

    panel.addEventListener(
        "pointerdown",
        stopDiscordMenuPropagation
    );

    panel.addEventListener(
        "click",
        stopDiscordMenuPropagation
    );

    renderEditorContents(panel);

    document.body.appendChild(
        panel
    );

    editorFlyout = panel;

    positionFloatingElement(
        panel,
        anchor,
        382
    );

    requestAnimationFrame(
        () =>
            panel.classList.add(
                "vc-hdrfix-open"
            )
    );
}

function createStreamMenuRow(
    container: HTMLElement
): HTMLElement {
    const row =
        document.createElement("button");

    row.type = "button";

    row.className =
        "vc-hdrfix-stream-row";

    row.dataset.discordHdrFixRow =
        "1";

    row.innerHTML = `
        <span class="vc-hdrfix-row-icon">
            ${svgIcon()}
        </span>
        <span class="vc-hdrfix-row-copy">
            <span class="vc-hdrfix-row-label">Discord HDR Fix</span>
            <span class="vc-hdrfix-row-value"></span>
        </span>
        <span class="vc-hdrfix-row-chevron">
            ${chevronSvg()}
        </span>
    `;

    row.addEventListener(
        "pointerdown",
        stopDiscordMenuPropagation
    );

    row.addEventListener(
        "click",
        event => {
            event.preventDefault();
            event.stopPropagation();

            openEditorFlyout();
        }
    );

    row.addEventListener(
        "mouseenter",
        openQuickFlyout
    );

    row.addEventListener(
        "mouseleave",
        scheduleQuickClose
    );

    const insertionPoint =
        findMenuInsertionPoint(
            container
        );

    insertionPoint.appendChild(row);

    return row;
}

function refreshStreamMenuUi(): void {
    if (
        !settings.store
            .showStreamPopoutControls
    ) {
        closeAllHdrFixMenus();
        removeStreamMenuRow();
        return;
    }

    const profile =
        getActiveProfile();

    const container =
        findStreamSettingsContainer();

    if (!container) {
        if (
            streamMenuRow &&
            !document.contains(
                streamMenuRow
            )
        ) {
            streamMenuRow = null;
            closeAllHdrFixMenus();
        }

        return;
    }

    const existing =
        container.querySelector<HTMLElement>(
            '[data-discord-hdr-fix-row="1"]'
        );

    if (existing) {
        streamMenuRow = existing;
    } else {
        removeStreamMenuRow();

        streamMenuRow =
            createStreamMenuRow(
                container
            );
    }

    const value =
        streamMenuRow.querySelector<HTMLElement>(
            ".vc-hdrfix-row-value"
        );

    if (value) {
        value.textContent =
            profile
                ? `${profileKindLabel(profile.kind)} · ${describeFormat(nativeStatus)}`
                : "Detecting active stream…";
    }

    if (editorFlyout) {
        rerenderEditorFlyout();

        positionFloatingElement(
            editorFlyout,
            streamMenuRow
                .getBoundingClientRect(),
            382
        );
    }
}

async function pollNativeStatus(): Promise<void> {
    try {
        nativeStatus =
            await Native.readNativeStatus();
    } catch {
        nativeStatus = null;
    }

    if (
        getActiveProfile()?.kind ===
        "auto"
    ) {
        await applyActiveProfile();
    }

    refreshStreamMenuUi();
    notifyProfileUi();
}

function fmt(value: unknown): string {
    if (value === undefined)
        return "<not observed>";

    return typeof value === "string"
        ? JSON.stringify(value)
        : String(value);
}

function combinedStatus(): string {
    const profile =
        getActiveProfile();

    const config =
        profile
            ? profileConfig(
                profile,
                nativeStatus
            )
            : null;

    return [
        "DiscordHDRFix v1.2.2 stream UI fix",
        "----------------------------------------",
        `Active stream:                 ${activeStream?.displayName ?? "<none>"}`,
        `Active executable:             ${activeStream?.exeName ?? "<unresolved>"}`,
        `Profile key:                   ${activeStream?.profileKey ?? "<none>"}`,
        `Profile:                       ${profile ? profileKindLabel(profile.kind) : "<none>"}`,
        `Detected format:               ${describeFormat(nativeStatus)}`,
        `Applied mode:                  ${config?.mode ?? "observe"}`,
        `Applied reason:                ${config?.reason ?? "No active stream"}`,
        `Logged streamed applications:  ${Object.keys(profiles.apps).length}`,
        `Candidate sources cached:      ${candidateSources.size}`,
        `Runtime connections hooked:    ${hookedConnections.size}`,
        "",
        "Capture routing",
        `HDR:                           ${fmt(captureState.originalHdr)} -> ${fmt(captureState.effectiveHdr)}`,
        `Graphics Capture:              ${fmt(captureState.originalGraphicsCapture)} -> ${fmt(captureState.effectiveGraphicsCapture)}`,
        `Graphics Capture API:          ${fmt(captureState.originalGraphicsApi)} -> ${fmt(captureState.effectiveGraphicsApi)}`,
        `Video Hook:                    ${fmt(captureState.originalVideoHook)} -> ${fmt(captureState.effectiveVideoHook)}`,
        "",
        "Native",
        JSON.stringify(
            nativeStatus,
            null,
            2
        )
    ].join("\n");
}

function SettingsProfileManager() {
    const [, setRevision] =
        useState(0);

    const [selectedKey, setSelectedKey] =
        useState<string>("");

    useEffect(() => {
        const listener = () =>
            setRevision(value =>
                value + 1
            );

        profileUiListeners.add(
            listener
        );

        return () =>
            void profileUiListeners.delete(
                listener
            );
    }, []);

    const sorted =
        Object.values(
            profiles.apps
        ).sort((a, b) =>
            b.lastSeen.localeCompare(
                a.lastSeen
            )
        );

    const effectiveKey =
        profiles.apps[selectedKey]
            ? selectedKey
            : (
                activeStream?.profileKey &&
                profiles.apps[
                    activeStream.profileKey
                ]
                    ? activeStream.profileKey
                    : sorted[0]?.key ?? ""
            );

    const profile =
        effectiveKey
            ? profiles.apps[effectiveKey]
            : null;

    const refresh = () =>
        setRevision(value =>
            value + 1
        );

    const selectProfile =
        async (kind: ProfileKind) => {
            if (!profile)
                return;

            await setProfileKind(
                profile.key,
                kind
            );

            refresh();
        };

    const updateCustom =
        async (
            patch: Partial<CustomValues>
        ) => {
            if (!profile)
                return;

            await updateProfileCustom(
                profile.key,
                patch
            );

            refresh();
        };

    return (
        <div className="vc-hdrfix-settings">
            <Forms.FormTitle tag="h3">
                Per-application HDR profiles
            </Forms.FormTitle>

            <Forms.FormText>
                Only applications you have actually streamed appear here. New applications remain Automatic until you override them.
            </Forms.FormText>

            {sorted.length === 0 ? (
                <div className="vc-hdrfix-settings-empty">
                    Stream an application once and it will appear here.
                </div>
            ) : (
                <>
                    <Forms.FormTitle
                        tag="h5"
                        className="vc-hdrfix-settings-label"
                    >
                        Application
                    </Forms.FormTitle>

                    <Select
                        options={sorted.map(item => ({
                            label:
                                item.exeName
                                    ? `${item.displayName} · ${item.exeName}`
                                    : item.displayName,
                            value: item.key
                        }))}
                        select={value => {
                            setSelectedKey(
                                String(value)
                            );
                        }}
                        isSelected={value =>
                            value === effectiveKey
                        }
                        serialize={value =>
                            String(value)
                        }
                        closeOnSelect
                    />

                    {profile && (
                        <div className="vc-hdrfix-settings-card">
                            <div className="vc-hdrfix-settings-app">
                                <strong>{profile.displayName}</strong>
                                <span>
                                    {profile.exePath ?? profile.exeName ?? profile.key}
                                </span>
                            </div>

                            <Forms.FormTitle
                                tag="h5"
                                className="vc-hdrfix-settings-label"
                            >
                                Profile
                            </Forms.FormTitle>

                            <Select
                                options={PROFILE_OPTIONS}
                                select={value =>
                                    void selectProfile(
                                        value as ProfileKind
                                    )
                                }
                                isSelected={value =>
                                    value === profile.kind
                                }
                                serialize={value =>
                                    String(value)
                                }
                                closeOnSelect
                            />

                            {(profile.kind === "renodx" ||
                                profile.kind === "custom") && (
                                <div className="vc-hdrfix-settings-advanced">
                                    <Forms.FormText>
                                        {profile.kind === "renodx"
                                            ? "RenoDX / ReShade remains fully customizable for this application."
                                            : "Custom source interpretation for this application."}
                                    </Forms.FormText>

                                    <Forms.FormTitle tag="h5" className="vc-hdrfix-settings-label">
                                        Primaries
                                    </Forms.FormTitle>

                                    <Select
                                        options={PRIMARIES_OPTIONS}
                                        select={value =>
                                            void updateCustom({
                                                primaries:
                                                    value as Primaries
                                            })
                                        }
                                        isSelected={value =>
                                            value ===
                                            profile.custom.primaries
                                        }
                                        serialize={value =>
                                            String(value)
                                        }
                                        closeOnSelect
                                    />

                                    <Forms.FormTitle tag="h5" className="vc-hdrfix-settings-label">
                                        Transfer
                                    </Forms.FormTitle>

                                    <Select
                                        options={TRANSFER_OPTIONS}
                                        select={value =>
                                            void updateCustom({
                                                transfer:
                                                    value as Transfer
                                            })
                                        }
                                        isSelected={value =>
                                            value ===
                                            profile.custom.transfer
                                        }
                                        serialize={value =>
                                            String(value)
                                        }
                                        closeOnSelect
                                    />

                                    <Forms.FormTitle tag="h5" className="vc-hdrfix-settings-label">
                                        HDR metadata
                                    </Forms.FormTitle>

                                    <Select
                                        options={METADATA_OPTIONS}
                                        select={value =>
                                            void updateCustom({
                                                metadata:
                                                    value as MetadataPolicy
                                            })
                                        }
                                        isSelected={value =>
                                            value ===
                                            profile.custom.metadata
                                        }
                                        serialize={value =>
                                            String(value)
                                        }
                                        closeOnSelect
                                    />

                                    <div className="vc-hdrfix-settings-numbers">
                                        <label>
                                            <span>SDR white</span>
                                            <TextInput
                                                type="number"
                                                value={String(
                                                    profile.custom.sdrWhite
                                                )}
                                                onChange={value =>
                                                    void updateCustom({
                                                        sdrWhite:
                                                            clampWhite(
                                                                value,
                                                                profile.custom.sdrWhite
                                                            )
                                                    })
                                                }
                                            />
                                        </label>

                                        <label>
                                            <span>Input max</span>
                                            <TextInput
                                                type="number"
                                                value={String(
                                                    profile.custom.inputMax
                                                )}
                                                onChange={value =>
                                                    void updateCustom({
                                                        inputMax:
                                                            clampPeak(
                                                                value,
                                                                profile.custom.inputMax
                                                            )
                                                    })
                                                }
                                            />
                                        </label>
                                    </div>
                                </div>
                            )}

                            <div className="vc-hdrfix-settings-actions">
                                <Button
                                    onClick={() =>
                                        void resetProfile(
                                            profile.key
                                        )
                                    }
                                >
                                    Reset to Automatic
                                </Button>

                                <Button
                                    color={Button.Colors.RED}
                                    onClick={() =>
                                        void forgetProfile(
                                            profile.key
                                        )
                                    }
                                >
                                    Forget profile
                                </Button>
                            </div>
                        </div>
                    )}
                </>
            )}
        </div>
    );
}

export default definePlugin({
    name: "DiscordHDRFix",
    description: "Corrects Discord Video Hook HDR capture with automatic and per-streamed-application profiles.",
    authors: [{
        name: "Discord HDR Fix",
        id: 0n
    }],
    tags: [
        "Developers",
        "Voice"
    ],
    settings,

    patches: [
        {
            find:
                "graphicsCaptureStaleFrameTimeoutMs",
            all: true,
            replacement: [
                {
                    match:
                        /hdrCaptureMode:([A-Za-z_$][\w$]*)/g,
                    replace:
                        "hdrCaptureMode:$self.forceSdrMode($1)"
                },
                {
                    match:
                        /useGraphicsCapture:([A-Za-z_$][\w$]*(?:\(\))?|![01])/g,
                    replace:
                        "useGraphicsCapture:$self.forceGraphicsCaptureOff($1)",
                    noWarn: true
                },
                {
                    match:
                        /useGraphicsCaptureApiLevel:([A-Za-z_$][\w$]*(?:\(\))?|[-]?\d+)/g,
                    replace:
                        "useGraphicsCaptureApiLevel:$self.forceGraphicsApiOff($1)",
                    noWarn: true
                },
                {
                    match:
                        /useVideoHook:([A-Za-z_$][\w$]*(?:\(\))?|![01])/g,
                    replace:
                        "useVideoHook:$self.forceVideoHookOn($1)",
                    noWarn: true
                }
            ]
        },
        {
            find: "sourcePid",
            all: true,
            replacement: [
                {
                    match:
                        /desktopSource:([A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)?)/g,
                    replace:
                        "desktopSource:$self.observeDesktopSource($1)",
                    noWarn: true
                }
            ]
        },

        // Proven stream-commit hook from v1.2.0. Keep the runtime MediaEngine
        // hook as a fallback too; confirmGoLiveOptions() is idempotent enough
        // because activateActualStream serializes the latest selection.
        {
            find: ".setGoLiveSource(",
            all: true,
            replacement: [
                {
                    match:
                        /([A-Za-z_$][\w$]*)\.setGoLiveSource\(([A-Za-z_$][\w$]*)\)/g,
                    replace:
                        "$1.setGoLiveSource($self.observeGoLiveOptions($2))",
                    noWarn: true
                }
            ]
        },

        {
            find: ".clearDesktopSource(",
            all: true,
            replacement: [
                {
                    match:
                        /([A-Za-z_$][\w$]*)\.clearDesktopSource\(\)/g,
                    replace:
                        "($self.onStreamEnded(),$1.clearDesktopSource())",
                    noWarn: true
                }
            ]
        }
    ],

    observeDesktopSource(source: any): any {
        return cacheCandidateSource(
            source
        );
    },

    observeGoLiveOptions(options: any): any {
        return confirmGoLiveOptions(options);
    },

    onStreamEnded(): void {
        streamEnded();
    },

    forceSdrMode(original: unknown): "never" {
        captureState.patchCalls++;
        captureState.lastSeenAt =
            Date.now();
        captureState.originalHdr =
            original;
        captureState.effectiveHdr =
            "never";

        return "never";
    },

    forceGraphicsCaptureOff(
        original: unknown
    ): boolean {
        captureState.originalGraphicsCapture =
            original;
        captureState.effectiveGraphicsCapture =
            false;
        captureState.lastSeenAt =
            Date.now();

        return false;
    },

    forceGraphicsApiOff(
        original: unknown
    ): number {
        captureState.originalGraphicsApi =
            original;
        captureState.effectiveGraphicsApi =
            0;
        captureState.lastSeenAt =
            Date.now();

        return 0;
    },

    forceVideoHookOn(
        original: unknown
    ): boolean {
        captureState.originalVideoHook =
            original;
        captureState.effectiveVideoHook =
            true;
        captureState.lastSeenAt =
            Date.now();

        return true;
    },

    toolboxActions: {
        "Show DiscordHDRFix Status":
            () =>
                alert(
                    combinedStatus()
                ),

        "Copy DiscordHDRFix Status":
            () => {
                const text =
                    combinedStatus();

                void navigator.clipboard
                    .writeText(text)
                    .catch(
                        () => alert(text)
                    );
            },

        "Reset Active App to Automatic":
            () => {
                const profile =
                    getActiveProfile();

                if (profile) {
                    void resetProfile(
                        profile.key
                    );
                }
            },

        "Show Logged Streamed Apps":
            () => {
                const rows =
                    Object.values(
                        profiles.apps
                    )
                        .sort((a, b) =>
                            b.lastSeen.localeCompare(
                                a.lastSeen
                            )
                        )
                        .map(profile =>
                            `${profile.displayName} | ${profile.exeName ?? "unresolved"} | ${profileKindLabel(profile.kind)} | ${profile.lastSeen}`
                        );

                alert(
                    rows.length
                        ? rows.join("\n")
                        : "DiscordHDRFix has not logged any actually-streamed applications yet."
                );
            }
    },

    start(): void {
        installGlobalDismiss();

        void (async () => {
            await loadProfiles();
            await applyActiveProfile(true);
            await startNativeHost();

            hookMediaConnections();

            connectionHookTimer =
                window.setInterval(
                    hookMediaConnections,
                    500
                );

            statusPollTimer =
                window.setInterval(
                    () =>
                        void pollNativeStatus(),
                    500
                );

            popoutTimer =
                window.setInterval(
                    refreshStreamMenuUi,
                    250
                );

            (window as any)
                .DiscordHDRFix = {
                    status:
                        combinedStatus,
                    profiles:
                        () => profiles,
                    activeStream:
                        () => activeStream,
                    nativeStatus:
                        () => nativeStatus,
                    apply:
                        () =>
                            applyActiveProfile(
                                true
                            )
                };
        })();
    },

    stop(): void {
        if (statusPollTimer != null)
            clearInterval(
                statusPollTimer
            );

        if (popoutTimer != null)
            clearInterval(
                popoutTimer
            );

        if (connectionHookTimer != null)
            clearInterval(
                connectionHookTimer
            );

        statusPollTimer = null;
        popoutTimer = null;
        connectionHookTimer = null;

        unhookMediaConnections();

        closeAllHdrFixMenus();
        removeStreamMenuRow();

        delete (window as any)
            .DiscordHDRFix;
    }
});
