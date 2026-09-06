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

async function pushHostConfig(): Promise<void> {
    try {
        const result = await Native.writeHostConfig(
            settings.store.enabled,
            settings.store.traceEnabled,
            settings.store.hostMode,
            settings.store.sdrWhiteLevel,
            settings.store.inputMaxLuminance,
            settings.store.customPrimaries,
            settings.store.customTransfer,
            settings.store.customMetadata,
            settings.store.rules
        );

        if (!result?.ok)
            logger.error("Failed to write dev-host config:", result);
    } catch (error) {
        logger.error("Failed to write dev-host config:", error);
    }
}

async function startNativeHost(): Promise<void> {
    try {
        await pushHostConfig();

        const result = await Native.startNativeFix();

        if (!result?.ok)
            logger.error("Native dev host launch failed:", result);
    } catch (error) {
        logger.error("Failed to launch native dev host:", error);
    }
}

const settings = definePluginSettings({
    enabled: {
        type: OptionType.BOOLEAN,
        description: "Master switch for runtime correction actions. Tracing can remain enabled while correction is disabled.",
        default: true,
        restartNeeded: false,
        onChange: () => void pushHostConfig()
    },
    hostMode: {
        type: OptionType.SELECT,
        description: "Observe is safest. Rules applies only matching caller rules. Force modes are live diagnostics.",
        options: [
            { label: "Observe only (no correction)", value: "observe", default: true },
            { label: "Caller rules", value: "rules" },
            { label: "Force SDR / preserve", value: "force_sdr" },
            { label: "Force HDR10 / Rec.2020 + PQ", value: "force_hdr10" },
            { label: "Force scRGB / Rec.709 + Linear", value: "force_scrgb" },
            { label: "Metadata only", value: "metadata_only" },
            { label: "Custom source interpretation", value: "custom" }
        ],
        restartNeeded: false,
        onChange: () => void pushHostConfig()
    },
    traceEnabled: {
        type: OptionType.BOOLEAN,
        description: "Record the most recent renderer callers and their source format/color metadata.",
        default: true,
        restartNeeded: false,
        onChange: () => void pushHostConfig()
    },
    sdrWhiteLevel: {
        type: OptionType.NUMBER,
        description: "Runtime HDR-to-SDR white level. Tested baseline: 460.",
        default: 460,
        restartNeeded: false,
        onChange: () => void pushHostConfig()
    },
    inputMaxLuminance: {
        type: OptionType.NUMBER,
        description: "Runtime HDR input maximum. Tested baseline: 1000.",
        default: 1000,
        restartNeeded: false,
        onChange: () => void pushHostConfig()
    },
    customPrimaries: {
        type: OptionType.SELECT,
        description: "Custom mode only. Override source gamut/primaries independently of the transfer function.",
        options: [
            { label: "Preserve Discord value", value: "preserve", default: true },
            { label: "Rec.709", value: "rec709" },
            { label: "Rec.2020", value: "rec2020" },
            { label: "Arc", value: "arc" }
        ],
        restartNeeded: false,
        onChange: () => void pushHostConfig()
    },
    customTransfer: {
        type: OptionType.SELECT,
        description: "Custom mode only. Override the source transfer function independently of gamut.",
        options: [
            { label: "Preserve Discord value", value: "preserve", default: true },
            { label: "Linear", value: "linear" },
            { label: "sRGB", value: "srgb" },
            { label: "ST.2084 / PQ", value: "pq" }
        ],
        restartNeeded: false,
        onChange: () => void pushHostConfig()
    },
    customMetadata: {
        type: OptionType.SELECT,
        description: "Custom mode only. Preserve Discord metadata, remove it, or inject the 460/1000 HDR metadata object.",
        options: [
            { label: "Preserve Discord metadata", value: "preserve", default: true },
            { label: "No HDR metadata", value: "none" },
            { label: "Inject HDR metadata", value: "inject" }
        ],
        restartNeeded: false,
        onChange: () => void pushHostConfig()
    },
    rules: {
        type: OptionType.STRING,
        description: "Live rules: callerRva,format|any,metadata(any|null|nonnull),action(preserve|sdr|hdr10|scrgb|metadata|custom). Separate rules with semicolons. Example: 0x3fd467,24,null,hdr10",
        default: "",
        restartNeeded: false,
        onChange: () => void pushHostConfig()
    }
});

function fmt(value: unknown): string {
    if (value === undefined) return "<not observed>";
    if (typeof value === "string") return JSON.stringify(value);
    return String(value);
}

function captureStatus(): string {
    return [
        "DiscordHDRFix capture routing v1.1.1-flex-source",
        "--------------------------------------------",
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
                "DiscordHDRFix native dev host v1.1.1",
                "-----------------------------------",
                "No dev-host status file found yet."
            ].join("\n");
        }

        return [
            "DiscordHDRFix native dev host v1.1.1",
            "-----------------------------------",
            JSON.stringify(result, null, 2)
        ].join("\n");
    } catch (error) {
        return `DiscordHDRFix native dev host v1.1.1\n-----------------------------------\nFailed to read status: ${String(error)}`;
    }
}

export default definePlugin({
    name: "DiscordHDRFix",
    description: "Hot-reload HDR/SDR tracing and correction host for Discord Video Hook development.",
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
        "Apply Dev Host Settings": () => void pushHostConfig(),
        "Start Native Dev Host": () => void startNativeHost(),
        "Show Native Dev Host Status": () => void nativeStatusText().then(text => alert(text)),
        "Copy Combined Dev Host Status": () => {
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
            apply: () => pushHostConfig(),
            startNativeHost: () => startNativeHost()
        };

        setTimeout(() => void startNativeHost(), 1500);
    },

    stop(): void {
        delete (window as any).DiscordHDRFix;
    }
});
