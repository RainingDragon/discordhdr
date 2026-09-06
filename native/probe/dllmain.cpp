#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#pragma intrinsic(_ReturnAddress)

constexpr DWORD kExpectedTimeDateStamp = 0x6a95b8a4;
constexpr DWORD kExpectedSizeOfImage = 0x00fd8000;
constexpr std::uintptr_t kRendererWrapperRva = 0x0052cad0;
constexpr std::size_t kPatchedPrologueSize = 16;

constexpr std::size_t kSourceFormatOffset = 0x178;
constexpr std::size_t kSourcePrimariesOffset = 0x17c;
constexpr std::size_t kSourceTransferOffset = 0x17d;

constexpr std::size_t kTraceCapacity = 4096;
constexpr std::size_t kMaxRules = 32;

constexpr std::uint32_t kDxgiR16G16B16A16Float = 10;
constexpr std::uint32_t kDxgiR10G10B10A2Unorm = 24;

enum class HostMode : int {
    Observe = 0,
    Rules = 1,
    ForceSdr = 2,
    ForceHdr10 = 3,
    ForceScRgb = 4,
    MetadataOnly = 5,
    Custom = 6
};

enum class Action : int {
    Preserve = 0,
    Sdr = 1,
    Hdr10 = 2,
    ScRgb = 3,
    MetadataOnly = 4,
    Custom = 5
};

enum class CustomMetadataPolicy : int {
    Preserve = 0,
    None = 1,
    Inject = 2
};

enum class MetadataMatch : int {
    Any = 0,
    Null = 1,
    NonNull = 2
};

struct HdrMetadata {
    float sdrWhiteLevel;
    float inputMaxLuminance;
    std::uint8_t state;
    std::uint8_t padding[3];
};

static_assert(sizeof(HdrMetadata) == 12);

struct Rule {
    std::uintptr_t callerRva = 0;
    int format = -1; // -1 = wildcard
    MetadataMatch metadataMatch = MetadataMatch::Any;
    Action action = Action::Preserve;
};

struct RuntimeConfig {
    unsigned generation = 1;
    bool enabled = true;
    bool traceEnabled = true;
    HostMode mode = HostMode::Observe;
    float sdrWhite = 460.0f;
    float inputMax = 1000.0f;

    // Orthogonal live source interpretation controls used by HostMode::Custom
    // and Action::Custom. -1 means preserve Discord's original enum.
    int customPrimaries = -1; // 0=Rec709, 1=Rec2020, 2=Arc
    int customTransfer = -1;  // 0=Linear, 1=sRGB, 2=ST2084/PQ
    CustomMetadataPolicy customMetadata = CustomMetadataPolicy::Preserve;

    std::size_t ruleCount = 0;
    Rule rules[kMaxRules]{};
};

struct TraceEvent {
    volatile LONG64 sequence;
    volatile LONG64 callerRva;
    volatile LONG64 sourcePointer;
    volatile LONG64 originalMetadataPointer;
    volatile LONG format;
    volatile LONG originalPrimaries;
    volatile LONG originalTransfer;
    volatile LONG effectivePrimaries;
    volatile LONG effectiveTransfer;
    volatile LONG action;
    volatile LONG sourceReadable;
};

struct CallerBucket {
    std::uintptr_t callerRva = 0;
    unsigned long long count = 0;
    unsigned long long metadataNull = 0;
    unsigned long long metadataNonNull = 0;
    unsigned long long sourceUnreadable = 0;
    LONG lastFormat = -1;
    LONG lastPrimaries = -1;
    LONG lastTransfer = -1;
    LONG lastAction = 0;
};

using RendererFn = void* (__fastcall*)(
    void*, void*, void*, void*,
    void*, void*, void*, void*, void*
);

std::atomic<const RuntimeConfig*> g_activeConfig{nullptr};

alignas(16) HdrMetadata g_metadataRing[256]{};
volatile LONG g_metadataIndex = 0;

alignas(64) TraceEvent g_trace[kTraceCapacity]{};
volatile LONG64 g_traceWriteIndex = 0;

volatile LONG g_hookInstalled = 0;
volatile LONG64 g_totalRendererCalls = 0;
volatile LONG64 g_totalModifiedCalls = 0;
volatile LONG64 g_totalMetadataInjected = 0;
volatile LONG64 g_totalSourceOverrides = 0;

volatile LONG64 g_lastCallerRva = 0;
volatile LONG g_lastFormat = -1;
volatile LONG g_lastOriginalPrimaries = -1;
volatile LONG g_lastOriginalTransfer = -1;
volatile LONG g_lastEffectivePrimaries = -1;
volatile LONG g_lastEffectiveTransfer = -1;
volatile LONG g_lastAction = 0;
volatile LONG g_lastSourceReadable = 0;
volatile LONG g_lastMetadataWasNull = -1;

HMODULE g_voiceModule = nullptr;
std::uintptr_t g_voiceBase = 0;
std::size_t g_voiceSize = 0;

std::uint8_t* g_trampoline = nullptr;
RendererFn g_originalRenderer = nullptr;
std::uint8_t g_originalPrologue[kPatchedPrologueSize]{};

wchar_t g_statusPath[MAX_PATH]{};
wchar_t g_configPath[MAX_PATH]{};
char g_signature[192] = "waiting for discord_voice.node";
char g_error[512]{};
char g_configError[512]{};

void SetSignature(const char* text) {
    strncpy_s(g_signature, text ? text : "", _TRUNCATE);
}

void SetError(const char* text) {
    strncpy_s(g_error, text ? text : "", _TRUNCATE);
}

void SetConfigError(const char* text) {
    strncpy_s(g_configError, text ? text : "", _TRUNCATE);
}

const char* HostModeName(HostMode mode) {
    switch (mode) {
    case HostMode::Observe: return "observe";
    case HostMode::Rules: return "rules";
    case HostMode::ForceSdr: return "force_sdr";
    case HostMode::ForceHdr10: return "force_hdr10";
    case HostMode::ForceScRgb: return "force_scrgb";
    case HostMode::MetadataOnly: return "metadata_only";
    case HostMode::Custom: return "custom";
    default: return "observe";
    }
}

const char* ActionName(Action action) {
    switch (action) {
    case Action::Preserve: return "preserve";
    case Action::Sdr: return "sdr_no_metadata";
    case Action::Hdr10: return "hdr10_rec2020_pq";
    case Action::ScRgb: return "scrgb_rec709_linear";
    case Action::MetadataOnly: return "metadata_only";
    case Action::Custom: return "custom";
    default: return "preserve";
    }
}

const char* PrimariesName(LONG value) {
    switch (value) {
    case 0: return "Rec709";
    case 1: return "Rec2020";
    case 2: return "Arc";
    default: return "unknown";
    }
}

const char* TransferName(LONG value) {
    switch (value) {
    case 0: return "Linear";
    case 1: return "sRGB";
    case 2: return "SMPTE_ST2084";
    default: return "unknown";
    }
}

const char* FormatName(LONG value) {
    switch (value) {
    case 10: return "R16G16B16A16_FLOAT";
    case 24: return "R10G10B10A2_UNORM";
    case 29: return "R8G8B8A8_UNORM_SRGB";
    case 91: return "B8G8R8A8_UNORM_SRGB";
    case 93: return "B8G8R8X8_UNORM_SRGB";
    default: return "other";
    }
}

void BuildPaths() {
    wchar_t temp[MAX_PATH]{};
    DWORD tempLen = GetTempPathW(MAX_PATH, temp);
    if (tempLen == 0 || tempLen >= MAX_PATH)
        wcscpy_s(temp, L".\\");

    swprintf_s(
        g_statusPath,
        L"%sDiscordHDRFix-devhost-%lu.json",
        temp,
        GetCurrentProcessId()
    );

    wchar_t localAppData[MAX_PATH]{};
    DWORD envLen = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        localAppData,
        MAX_PATH
    );

    if (envLen == 0 || envLen >= MAX_PATH) {
        wcscpy_s(g_configPath, L"host.cfg");
        return;
    }

    swprintf_s(
        g_configPath,
        L"%s\\DiscordHDRFix\\host.cfg",
        localAppData
    );
}

bool EqualBytes(
    const std::uint8_t* address,
    const std::uint8_t* expected,
    std::size_t size
) {
    return std::memcmp(address, expected, size) == 0;
}

bool VerifyDiscordBuild(HMODULE voice) {
    auto* base = reinterpret_cast<std::uint8_t*>(voice);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        SetSignature("DOS header mismatch");
        SetError("invalid discord_voice.node DOS header");
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        base + dos->e_lfanew
    );

    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        SetSignature("PE header mismatch");
        SetError("invalid discord_voice.node PE32+ header");
        return false;
    }

    if (nt->FileHeader.TimeDateStamp != kExpectedTimeDateStamp ||
        nt->OptionalHeader.SizeOfImage != kExpectedSizeOfImage) {
        SetSignature("unsupported discord_voice.node build");
        SetError("Discord build changed; dev host refused to patch");
        return false;
    }

    constexpr std::uint8_t wrapperPrologue[kPatchedPrologueSize] = {
        0x41, 0x57,
        0x41, 0x56,
        0x41, 0x55,
        0x41, 0x54,
        0x56,
        0x57,
        0x55,
        0x53,
        0x48, 0x83, 0xec, 0x68
    };

    auto* wrapper = base + kRendererWrapperRva;

    if (!EqualBytes(
            wrapper,
            wrapperPrologue,
            sizeof(wrapperPrologue))) {
        SetSignature("shared renderer signature mismatch");
        SetError("renderer prologue changed; dev host refused to patch");
        return false;
    }

    std::memcpy(
        g_originalPrologue,
        wrapperPrologue,
        sizeof(g_originalPrologue)
    );

    g_voiceModule = voice;
    g_voiceBase = reinterpret_cast<std::uintptr_t>(base);
    g_voiceSize = nt->OptionalHeader.SizeOfImage;

    SetSignature("matched exact analyzed shared renderer wrapper");
    SetError("");
    return true;
}

void WriteAbsoluteIndirectJump(
    std::uint8_t* destination,
    std::uintptr_t target
) {
    destination[0] = 0xff;
    destination[1] = 0x25;
    destination[2] = 0x00;
    destination[3] = 0x00;
    destination[4] = 0x00;
    destination[5] = 0x00;

    std::memcpy(
        destination + 6,
        &target,
        sizeof(target)
    );
}

bool BuildTrampoline(HMODULE voice) {
    auto* base = reinterpret_cast<std::uint8_t*>(voice);
    auto* wrapper = base + kRendererWrapperRva;

    constexpr SIZE_T trampolineSize = 64;

    g_trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(
            nullptr,
            trampolineSize,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_EXECUTE_READWRITE
        )
    );

    if (!g_trampoline) {
        SetError("VirtualAlloc failed for renderer trampoline");
        return false;
    }

    std::memset(g_trampoline, 0xcc, trampolineSize);

    std::memcpy(
        g_trampoline,
        g_originalPrologue,
        kPatchedPrologueSize
    );

    WriteAbsoluteIndirectJump(
        g_trampoline + kPatchedPrologueSize,
        reinterpret_cast<std::uintptr_t>(
            wrapper + kPatchedPrologueSize
        )
    );

    FlushInstructionCache(
        GetCurrentProcess(),
        g_trampoline,
        trampolineSize
    );

    g_originalRenderer =
        reinterpret_cast<RendererFn>(g_trampoline);

    return true;
}

bool SafeReadSource(
    void* sourcePtr,
    LONG& format,
    LONG& primaries,
    LONG& transfer
) {
    format = -1;
    primaries = -1;
    transfer = -1;

    if (!sourcePtr)
        return false;

    __try {
        auto* p = static_cast<std::uint8_t*>(sourcePtr);

        format = static_cast<LONG>(
            *reinterpret_cast<std::uint32_t*>(
                p + kSourceFormatOffset
            )
        );

        primaries = static_cast<LONG>(
            *(p + kSourcePrimariesOffset)
        );

        transfer = static_cast<LONG>(
            *(p + kSourceTransferOffset)
        );

        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWriteSource(
    void* sourcePtr,
    std::uint8_t primaries,
    std::uint8_t transfer
) {
    if (!sourcePtr)
        return false;

    __try {
        auto* p = static_cast<std::uint8_t*>(sourcePtr);
        *(p + kSourcePrimariesOffset) = primaries;
        *(p + kSourceTransferOffset) = transfer;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uintptr_t CallerRvaFromAddress(std::uintptr_t address) {
    if (address < g_voiceBase ||
        address >= g_voiceBase + g_voiceSize) {
        return 0;
    }

    return address - g_voiceBase;
}

bool MetadataMatches(
    MetadataMatch match,
    void* originalMetadata
) {
    switch (match) {
    case MetadataMatch::Any:
        return true;
    case MetadataMatch::Null:
        return originalMetadata == nullptr;
    case MetadataMatch::NonNull:
        return originalMetadata != nullptr;
    default:
        return false;
    }
}

Action SelectAction(
    const RuntimeConfig& cfg,
    std::uintptr_t callerRva,
    LONG format,
    void* originalMetadata
) {
    if (!cfg.enabled)
        return Action::Preserve;

    switch (cfg.mode) {
    case HostMode::Observe:
        return Action::Preserve;

    case HostMode::ForceSdr:
        return Action::Sdr;

    case HostMode::ForceHdr10:
        return Action::Hdr10;

    case HostMode::ForceScRgb:
        return Action::ScRgb;

    case HostMode::MetadataOnly:
        return Action::MetadataOnly;

    case HostMode::Custom:
        return Action::Custom;

    case HostMode::Rules:
        break;

    default:
        return Action::Preserve;
    }

    for (std::size_t i = 0; i < cfg.ruleCount; ++i) {
        const Rule& rule = cfg.rules[i];

        if (rule.callerRva != callerRva)
            continue;

        if (rule.format >= 0 &&
            rule.format != format) {
            continue;
        }

        if (!MetadataMatches(
                rule.metadataMatch,
                originalMetadata)) {
            continue;
        }

        return rule.action;
    }

    return Action::Preserve;
}

HdrMetadata* PublishMetadata(float white, float peak) {
    LONG seq = InterlockedIncrement(&g_metadataIndex);

    const unsigned index =
        static_cast<unsigned>(seq) % 256u;

    HdrMetadata& m = g_metadataRing[index];

    m.sdrWhiteLevel = white;
    m.inputMaxLuminance = peak;
    m.state = 1;
    m.padding[0] = 0;
    m.padding[1] = 0;
    m.padding[2] = 0;

    MemoryBarrier();

    return &m;
}

void Trace(
    const RuntimeConfig& cfg,
    std::uintptr_t callerRva,
    void* sourcePtr,
    void* originalMetadata,
    LONG format,
    LONG originalPrimaries,
    LONG originalTransfer,
    LONG effectivePrimaries,
    LONG effectiveTransfer,
    Action action,
    bool sourceReadable
) {
    if (!cfg.traceEnabled)
        return;

    const unsigned long long sequence =
        static_cast<unsigned long long>(
            InterlockedIncrement64(
                &g_traceWriteIndex
            )
        );

    const std::size_t index =
        static_cast<std::size_t>(
            (sequence - 1) % kTraceCapacity
        );

    TraceEvent& e = g_trace[index];

    e.callerRva =
        static_cast<LONG64>(callerRva);

    e.sourcePointer =
        reinterpret_cast<LONG64>(sourcePtr);

    e.originalMetadataPointer =
        reinterpret_cast<LONG64>(originalMetadata);

    e.format = format;
    e.originalPrimaries = originalPrimaries;
    e.originalTransfer = originalTransfer;
    e.effectivePrimaries = effectivePrimaries;
    e.effectiveTransfer = effectiveTransfer;
    e.action = static_cast<LONG>(action);
    e.sourceReadable = sourceReadable ? 1 : 0;

    MemoryBarrier();

    InterlockedExchange64(
        &e.sequence,
        static_cast<LONG64>(sequence)
    );
}

void* __fastcall HookRenderer(
    void* a1,
    void* a2,
    void* a3,
    void* a4,
    void* a5,
    void* a6,
    void* a7,
    void* a8,
    void* a9
) {
    InterlockedIncrement64(&g_totalRendererCalls);

    const auto returnAddress =
        reinterpret_cast<std::uintptr_t>(
            _ReturnAddress()
        );

    const std::uintptr_t callerRva =
        CallerRvaFromAddress(returnAddress);

    LONG format = -1;
    LONG originalPrimaries = -1;
    LONG originalTransfer = -1;

    const bool sourceReadable =
        SafeReadSource(
            a4,
            format,
            originalPrimaries,
            originalTransfer
        );

    LONG effectivePrimaries = originalPrimaries;
    LONG effectiveTransfer = originalTransfer;

    const RuntimeConfig* cfgPtr =
        g_activeConfig.load(
            std::memory_order_acquire
        );

    RuntimeConfig fallback{};
    const RuntimeConfig& cfg =
        cfgPtr ? *cfgPtr : fallback;

    const Action action =
        SelectAction(
            cfg,
            callerRva,
            format,
            a9
        );

    bool sourceChanged = false;
    void* effectiveMetadata = a9;

    if (action == Action::Sdr) {
        effectiveMetadata = nullptr;
    }
    else if (action == Action::Hdr10) {
        if (sourceReadable) {
            sourceChanged =
                SafeWriteSource(
                    a4,
                    1,
                    2
                );

            if (sourceChanged) {
                effectivePrimaries = 1;
                effectiveTransfer = 2;
                InterlockedIncrement64(
                    &g_totalSourceOverrides
                );
            }
        }

        effectiveMetadata =
            PublishMetadata(
                cfg.sdrWhite,
                cfg.inputMax
            );

        InterlockedIncrement64(
            &g_totalMetadataInjected
        );
    }
    else if (action == Action::ScRgb) {
        if (sourceReadable) {
            sourceChanged =
                SafeWriteSource(
                    a4,
                    0,
                    0
                );

            if (sourceChanged) {
                effectivePrimaries = 0;
                effectiveTransfer = 0;
                InterlockedIncrement64(
                    &g_totalSourceOverrides
                );
            }
        }

        effectiveMetadata =
            PublishMetadata(
                cfg.sdrWhite,
                cfg.inputMax
            );

        InterlockedIncrement64(
            &g_totalMetadataInjected
        );
    }
    else if (action == Action::MetadataOnly) {
        effectiveMetadata =
            PublishMetadata(
                cfg.sdrWhite,
                cfg.inputMax
            );

        InterlockedIncrement64(
            &g_totalMetadataInjected
        );
    }
    else if (action == Action::Custom) {
        if (sourceReadable) {
            const int desiredPrimaries =
                cfg.customPrimaries >= 0
                    ? cfg.customPrimaries
                    : originalPrimaries;

            const int desiredTransfer =
                cfg.customTransfer >= 0
                    ? cfg.customTransfer
                    : originalTransfer;

            if (desiredPrimaries != originalPrimaries ||
                desiredTransfer != originalTransfer) {
                sourceChanged =
                    SafeWriteSource(
                        a4,
                        static_cast<std::uint8_t>(desiredPrimaries),
                        static_cast<std::uint8_t>(desiredTransfer)
                    );

                if (sourceChanged) {
                    effectivePrimaries = desiredPrimaries;
                    effectiveTransfer = desiredTransfer;

                    InterlockedIncrement64(
                        &g_totalSourceOverrides
                    );
                }
            }
        }

        switch (cfg.customMetadata) {
        case CustomMetadataPolicy::None:
            effectiveMetadata = nullptr;
            break;

        case CustomMetadataPolicy::Inject:
            effectiveMetadata =
                PublishMetadata(
                    cfg.sdrWhite,
                    cfg.inputMax
                );

            InterlockedIncrement64(
                &g_totalMetadataInjected
            );
            break;

        case CustomMetadataPolicy::Preserve:
        default:
            effectiveMetadata = a9;
            break;
        }
    }

    if (action != Action::Preserve) {
        InterlockedIncrement64(
            &g_totalModifiedCalls
        );
    }

    InterlockedExchange64(
        &g_lastCallerRva,
        static_cast<LONG64>(callerRva)
    );

    InterlockedExchange(&g_lastFormat, format);

    InterlockedExchange(
        &g_lastOriginalPrimaries,
        originalPrimaries
    );

    InterlockedExchange(
        &g_lastOriginalTransfer,
        originalTransfer
    );

    InterlockedExchange(
        &g_lastEffectivePrimaries,
        effectivePrimaries
    );

    InterlockedExchange(
        &g_lastEffectiveTransfer,
        effectiveTransfer
    );

    InterlockedExchange(
        &g_lastAction,
        static_cast<LONG>(action)
    );

    InterlockedExchange(
        &g_lastSourceReadable,
        sourceReadable ? 1 : 0
    );

    InterlockedExchange(
        &g_lastMetadataWasNull,
        a9 == nullptr ? 1 : 0
    );

    Trace(
        cfg,
        callerRva,
        a4,
        a9,
        format,
        originalPrimaries,
        originalTransfer,
        effectivePrimaries,
        effectiveTransfer,
        action,
        sourceReadable
    );

    void* result = g_originalRenderer(
        a1, a2, a3, a4,
        a5, a6, a7, a8,
        effectiveMetadata
    );

    if (sourceChanged) {
        SafeWriteSource(
            a4,
            static_cast<std::uint8_t>(
                originalPrimaries
            ),
            static_cast<std::uint8_t>(
                originalTransfer
            )
        );
    }

    return result;
}

bool PatchRendererEntry(HMODULE voice) {
    auto* base = reinterpret_cast<std::uint8_t*>(voice);
    auto* wrapper = base + kRendererWrapperRva;

    if (!BuildTrampoline(voice))
        return false;

    std::uint8_t patch[kPatchedPrologueSize]{};

    WriteAbsoluteIndirectJump(
        patch,
        reinterpret_cast<std::uintptr_t>(
            &HookRenderer
        )
    );

    patch[14] = 0x90;
    patch[15] = 0x90;

    DWORD oldProtection = 0;

    if (!VirtualProtect(
            wrapper,
            sizeof(patch),
            PAGE_EXECUTE_READWRITE,
            &oldProtection)) {
        SetError("VirtualProtect failed for shared renderer");
        return false;
    }

    std::memcpy(wrapper, patch, sizeof(patch));

    DWORD ignored = 0;

    VirtualProtect(
        wrapper,
        sizeof(patch),
        oldProtection,
        &ignored
    );

    FlushInstructionCache(
        GetCurrentProcess(),
        wrapper,
        sizeof(patch)
    );

    return true;
}

HostMode ParseHostMode(const char* value) {
    if (_stricmp(value, "rules") == 0)
        return HostMode::Rules;

    if (_stricmp(value, "force_sdr") == 0)
        return HostMode::ForceSdr;

    if (_stricmp(value, "force_hdr10") == 0)
        return HostMode::ForceHdr10;

    if (_stricmp(value, "force_scrgb") == 0)
        return HostMode::ForceScRgb;

    if (_stricmp(value, "metadata_only") == 0)
        return HostMode::MetadataOnly;

    if (_stricmp(value, "custom") == 0)
        return HostMode::Custom;

    return HostMode::Observe;
}

Action ParseAction(const char* value) {
    if (_stricmp(value, "sdr") == 0)
        return Action::Sdr;

    if (_stricmp(value, "hdr10") == 0)
        return Action::Hdr10;

    if (_stricmp(value, "scrgb") == 0)
        return Action::ScRgb;

    if (_stricmp(value, "metadata") == 0)
        return Action::MetadataOnly;

    if (_stricmp(value, "custom") == 0)
        return Action::Custom;

    return Action::Preserve;
}

MetadataMatch ParseMetadataMatch(
    const char* value
) {
    if (_stricmp(value, "null") == 0)
        return MetadataMatch::Null;

    if (_stricmp(value, "nonnull") == 0)
        return MetadataMatch::NonNull;

    return MetadataMatch::Any;
}

bool ParseUnsigned(
    const char* text,
    std::uintptr_t& value
) {
    if (!text || !*text)
        return false;

    char* end = nullptr;

    unsigned long long parsed =
        std::strtoull(
            text,
            &end,
            0
        );

    if (end == text || *end != '\0')
        return false;

    value = static_cast<std::uintptr_t>(
        parsed
    );

    return true;
}

void Trim(char* text) {
    if (!text)
        return;

    char* start = text;

    while (*start == ' ' ||
           *start == '\t' ||
           *start == '\r' ||
           *start == '\n') {
        ++start;
    }

    if (start != text)
        std::memmove(
            text,
            start,
            std::strlen(start) + 1
        );

    const std::size_t len =
        std::strlen(text);

    if (len == 0)
        return;

    char* end =
        text + len - 1;

    while (end >= text &&
           (*end == ' ' ||
            *end == '\t' ||
            *end == '\r' ||
            *end == '\n')) {
        *end = '\0';

        if (end == text)
            break;

        --end;
    }
}

bool ParseRules(
    const char* text,
    RuntimeConfig& cfg
) {
    cfg.ruleCount = 0;

    if (!text || !*text)
        return true;

    char buffer[4096]{};
    strncpy_s(buffer, text, _TRUNCATE);

    char* context = nullptr;

    for (char* entry = strtok_s(
            buffer,
            ";",
            &context);
         entry != nullptr;
         entry = strtok_s(
            nullptr,
            ";",
            &context)) {
        Trim(entry);

        if (!*entry)
            continue;

        if (cfg.ruleCount >= kMaxRules) {
            SetConfigError(
                "Too many rules; maximum is 32"
            );
            return false;
        }

        char ruleText[512]{};
        strncpy_s(
            ruleText,
            entry,
            _TRUNCATE
        );

        char* fieldContext = nullptr;

        char* callerText =
            strtok_s(
                ruleText,
                ",",
                &fieldContext
            );

        char* formatText =
            strtok_s(
                nullptr,
                ",",
                &fieldContext
            );

        char* metadataText =
            strtok_s(
                nullptr,
                ",",
                &fieldContext
            );

        char* actionText =
            strtok_s(
                nullptr,
                ",",
                &fieldContext
            );

        if (!callerText ||
            !formatText ||
            !metadataText ||
            !actionText) {
            SetConfigError(
                "Rule syntax: callerRva,format|any,metadata(any|null|nonnull),action"
            );
            return false;
        }

        Trim(callerText);
        Trim(formatText);
        Trim(metadataText);
        Trim(actionText);

        Rule rule{};

        if (!ParseUnsigned(
                callerText,
                rule.callerRva)) {
            SetConfigError(
                "Invalid caller RVA in rule"
            );
            return false;
        }

        if (_stricmp(
                formatText,
                "any") == 0) {
            rule.format = -1;
        } else {
            std::uintptr_t parsedFormat = 0;

            if (!ParseUnsigned(
                    formatText,
                    parsedFormat)) {
                SetConfigError(
                    "Invalid format in rule"
                );
                return false;
            }

            rule.format =
                static_cast<int>(
                    parsedFormat
                );
        }

        rule.metadataMatch =
            ParseMetadataMatch(
                metadataText
            );

        rule.action =
            ParseAction(
                actionText
            );

        cfg.rules[cfg.ruleCount++] =
            rule;
    }

    return true;
}

RuntimeConfig* ReadConfig() {
    auto* cfg = new RuntimeConfig();

    const RuntimeConfig* previous =
        g_activeConfig.load(
            std::memory_order_acquire
        );

    if (previous) {
        *cfg = *previous;
        cfg->generation =
            previous->generation + 1;
    }

    FILE* f = nullptr;

    if (_wfopen_s(
            &f,
            g_configPath,
            L"rt") != 0 ||
        !f) {
        SetConfigError("");
        return cfg;
    }

    char rulesText[4096]{};

    char line[4608]{};

    while (fgets(
            line,
            static_cast<int>(
                sizeof(line)
            ),
            f)) {
        Trim(line);

        if (!*line ||
            line[0] == '#') {
            continue;
        }

        int intValue = 0;
        float floatValue = 0.0f;
        char textValue[4096]{};

        if (sscanf_s(
                line,
                "enabled=%d",
                &intValue) == 1) {
            cfg->enabled =
                intValue != 0;
            continue;
        }

        if (sscanf_s(
                line,
                "trace=%d",
                &intValue) == 1) {
            cfg->traceEnabled =
                intValue != 0;
            continue;
        }

        if (sscanf_s(
                line,
                "sdr_white=%f",
                &floatValue) == 1) {
            cfg->sdrWhite =
                std::clamp(
                    floatValue,
                    40.0f,
                    1000.0f
                );
            continue;
        }

        if (sscanf_s(
                line,
                "input_max=%f",
                &floatValue) == 1) {
            cfg->inputMax =
                std::clamp(
                    floatValue,
                    100.0f,
                    10000.0f
                );
            continue;
        }

        if (sscanf_s(
                line,
                "custom_primaries=%d",
                &intValue) == 1) {
            cfg->customPrimaries =
                (intValue >= -1 && intValue <= 2)
                    ? intValue
                    : -1;
            continue;
        }

        if (sscanf_s(
                line,
                "custom_transfer=%d",
                &intValue) == 1) {
            cfg->customTransfer =
                (intValue >= -1 && intValue <= 2)
                    ? intValue
                    : -1;
            continue;
        }

        if (sscanf_s(
                line,
                "custom_metadata=%d",
                &intValue) == 1) {
            if (intValue < 0 || intValue > 2)
                intValue = 0;

            cfg->customMetadata =
                static_cast<CustomMetadataPolicy>(
                    intValue
                );
            continue;
        }

        if (sscanf_s(
                line,
                "mode=%4095s",
                textValue,
                static_cast<unsigned>(
                    sizeof(textValue))) == 1) {
            cfg->mode =
                ParseHostMode(
                    textValue
                );
            continue;
        }

        if (strncmp(
                line,
                "rules=",
                6) == 0) {
            strncpy_s(
                rulesText,
                line + 6,
                _TRUNCATE
            );
            continue;
        }
    }

    fclose(f);

    SetConfigError("");

    if (!ParseRules(
            rulesText,
            *cfg)) {
        delete cfg;
        return nullptr;
    }

    return cfg;
}

void PublishConfig(RuntimeConfig* cfg) {
    if (!cfg)
        return;

    g_activeConfig.store(
        cfg,
        std::memory_order_release
    );
}

unsigned long long ReadCounter(
    volatile LONG64* value
) {
    return static_cast<unsigned long long>(
        InterlockedCompareExchange64(
            value,
            0,
            0
        )
    );
}

LONG ReadLong(
    volatile LONG* value
) {
    return InterlockedCompareExchange(
        value,
        0,
        0
    );
}

std::size_t SnapshotCallers(
    std::array<CallerBucket, 64>& buckets,
    unsigned long long& validEvents,
    unsigned long long& windowStart,
    unsigned long long& windowEnd
) {
    buckets = {};
    validEvents = 0;

    const unsigned long long current =
        ReadCounter(
            &g_traceWriteIndex
        );

    windowEnd = current;

    if (current == 0) {
        windowStart = 0;
        return 0;
    }

    const unsigned long long available =
        std::min<unsigned long long>(
            current,
            kTraceCapacity
        );

    const unsigned long long first =
        current - available + 1;

    windowStart = first;

    std::size_t bucketCount = 0;

    for (unsigned long long seq = first;
         seq <= current;
         ++seq) {
        const std::size_t index =
            static_cast<std::size_t>(
                (seq - 1) %
                kTraceCapacity
            );

        TraceEvent& event =
            g_trace[index];

        const unsigned long long committed =
            static_cast<unsigned long long>(
                InterlockedCompareExchange64(
                    &event.sequence,
                    0,
                    0
                )
            );

        if (committed != seq)
            continue;

        MemoryBarrier();

        const std::uintptr_t callerRva =
            static_cast<std::uintptr_t>(
                InterlockedCompareExchange64(
                    &event.callerRva,
                    0,
                    0
                )
            );

        const auto metadata =
            reinterpret_cast<void*>(
                InterlockedCompareExchange64(
                    &event.originalMetadataPointer,
                    0,
                    0
                )
            );

        const LONG readable =
            ReadLong(
                &event.sourceReadable
            );

        const LONG format =
            ReadLong(
                &event.format
            );

        const LONG primaries =
            ReadLong(
                &event.originalPrimaries
            );

        const LONG transfer =
            ReadLong(
                &event.originalTransfer
            );

        const LONG action =
            ReadLong(
                &event.action
            );

        ++validEvents;

        std::size_t bucketIndex =
            bucketCount;

        for (std::size_t i = 0;
             i < bucketCount;
             ++i) {
            if (buckets[i].callerRva ==
                callerRva) {
                bucketIndex = i;
                break;
            }
        }

        if (bucketIndex == bucketCount) {
            if (bucketCount >=
                buckets.size()) {
                continue;
            }

            buckets[bucketCount].callerRva =
                callerRva;

            ++bucketCount;
        }

        CallerBucket& bucket =
            buckets[bucketIndex];

        ++bucket.count;

        if (metadata == nullptr)
            ++bucket.metadataNull;
        else
            ++bucket.metadataNonNull;

        if (!readable)
            ++bucket.sourceUnreadable;

        bucket.lastFormat = format;
        bucket.lastPrimaries = primaries;
        bucket.lastTransfer = transfer;
        bucket.lastAction = action;
    }

    std::sort(
        buckets.begin(),
        buckets.begin() + bucketCount,
        [](const CallerBucket& a,
           const CallerBucket& b) {
            return a.count > b.count;
        }
    );

    return bucketCount;
}

void AppendFormat(
    char* buffer,
    std::size_t capacity,
    std::size_t& used,
    const char* format,
    ...
) {
    if (used >= capacity)
        return;

    va_list args;
    va_start(args, format);

    const int written =
        _vsnprintf_s(
            buffer + used,
            capacity - used,
            _TRUNCATE,
            format,
            args
        );

    va_end(args);

    if (written > 0)
        used +=
            static_cast<std::size_t>(
                written
            );
}

void WriteStatus() {
    const RuntimeConfig* cfg =
        g_activeConfig.load(
            std::memory_order_acquire
        );

    RuntimeConfig fallback{};
    const RuntimeConfig& c =
        cfg ? *cfg : fallback;

    std::array<CallerBucket, 64> buckets{};

    unsigned long long validEvents = 0;
    unsigned long long windowStart = 0;
    unsigned long long windowEnd = 0;

    const std::size_t bucketCount =
        SnapshotCallers(
            buckets,
            validEvents,
            windowStart,
            windowEnd
        );

    constexpr std::size_t capacity =
        65536;

    char json[capacity]{};
    std::size_t used = 0;

    const LONG lastAction =
        ReadLong(
            &g_lastAction
        );

    AppendFormat(
        json,
        capacity,
        used,
        "{\n"
        "  \"version\": \"1.2.2-stream-ui-fix\",\n"
        "  \"pid\": %lu,\n"
        "  \"hook_installed\": %s,\n"
        "  \"signature\": \"%s\",\n"
        "  \"renderer_wrapper_rva\": \"0x52cad0\",\n"
        "  \"config_generation\": %u,\n"
        "  \"config_enabled\": %s,\n"
        "  \"host_mode\": \"%s\",\n"
        "  \"trace_enabled\": %s,\n"
        "  \"sdr_white_level\": %.3f,\n"
        "  \"input_max_luminance\": %.3f,\n"
        "  \"custom_primaries\": %d,\n"
        "  \"custom_primaries_name\": \"%s\",\n"
        "  \"custom_transfer\": %d,\n"
        "  \"custom_transfer_name\": \"%s\",\n"
        "  \"custom_metadata\": %d,\n"
        "  \"custom_metadata_name\": \"%s\",\n"
        "  \"rules_loaded\": %zu,\n"
        "  \"config_error\": \"%s\",\n"
        "  \"total_renderer_calls\": %llu,\n"
        "  \"total_modified_calls\": %llu,\n"
        "  \"total_metadata_injected\": %llu,\n"
        "  \"total_source_overrides\": %llu,\n"
        "  \"last_caller_rva\": \"0x%llx\",\n"
        "  \"last_format\": %ld,\n"
        "  \"last_format_name\": \"%s\",\n"
        "  \"last_original_primaries\": %ld,\n"
        "  \"last_original_primaries_name\": \"%s\",\n"
        "  \"last_original_transfer\": %ld,\n"
        "  \"last_original_transfer_name\": \"%s\",\n"
        "  \"last_effective_primaries\": %ld,\n"
        "  \"last_effective_primaries_name\": \"%s\",\n"
        "  \"last_effective_transfer\": %ld,\n"
        "  \"last_effective_transfer_name\": \"%s\",\n"
        "  \"last_action\": \"%s\",\n"
        "  \"last_source_readable\": %s,\n"
        "  \"last_original_metadata_null\": %s,\n"
        "  \"recent_window_start_sequence\": %llu,\n"
        "  \"recent_window_end_sequence\": %llu,\n"
        "  \"recent_valid_events\": %llu,\n"
        "  \"recent_unique_callers\": %zu,\n"
        "  \"recent_callers\": [\n",
        GetCurrentProcessId(),
        ReadLong(&g_hookInstalled)
            ? "true"
            : "false",
        g_signature,
        c.generation,
        c.enabled ? "true" : "false",
        HostModeName(c.mode),
        c.traceEnabled
            ? "true"
            : "false",
        static_cast<double>(
            c.sdrWhite
        ),
        static_cast<double>(
            c.inputMax
        ),
        c.customPrimaries,
        c.customPrimaries < 0
            ? "preserve"
            : PrimariesName(c.customPrimaries),
        c.customTransfer,
        c.customTransfer < 0
            ? "preserve"
            : TransferName(c.customTransfer),
        static_cast<int>(c.customMetadata),
        c.customMetadata == CustomMetadataPolicy::None
            ? "none"
            : (c.customMetadata == CustomMetadataPolicy::Inject
                ? "inject"
                : "preserve"),
        c.ruleCount,
        g_configError,
        ReadCounter(
            &g_totalRendererCalls
        ),
        ReadCounter(
            &g_totalModifiedCalls
        ),
        ReadCounter(
            &g_totalMetadataInjected
        ),
        ReadCounter(
            &g_totalSourceOverrides
        ),
        static_cast<unsigned long long>(
            ReadCounter(
                &g_lastCallerRva
            )
        ),
        ReadLong(&g_lastFormat),
        FormatName(
            ReadLong(
                &g_lastFormat
            )
        ),
        ReadLong(
            &g_lastOriginalPrimaries
        ),
        PrimariesName(
            ReadLong(
                &g_lastOriginalPrimaries
            )
        ),
        ReadLong(
            &g_lastOriginalTransfer
        ),
        TransferName(
            ReadLong(
                &g_lastOriginalTransfer
            )
        ),
        ReadLong(
            &g_lastEffectivePrimaries
        ),
        PrimariesName(
            ReadLong(
                &g_lastEffectivePrimaries
            )
        ),
        ReadLong(
            &g_lastEffectiveTransfer
        ),
        TransferName(
            ReadLong(
                &g_lastEffectiveTransfer
            )
        ),
        ActionName(
            static_cast<Action>(
                lastAction
            )
        ),
        ReadLong(
            &g_lastSourceReadable
        ) ? "true" : "false",
        ReadLong(
            &g_lastMetadataWasNull
        ) < 0
            ? "null"
            : (ReadLong(
                    &g_lastMetadataWasNull
                ) ? "true" : "false"),
        windowStart,
        windowEnd,
        validEvents,
        bucketCount
    );

    const std::size_t show =
        std::min<std::size_t>(
            bucketCount,
            16
        );

    for (std::size_t i = 0;
         i < show;
         ++i) {
        const CallerBucket& b =
            buckets[i];

        AppendFormat(
            json,
            capacity,
            used,
            "    {"
            "\"caller_rva\":\"0x%llx\","
            "\"count\":%llu,"
            "\"metadata_null\":%llu,"
            "\"metadata_nonnull\":%llu,"
            "\"source_unreadable\":%llu,"
            "\"last_format\":%ld,"
            "\"last_format_name\":\"%s\","
            "\"last_primaries\":\"%s\","
            "\"last_transfer\":\"%s\","
            "\"last_action\":\"%s\""
            "}%s\n",
            static_cast<unsigned long long>(
                b.callerRva
            ),
            b.count,
            b.metadataNull,
            b.metadataNonNull,
            b.sourceUnreadable,
            b.lastFormat,
            FormatName(
                b.lastFormat
            ),
            PrimariesName(
                b.lastPrimaries
            ),
            TransferName(
                b.lastTransfer
            ),
            ActionName(
                static_cast<Action>(
                    b.lastAction
                )
            ),
            (i + 1 < show)
                ? ","
                : ""
        );
    }

    AppendFormat(
        json,
        capacity,
        used,
        "  ],\n"
        "  \"rules\": [\n"
    );

    for (std::size_t i = 0;
         i < c.ruleCount;
         ++i) {
        const Rule& r = c.rules[i];

        const char* metadataName =
            r.metadataMatch ==
                MetadataMatch::Null
            ? "null"
            : (r.metadataMatch ==
                    MetadataMatch::NonNull
                ? "nonnull"
                : "any");

        AppendFormat(
            json,
            capacity,
            used,
            "    {"
            "\"caller_rva\":\"0x%llx\","
            "\"format\":%d,"
            "\"metadata\":\"%s\","
            "\"action\":\"%s\""
            "}%s\n",
            static_cast<unsigned long long>(
                r.callerRva
            ),
            r.format,
            metadataName,
            ActionName(r.action),
            (i + 1 < c.ruleCount)
                ? ","
                : ""
        );
    }

    AppendFormat(
        json,
        capacity,
        used,
        "  ],\n"
        "  \"error\": \"%s\"\n"
        "}\n",
        g_error
    );

    wchar_t tempPath[MAX_PATH]{};

    swprintf_s(
        tempPath,
        L"%s.tmp",
        g_statusPath
    );

    HANDLE f = CreateFileW(
        tempPath,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (f == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;

    WriteFile(
        f,
        json,
        static_cast<DWORD>(
            std::strlen(json)
        ),
        &written,
        nullptr
    );

    FlushFileBuffers(f);
    CloseHandle(f);

    MoveFileExW(
        tempPath,
        g_statusPath,
        MOVEFILE_REPLACE_EXISTING |
        MOVEFILE_WRITE_THROUGH
    );
}


bool GetConfigWriteTime(FILETIME& time) {
    WIN32_FILE_ATTRIBUTE_DATA data{};

    if (!GetFileAttributesExW(
            g_configPath,
            GetFileExInfoStandard,
            &data)) {
        return false;
    }

    time = data.ftLastWriteTime;
    return true;
}

DWORD WINAPI WorkerThread(void*) {
    BuildPaths();

    PublishConfig(
        new RuntimeConfig()
    );

    SetSignature(
        "waiting for discord_voice.node"
    );

    SetError("");
    SetConfigError("");

    HMODULE voice = nullptr;

    for (int i = 0; i < 3000; ++i) {
        voice = GetModuleHandleW(
            L"discord_voice.node"
        );

        if (voice)
            break;

        Sleep(100);
    }

    if (!voice) {
        SetError(
            "discord_voice.node did not load within 300 seconds"
        );

        WriteStatus();
        return 0;
    }

    if (!VerifyDiscordBuild(voice)) {
        WriteStatus();
        return 0;
    }

    if (!PatchRendererEntry(voice)) {
        WriteStatus();
        return 0;
    }

    InterlockedExchange(
        &g_hookInstalled,
        1
    );

    SetSignature(
        "shared renderer dev host installed"
    );

    SetError("");

    FILETIME lastConfigWrite{};
    bool haveConfigWrite = false;

    for (;;) {
        FILETIME currentWrite{};

        if (GetConfigWriteTime(currentWrite)) {
            if (!haveConfigWrite ||
                CompareFileTime(
                    &currentWrite,
                    &lastConfigWrite) != 0) {
                RuntimeConfig* next =
                    ReadConfig();

                if (next) {
                    PublishConfig(next);
                    lastConfigWrite = currentWrite;
                    haveConfigWrite = true;
                }
            }
        }

        WriteStatus();
        Sleep(250);
    }
}

BOOL WINAPI DllMain(
    HINSTANCE instance,
    DWORD reason,
    LPVOID
) {
    if (reason ==
        DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(
            instance
        );

        HANDLE thread = CreateThread(
            nullptr,
            0,
            &WorkerThread,
            nullptr,
            0,
            nullptr
        );

        if (thread)
            CloseHandle(thread);
    }

    return TRUE;
}
