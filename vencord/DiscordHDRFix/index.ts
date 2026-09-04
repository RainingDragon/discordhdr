/*
 * DiscordHDRFix v0.5
 *
 * Keeps Discord on the D3D11 Video Hook path discovered in v0.4 and
 * controls the native pre-encode probe.
 *
 * v0.5 intentionally does NOT alter pixel data yet. It validates the
 * cc_encoder_add_frame_native seam on the user's Windows Discord build.
 */

import { definePluginSettings } from "@api/Settings";
import { Logger } from "@utils/Logger";
import definePlugin, { OptionType, PluginNative } from "@utils/types";

const logger = new Logger("DiscordHDRFix");
const Native = VencordNative.pluginHelpers.DiscordHDRFix as PluginNative<typeof import("./native")>;

type HdrMode = "never" | "always" | "original";

interface CaptureState {
    patchCalls: number;
    lastSeenAt: number | null;

    originalHdr: unknown;
    effectiveHdr: unknown;

    originalGraphicsCapture: unknown;
    effectiveGraphicsCapture: unknown;

    originalGraphicsApi: unknown;
    effectiveGraphicsApi: unknown;

    originalVideoHook: unknown;
    effectiveVideoHook: unknown;
}

const captureState: CaptureState = {
    patchCalls: 0,
    lastSeenAt: null,

    originalHdr: undefined,
    effectiveHdr: undefined,

    originalGraphicsCapture: undefined,
    effectiveGraphicsCapture: undefined,

    originalGraphicsApi: undefined,
    effectiveGraphicsApi: undefined,

    originalVideoHook: undefined,
    effectiveVideoHook: undefined
};

const settings = definePluginSettings({
    hdrMode: {
        type: OptionType.SELECT,
        description: "HDR mode sent to Discord. Keep this on never for the v0.5 Video Hook probe.",
        options: [
            { label: "Force SDR / never", value: "never", default: true },
            { label: "Force HDR / always", value: "always" },
            { label: "Discord original", value: "original" }
        ],
        restartNeeded: false
    },

    autoStartNativeProbe: {
        type: OptionType.BOOLEAN,
        description: "Start the native probe helper automatically after Discord starts.",
        default: true,
        restartNeeded: false
    },

    verboseLogging: {
        type: OptionType.BOOLEAN,
        description: "Log capture-option changes to DevTools.",
        default: false,
        restartNeeded: false
    }
});

function fmt(value: unknown): string {
    if (value === undefined) return "<not observed>";
    if (typeof value === "string") return JSON.stringify(value);
    return String(value);
}

function captureStatus(): string {
    return [
        "DiscordHDRFix capture status v0.5",
        "---------------------------------",
        `HDR setting:                    ${settings.store.hdrMode}`,
        `Original HDR mode:              ${fmt(captureState.originalHdr)}`,
        `Effective HDR mode:             ${fmt(captureState.effectiveHdr)}`,
        "",
        `Original Graphics Capture:      ${fmt(captureState.originalGraphicsCapture)}`,
        `Effective Graphics Capture:     ${fmt(captureState.effectiveGraphicsCapture)}`,
        `Original Graphics Capture API:  ${fmt(captureState.originalGraphicsApi)}`,
        `Effective Graphics Capture API: ${fmt(captureState.effectiveGraphicsApi)}`,
        `Original Video Hook:            ${fmt(captureState.originalVideoHook)}`,
        `Effective Video Hook:           ${fmt(captureState.effectiveVideoHook)}`,
        "",
        `Patch calls:                    ${captureState.patchCalls}`,
        `Last capture options:           ${captureState.lastSeenAt ? new Date(captureState.lastSeenAt).toLocaleString() : "<not yet>"}`
    ].join("\n");
}

function logChanged(name: string, value: unknown, previous: unknown): void {
    if (settings.store.verboseLogging || value !== previous)
        logger.info(`${name}:`, value);
}

async function startNativeProbe(): Promise<void> {
    try {
        const result = await Native.startProbe();
        logger.info("Native probe launch:", result);
    } catch (error) {
        logger.error("Failed to launch native probe:", error);
    }
}

async function nativeStatusText(): Promise<string> {
    try {
        const result = await Native.readProbeStatus();
        if (result == null)
            return "DiscordHDRFix native probe\n--------------------------\nNo native probe status file found yet.";

        return [
            "DiscordHDRFix native probe",
            "--------------------------",
            JSON.stringify(result, null, 2)
        ].join("\n");
    } catch (error) {
        return `DiscordHDRFix native probe\n--------------------------\nFailed to read status: ${String(error)}`;
    }
}

export default definePlugin({
    name: "DiscordHDRFix",
    description: "Routes Go Live through D3D11 Video Hook and probes Discord's native GPU frame handoff immediately before encoding.",
    authors: [{ name: "Discord HDR Fix", id: 0n }],
    tags: ["Developers", "Voice"],
    settings,

    patches: [
        {
            find: "graphicsCaptureStaleFrameTimeoutMs",
            all: true,
            replacement: [
                {
                    match: /hdrCaptureMode:([A-Za-z_$][\w$]*)/g,
                    replace: "hdrCaptureMode:$self.resolveHdrMode($1)"
                },
                {
                    match: /useGraphicsCapture:([A-Za-z_$][\w$]*(?:\(\))?|![01])/g,
                    replace: "useGraphicsCapture:$self.forceGraphicsCaptureOff($1)",
                    noWarn: true
                },
                {
                    match: /useGraphicsCaptureApiLevel:([A-Za-z_$][\w$]*(?:\(\))?|[-]?\d+)/g,
                    replace: "useGraphicsCaptureApiLevel:$self.forceGraphicsApiOff($1)",
                    noWarn: true
                },
                {
                    match: /useVideoHook:([A-Za-z_$][\w$]*(?:\(\))?|![01])/g,
                    replace: "useVideoHook:$self.forceVideoHookOn($1)",
                    noWarn: true
                }
            ]
        }
    ],

    resolveHdrMode(original: unknown): unknown {
        const previousOriginal = captureState.originalHdr;
        const previousEffective = captureState.effectiveHdr;

        const configured = settings.store.hdrMode as HdrMode;
        const effective = configured === "original" ? original : configured;

        captureState.patchCalls++;
        captureState.lastSeenAt = Date.now();
        captureState.originalHdr = original;
        captureState.effectiveHdr = effective;

        logChanged("original hdrCaptureMode", original, previousOriginal);
        logChanged("effective hdrCaptureMode", effective, previousEffective);
        return effective;
    },

    forceGraphicsCaptureOff(original: unknown): boolean {
        const previousOriginal = captureState.originalGraphicsCapture;
        const previousEffective = captureState.effectiveGraphicsCapture;

        captureState.originalGraphicsCapture = original;
        captureState.effectiveGraphicsCapture = false;
        captureState.lastSeenAt = Date.now();

        logChanged("original useGraphicsCapture", original, previousOriginal);
        logChanged("effective useGraphicsCapture", false, previousEffective);
        return false;
    },

    forceGraphicsApiOff(original: unknown): number {
        const previousOriginal = captureState.originalGraphicsApi;
        const previousEffective = captureState.effectiveGraphicsApi;

        captureState.originalGraphicsApi = original;
        captureState.effectiveGraphicsApi = 0;
        captureState.lastSeenAt = Date.now();

        logChanged("original useGraphicsCaptureApiLevel", original, previousOriginal);
        logChanged("effective useGraphicsCaptureApiLevel", 0, previousEffective);
        return 0;
    },

    forceVideoHookOn(original: unknown): boolean {
        const previousOriginal = captureState.originalVideoHook;
        const previousEffective = captureState.effectiveVideoHook;

        captureState.originalVideoHook = original;
        captureState.effectiveVideoHook = true;
        captureState.lastSeenAt = Date.now();

        logChanged("original useVideoHook", original, previousOriginal);
        logChanged("effective useVideoHook", true, previousEffective);
        return true;
    },

    toolboxActions: {
        "Start Native Frame Probe": () => void startNativeProbe(),

        "Show Native Frame Probe Status": () => {
            void nativeStatusText().then(text => alert(text));
        },

        "Copy Combined HDR Fix Status": () => {
            void nativeStatusText().then(native => {
                const text = `${captureStatus()}\n\n${native}`;
                void navigator.clipboard.writeText(text).catch(() => alert(text));
            });
        }
    },

    start(): void {
        (window as any).DiscordHDRFix = {
            captureStatus,
            captureState,
            nativeStatus: () => Native.readProbeStatus(),
            startProbe: () => Native.startProbe()
        };

        if (settings.store.autoStartNativeProbe) {
            setTimeout(() => void startNativeProbe(), 2000);
        }

        logger.info("Started v0.5. Capture target: Video Hook=true, Graphics Capture=false, Graphics API=0.");
    },

    stop(): void {
        delete (window as any).DiscordHDRFix;
        logger.info("Stopped. The injected probe remains loaded until Discord exits.");
    }
});
