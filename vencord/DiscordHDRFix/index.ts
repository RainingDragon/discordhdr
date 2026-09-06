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
        await Native.writeToneMapConfig(
            settings.store.correctionEnabled,
            settings.store.sdrWhiteLevel,
            settings.store.inputMaxLuminance,
            settings.store.detectionMode
        );
    } catch (error) {
        logger.error("Failed to update HDR runtime config:", error);
    }
}

async function startNativeFix(): Promise<void> {
    try {
        await pushNativeConfig();
        const result = await Native.startNativeFix();
        if (!result?.ok)
            logger.error("Native HDR fix launch failed:", result);
    } catch (error) {
        logger.error("Failed to launch native HDR fix:", error);
    }
}

const settings = definePluginSettings({
    correctionEnabled: {
        type: OptionType.BOOLEAN,
        description: "Enable DiscordHDRFix. Automatic mode bypasses SDR frames and corrects only frames Discord marks as HDR.",
        default: true,
        restartNeeded: false,
        onChange: () => void pushNativeConfig()
    },
    detectionMode: {
        type: OptionType.SELECT,
        description: "Automatic is recommended. Manual modes are only for troubleshooting a game that Discord classifies incorrectly.",
        options: [
            { label: "Automatic (recommended)", value: "automatic", default: true },
            { label: "Force SDR / no tone mapping", value: "forceSdr" },
            { label: "Force HDR10 / Rec.2020 + PQ", value: "forceHdr10" },
            { label: "Force scRGB / Rec.709 + Linear", value: "forceScRgb" }
        ],
        restartNeeded: false,
        onChange: () => void pushNativeConfig()
    },
    sdrWhiteLevel: {
        type: OptionType.NUMBER,
        description: "HDR-to-SDR reference white passed to Discord's native HDR shader. Tested baseline: 460.",
        default: 460,
        restartNeeded: false,
        onChange: () => void pushNativeConfig()
    },
    inputMaxLuminance: {
        type: OptionType.NUMBER,
        description: "HDR input maximum luminance passed to Discord's native HDR shader. Tested baseline: 1000.",
        default: 1000,
        restartNeeded: false,
        onChange: () => void pushNativeConfig()
    }
});

function fmt(value: unknown): string {
    if (value === undefined) return "<not observed>";
    if (typeof value === "string") return JSON.stringify(value);
    return String(value);
}

function captureStatus(): string {
    return [
        "DiscordHDRFix capture status v1.0.1-auto-test",
        "---------------------------------------------",
        `Correction enabled:              ${settings.store.correctionEnabled}`,
        `Detection mode:                  ${settings.store.detectionMode}`,
        `SDR white level:                 ${settings.store.sdrWhiteLevel}`,
        `Input max luminance:             ${settings.store.inputMaxLuminance}`,
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

        if (result == null) {
            return [
                "DiscordHDRFix native status v1.0.1-auto-test",
                "------------------------------------------------",
                "No v1.0.1 status file found yet."
            ].join("\n");
        }

        return [
            "DiscordHDRFix native status v1.0.1-auto-test",
            "------------------------------------------------",
            JSON.stringify(result, null, 2)
        ].join("\n");
    } catch (error) {
        return `DiscordHDRFix native status v1.0.1-auto-test\n------------------------------------------------\nFailed to read status: ${String(error)}`;
    }
}

export default definePlugin({
    name: "DiscordHDRFix",
    description: "Automatically bypasses SDR frames and corrects Discord Video Hook HDR color/tone mapping.",
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
        "Apply HDR Runtime Settings": () => void pushNativeConfig(),
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
            startNativeFix: () => startNativeFix()
        };

        setTimeout(() => void startNativeFix(), 1500);
    },

    stop(): void {
        delete (window as any).DiscordHDRFix;
    }
});
