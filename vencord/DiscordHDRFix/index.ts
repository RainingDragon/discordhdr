import { definePluginSettings } from "@api/Settings";
import { Logger } from "@utils/Logger";
import definePlugin, { OptionType, PluginNative } from "@utils/types";

const logger = new Logger("DiscordHDRFix");
const Native = VencordNative.pluginHelpers.DiscordHDRFix as PluginNative<typeof import("./native")>;

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

async function pushNativeConfig(): Promise<void> {
    try {
        const result = await Native.writeToneMapConfig(
            settings.store.toneMapEnabled,
            settings.store.sdrWhiteLevel,
            settings.store.inputMaxLuminance
        );
        logger.info("Tone-map config updated:", result);
    } catch (error) {
        logger.error("Failed to update tone-map config:", error);
    }
}

async function startNativeFix(): Promise<void> {
    try {
        await pushNativeConfig();
        const result = await Native.startNativeFix();
        logger.info("Native HDR fix launch:", result);
    } catch (error) {
        logger.error("Failed to launch native HDR fix:", error);
    }
}

function setPreset(white: number, peak: number): void {
    settings.store.sdrWhiteLevel = white;
    settings.store.inputMaxLuminance = peak;
    void pushNativeConfig();
}

const settings = definePluginSettings({
    toneMapEnabled: {
        type: OptionType.BOOLEAN,
        description: "Use Discord's built-in HDR shader on the D3D11 Video Hook path.",
        default: true,
        restartNeeded: false,
        onChange: () => void pushNativeConfig()
    },
    sdrWhiteLevel: {
        type: OptionType.NUMBER,
        description: "SDR reference white in nits. Discord's Windows fallback is 200. This is not the monitor HDR peak.",
        default: 200,
        restartNeeded: false,
        onChange: () => void pushNativeConfig()
    },
    inputMaxLuminance: {
        type: OptionType.NUMBER,
        description: "Maximum HDR input luminance in nits. AW3423DWF: try 460 for True Black 400 or 1000 for Peak 1000.",
        default: 1000,
        restartNeeded: false,
        onChange: () => void pushNativeConfig()
    },
    verboseLogging: {
        type: OptionType.BOOLEAN,
        description: "Log Discord capture-option changes to DevTools.",
        default: false,
        restartNeeded: false
    }
});

function fmt(value: unknown): string {
    if (value === undefined) return "<not observed>";
    if (typeof value === "string") return JSON.stringify(value);
    return String(value);
}

function logChanged(name: string, value: unknown, previous: unknown): void {
    if (settings.store.verboseLogging || value !== previous)
        logger.info(`${name}:`, value);
}

function captureStatus(): string {
    return [
        "DiscordHDRFix capture status v0.6.1",
        "---------------------------------",
        `Tone map enabled:                ${settings.store.toneMapEnabled}`,
        `SDR white level:                 ${settings.store.sdrWhiteLevel} nits`,
        `Input max luminance:             ${settings.store.inputMaxLuminance} nits`,
        "",
        `Original HDR mode:               ${fmt(captureState.originalHdr)}`,
        `Effective HDR mode:              ${fmt(captureState.effectiveHdr)}`,
        `Original Graphics Capture:       ${fmt(captureState.originalGraphicsCapture)}`,
        `Effective Graphics Capture:      ${fmt(captureState.effectiveGraphicsCapture)}`,
        `Original Graphics Capture API:   ${fmt(captureState.originalGraphicsApi)}`,
        `Effective Graphics Capture API:  ${fmt(captureState.effectiveGraphicsApi)}`,
        `Original Video Hook:             ${fmt(captureState.originalVideoHook)}`,
        `Effective Video Hook:            ${fmt(captureState.effectiveVideoHook)}`,
        "",
        `Patch calls:                     ${captureState.patchCalls}`,
        `Last capture options:            ${captureState.lastSeenAt ? new Date(captureState.lastSeenAt).toLocaleString() : "<not yet>"}`
    ].join("\n");
}

async function nativeStatusText(): Promise<string> {
    try {
        const result = await Native.readNativeStatus();
        if (result == null)
            return "DiscordHDRFix native status v0.6.1\n--------------------------------\nNo v0.6 native status file found yet.";
        return `DiscordHDRFix native status v0.6.1\n--------------------------------\n${JSON.stringify(result, null, 2)}`;
    } catch (error) {
        return `DiscordHDRFix native status v0.6.1\n--------------------------------\nFailed to read status: ${String(error)}`;
    }
}

export default definePlugin({
    name: "DiscordHDRFix",
    description: "Forces D3D11 Video Hook and supplies the complete HDR metadata Discord omits so its own HDR→SDR shader can run.",
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
                    replace: "hdrCaptureMode:$self.forceSdrMode($1)"
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

    forceSdrMode(original: unknown): "never" {
        const prevOriginal = captureState.originalHdr;
        const prevEffective = captureState.effectiveHdr;
        captureState.patchCalls++;
        captureState.lastSeenAt = Date.now();
        captureState.originalHdr = original;
        captureState.effectiveHdr = "never";
        logChanged("original hdrCaptureMode", original, prevOriginal);
        logChanged("effective hdrCaptureMode", "never", prevEffective);
        return "never";
    },

    forceGraphicsCaptureOff(original: unknown): boolean {
        const prevOriginal = captureState.originalGraphicsCapture;
        const prevEffective = captureState.effectiveGraphicsCapture;
        captureState.originalGraphicsCapture = original;
        captureState.effectiveGraphicsCapture = false;
        captureState.lastSeenAt = Date.now();
        logChanged("original useGraphicsCapture", original, prevOriginal);
        logChanged("effective useGraphicsCapture", false, prevEffective);
        return false;
    },

    forceGraphicsApiOff(original: unknown): number {
        const prevOriginal = captureState.originalGraphicsApi;
        const prevEffective = captureState.effectiveGraphicsApi;
        captureState.originalGraphicsApi = original;
        captureState.effectiveGraphicsApi = 0;
        captureState.lastSeenAt = Date.now();
        logChanged("original useGraphicsCaptureApiLevel", original, prevOriginal);
        logChanged("effective useGraphicsCaptureApiLevel", 0, prevEffective);
        return 0;
    },

    forceVideoHookOn(original: unknown): boolean {
        const prevOriginal = captureState.originalVideoHook;
        const prevEffective = captureState.effectiveVideoHook;
        captureState.originalVideoHook = original;
        captureState.effectiveVideoHook = true;
        captureState.lastSeenAt = Date.now();
        logChanged("original useVideoHook", original, prevOriginal);
        logChanged("effective useVideoHook", true, prevEffective);
        return true;
    },

    toolboxActions: {
        "Apply HDR Tone Map Settings": () => void pushNativeConfig(),
        "AW3423DWF: True Black 400 Preset": () => setPreset(200, 460),
        "AW3423DWF: Peak 1000 Preset": () => setPreset(200, 1000),
        "Start Native HDR Fix": () => void startNativeFix(),
        "Show Native HDR Fix Status": () => void nativeStatusText().then(text => alert(text)),
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
            nativeStatus: () => Native.readNativeStatus(),
            apply: () => pushNativeConfig(),
            startNativeFix: () => startNativeFix(),
            presetTrueBlack400: () => setPreset(200, 460),
            presetPeak1000: () => setPreset(200, 1000)
        };

        setTimeout(() => void startNativeFix(), 1500);
        logger.info("Started v0.6.1. HDR=never, Video Hook=true, Graphics Capture=false, Graphics API=0.");
    },

    stop(): void {
        delete (window as any).DiscordHDRFix;
        logger.info("Stopped. The injected DLL remains loaded until Discord exits.");
    }
});
