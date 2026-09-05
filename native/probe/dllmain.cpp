#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

// Exact analyzed Windows discord_voice.node supplied by the tester.
// SHA-256:
// 54d452eefb5bd20f022dca1f93e3a92cf641e14283761720e4276f3fb0585db9
constexpr DWORD kExpectedTimeDateStamp = 0x6a95b8a4;
constexpr DWORD kExpectedSizeOfImage = 0x00fd8000;

constexpr std::uintptr_t kVideoHookNullMetadataRva = 0x003fd443;
constexpr std::uintptr_t kVideoHookRendererCallRva = 0x003fd462;
constexpr std::uintptr_t kVideoHookReturnRva = 0x003fd467;
constexpr std::uintptr_t kRendererWrapperRva = 0x0052cad0;

// Renderer source descriptor fields recovered from the exact binary:
// +0x178 = DXGI_FORMAT (u32)
// +0x17c = source gamut / primaries enum
//          0 = Rec709, 1 = Rec2020, 2 = Arc
// +0x17d = transfer enum
//          0 = Linear, 1 = sRGB, 2 = SMPTE ST 2084 / PQ
constexpr std::size_t kSourceFormatOffset = 0x178;
constexpr std::size_t kSourcePrimariesOffset = 0x17c;
constexpr std::size_t kSourceTransferOffset = 0x17d;

constexpr std::uint8_t kRec709 = 0;
constexpr std::uint8_t kRec2020 = 1;
constexpr std::uint8_t kArc = 2;

constexpr std::uint8_t kLinear = 0;
constexpr std::uint8_t kSrgb = 1;
constexpr std::uint8_t kSt2084 = 2;

// DXGI formats relevant to the renderer's own HDR branches.
constexpr std::uint32_t kDxgiR16G16B16A16Float = 10;
constexpr std::uint32_t kDxgiR10G10B10A2Unorm = 24;

struct HdrMetadata {
    float sdrWhiteLevel;       // +0x00
    float inputMaxLuminance;   // +0x04
    std::uint8_t state;        // +0x08; 1 = valid HDR metadata
    std::uint8_t padding[3];
};

static_assert(sizeof(HdrMetadata) == 12);
static_assert(offsetof(HdrMetadata, state) == 8);

enum class SourceColorMode : std::uint32_t {
    Preserve = 0,
    AutoHdrByFormat = 1,
    Rec709Linear = 2,
    Rec709Srgb = 3,
    Rec2020Linear = 4,
    Rec2020Srgb = 5,
    Rec2020St2084 = 6
};

struct Config {
    bool enabled = true;
    float white = 200.0f;
    float peak = 1000.0f;
    SourceColorMode sourceColorMode = SourceColorMode::Preserve;
};

using RendererFn = void* (__fastcall*)(
    void*, void*, void*, void*,
    void*, void*, void*, void*, void*
);

RendererFn g_originalRenderer = nullptr;

alignas(16) HdrMetadata g_metadataRing[256]{};
std::atomic<unsigned> g_metadataWriteIndex{0};
void* volatile g_activeMetadataPtr = nullptr;

std::atomic<bool> g_enabled{true};
std::atomic<std::uint32_t> g_whiteBits{0};
std::atomic<std::uint32_t> g_peakBits{0};
std::atomic<std::uint32_t> g_sourceMode{0};
std::atomic<bool> g_hookInstalled{false};

std::atomic<std::uint32_t> g_lastOriginalFormat{0xffffffffu};
std::atomic<std::uint32_t> g_lastOriginalPrimaries{0xffffffffu};
std::atomic<std::uint32_t> g_lastOriginalTransfer{0xffffffffu};
std::atomic<std::uint32_t> g_lastEffectivePrimaries{0xffffffffu};
std::atomic<std::uint32_t> g_lastEffectiveTransfer{0xffffffffu};

std::atomic<unsigned long long> g_rendererCalls{0};
std::atomic<unsigned long long> g_metadataInjectedCalls{0};
std::atomic<unsigned long long> g_sourceOverrideCalls{0};

std::uint8_t* g_relay = nullptr;

wchar_t g_statusPath[MAX_PATH]{};
wchar_t g_configPath[MAX_PATH]{};
char g_signature[160] = "waiting for discord_voice.node";
char g_error[384]{};

std::uint32_t FloatBits(float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

float BitsFloat(std::uint32_t bits) {
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

void SetSignature(const char* text) {
    strncpy_s(g_signature, text ? text : "", _TRUNCATE);
}

void SetError(const char* text) {
    strncpy_s(g_error, text ? text : "", _TRUNCATE);
}

const char* FormatName(std::uint32_t format) {
    switch (format) {
    case 10: return "R16G16B16A16_FLOAT";
    case 24: return "R10G10B10A2_UNORM";
    case 29: return "R8G8B8A8_UNORM_SRGB";
    case 91: return "B8G8R8A8_UNORM_SRGB";
    case 93: return "B8G8R8X8_UNORM_SRGB";
    default: return "other";
    }
}

const char* PrimariesName(std::uint32_t value) {
    switch (value) {
    case 0: return "Rec709";
    case 1: return "Rec2020";
    case 2: return "Arc";
    default: return "unknown";
    }
}

const char* TransferName(std::uint32_t value) {
    switch (value) {
    case 0: return "Linear";
    case 1: return "sRGB";
    case 2: return "SMPTE_ST2084";
    default: return "unknown";
    }
}

const char* SourceModeName(std::uint32_t value) {
    switch (static_cast<SourceColorMode>(value)) {
    case SourceColorMode::Preserve: return "preserve";
    case SourceColorMode::AutoHdrByFormat: return "auto_hdr_by_dxgi_format";
    case SourceColorMode::Rec709Linear: return "rec709_linear";
    case SourceColorMode::Rec709Srgb: return "rec709_srgb";
    case SourceColorMode::Rec2020Linear: return "rec2020_linear";
    case SourceColorMode::Rec2020Srgb: return "rec2020_srgb";
    case SourceColorMode::Rec2020St2084: return "rec2020_st2084";
    default: return "preserve";
    }
}

void BuildPaths() {
    wchar_t temp[MAX_PATH]{};
    DWORD tempLen = GetTempPathW(MAX_PATH, temp);
    if (tempLen == 0 || tempLen >= MAX_PATH)
        wcscpy_s(temp, L".\\");

    swprintf_s(
        g_statusPath,
        L"%sDiscordHDRFix-v07-%lu.json",
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
        wcscpy_s(g_configPath, L"tone-map.cfg");
        return;
    }

    swprintf_s(
        g_configPath,
        L"%s\\DiscordHDRFix\\tone-map.cfg",
        localAppData
    );
}

Config CurrentConfig() {
    Config c;
    c.enabled = g_enabled.load(std::memory_order_relaxed);

    c.white = BitsFloat(g_whiteBits.load(std::memory_order_relaxed));
    if (!(c.white > 0.0f))
        c.white = 200.0f;

    c.peak = BitsFloat(g_peakBits.load(std::memory_order_relaxed));
    if (!(c.peak > 0.0f))
        c.peak = 1000.0f;

    const auto mode = g_sourceMode.load(std::memory_order_relaxed);
    c.sourceColorMode =
        mode <= static_cast<std::uint32_t>(SourceColorMode::Rec2020St2084)
            ? static_cast<SourceColorMode>(mode)
            : SourceColorMode::Preserve;

    return c;
}

Config ReadConfig() {
    Config c = CurrentConfig();

    FILE* f = nullptr;
    if (_wfopen_s(&f, g_configPath, L"rt") != 0 || !f)
        return c;

    char line[256]{};
    while (fgets(line, static_cast<int>(sizeof(line)), f)) {
        int enabled = 0;
        unsigned sourceMode = 0;
        float value = 0.0f;

        if (sscanf_s(line, "enabled=%d", &enabled) == 1) {
            c.enabled = enabled != 0;
            continue;
        }

        if (sscanf_s(line, "sdr_white=%f", &value) == 1) {
            c.white = value;
            continue;
        }

        if (sscanf_s(line, "input_max=%f", &value) == 1) {
            c.peak = value;
            continue;
        }

        if (sscanf_s(line, "source_mode=%u", &sourceMode) == 1) {
            if (sourceMode <=
                static_cast<unsigned>(SourceColorMode::Rec2020St2084)) {
                c.sourceColorMode =
                    static_cast<SourceColorMode>(sourceMode);
            }
            continue;
        }
    }

    fclose(f);

    c.white = std::clamp(c.white, 40.0f, 1000.0f);
    c.peak = std::clamp(c.peak, 100.0f, 10000.0f);

    return c;
}

void PublishConfig(const Config& c) {
    const unsigned index =
        g_metadataWriteIndex.fetch_add(1, std::memory_order_relaxed) % 256u;

    HdrMetadata& m = g_metadataRing[index];
    m.sdrWhiteLevel = c.white;
    m.inputMaxLuminance = c.peak;
    m.state = 1;
    m.padding[0] = 0;
    m.padding[1] = 0;
    m.padding[2] = 0;

    MemoryBarrier();

    InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(&g_activeMetadataPtr),
        c.enabled ? static_cast<void*>(&m) : nullptr
    );

    g_enabled.store(c.enabled, std::memory_order_relaxed);
    g_whiteBits.store(FloatBits(c.white), std::memory_order_relaxed);
    g_peakBits.store(FloatBits(c.peak), std::memory_order_relaxed);
    g_sourceMode.store(
        static_cast<std::uint32_t>(c.sourceColorMode),
        std::memory_order_relaxed
    );
}

void ChooseEffectiveColor(
    SourceColorMode mode,
    std::uint32_t format,
    std::uint8_t originalPrimaries,
    std::uint8_t originalTransfer,
    std::uint8_t& effectivePrimaries,
    std::uint8_t& effectiveTransfer
) {
    effectivePrimaries = originalPrimaries;
    effectiveTransfer = originalTransfer;

    switch (mode) {
    case SourceColorMode::Preserve:
        return;

    case SourceColorMode::AutoHdrByFormat:
        if (format == kDxgiR16G16B16A16Float) {
            effectivePrimaries = kRec709;
            effectiveTransfer = kLinear;
        } else if (format == kDxgiR10G10B10A2Unorm) {
            effectivePrimaries = kRec2020;
            effectiveTransfer = kSt2084;
        }
        return;

    case SourceColorMode::Rec709Linear:
        effectivePrimaries = kRec709;
        effectiveTransfer = kLinear;
        return;

    case SourceColorMode::Rec709Srgb:
        effectivePrimaries = kRec709;
        effectiveTransfer = kSrgb;
        return;

    case SourceColorMode::Rec2020Linear:
        effectivePrimaries = kRec2020;
        effectiveTransfer = kLinear;
        return;

    case SourceColorMode::Rec2020Srgb:
        effectivePrimaries = kRec2020;
        effectiveTransfer = kSrgb;
        return;

    case SourceColorMode::Rec2020St2084:
        effectivePrimaries = kRec2020;
        effectiveTransfer = kSt2084;
        return;
    }
}

extern "C" __declspec(noinline) void* __fastcall HookRenderer(
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
    g_rendererCalls.fetch_add(1, std::memory_order_relaxed);

    if (a4) {
        auto* source = static_cast<std::uint8_t*>(a4);

        const std::uint32_t format =
            *reinterpret_cast<std::uint32_t*>(
                source + kSourceFormatOffset
            );

        const std::uint8_t originalPrimaries =
            *(source + kSourcePrimariesOffset);

        const std::uint8_t originalTransfer =
            *(source + kSourceTransferOffset);

        g_lastOriginalFormat.store(format, std::memory_order_relaxed);
        g_lastOriginalPrimaries.store(
            originalPrimaries,
            std::memory_order_relaxed
        );
        g_lastOriginalTransfer.store(
            originalTransfer,
            std::memory_order_relaxed
        );

        std::uint8_t effectivePrimaries = originalPrimaries;
        std::uint8_t effectiveTransfer = originalTransfer;

        const auto mode = static_cast<SourceColorMode>(
            g_sourceMode.load(std::memory_order_relaxed)
        );

        ChooseEffectiveColor(
            mode,
            format,
            originalPrimaries,
            originalTransfer,
            effectivePrimaries,
            effectiveTransfer
        );

        if (effectivePrimaries != originalPrimaries ||
            effectiveTransfer != originalTransfer) {
            *(source + kSourcePrimariesOffset) = effectivePrimaries;
            *(source + kSourceTransferOffset) = effectiveTransfer;

            g_sourceOverrideCalls.fetch_add(
                1,
                std::memory_order_relaxed
            );
        }

        g_lastEffectivePrimaries.store(
            effectivePrimaries,
            std::memory_order_relaxed
        );
        g_lastEffectiveTransfer.store(
            effectiveTransfer,
            std::memory_order_relaxed
        );
    }

    void* metadata = a9;

    if (g_enabled.load(std::memory_order_relaxed)) {
        metadata = InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_activeMetadataPtr),
            nullptr,
            nullptr
        );

        if (metadata) {
            g_metadataInjectedCalls.fetch_add(
                1,
                std::memory_order_relaxed
            );
        }
    }

    return g_originalRenderer(
        a1, a2, a3, a4,
        a5, a6, a7, a8, metadata
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
        SetSignature("discord_voice.node DOS header mismatch");
        SetError("invalid DOS header");
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        base + dos->e_lfanew
    );

    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        SetSignature("discord_voice.node PE32+ header mismatch");
        SetError("invalid PE32+ header");
        return false;
    }

    if (nt->FileHeader.TimeDateStamp != kExpectedTimeDateStamp ||
        nt->OptionalHeader.SizeOfImage != kExpectedSizeOfImage) {
        SetSignature("unsupported discord_voice.node build");
        SetError("discord_voice.node version changed; refusing to patch");
        return false;
    }

    constexpr std::uint8_t nullArg[] = {
        0x48, 0xc7, 0x44, 0x24, 0x40,
        0x00, 0x00, 0x00, 0x00
    };

    constexpr std::uint8_t callBytes[] = {
        0xe8, 0x69, 0xf6, 0x12, 0x00
    };

    constexpr std::uint8_t wrapperPrologue[] = {
        0x41, 0x57, 0x41, 0x56,
        0x41, 0x55, 0x41, 0x54,
        0x56, 0x57, 0x55, 0x53,
        0x48, 0x83, 0xec, 0x68
    };

    if (!EqualBytes(
            base + kVideoHookNullMetadataRva,
            nullArg,
            sizeof(nullArg))) {
        SetSignature("video-hook signature mismatch");
        SetError("video-hook HDR metadata callsite signature did not match");
        return false;
    }

    if (!EqualBytes(
            base + kVideoHookRendererCallRva,
            callBytes,
            sizeof(callBytes))) {
        SetSignature("video-hook renderer call mismatch");
        SetError("video-hook renderer CALL signature did not match");
        return false;
    }

    if (!EqualBytes(
            base + kRendererWrapperRva,
            wrapperPrologue,
            sizeof(wrapperPrologue))) {
        SetSignature("renderer signature mismatch");
        SetError("renderer wrapper signature did not match");
        return false;
    }

    const auto* call = base + kVideoHookRendererCallRva;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, call + 1, sizeof(displacement));

    const std::uintptr_t destination =
        reinterpret_cast<std::uintptr_t>(call + 5) +
        static_cast<std::intptr_t>(displacement);

    if (destination !=
        reinterpret_cast<std::uintptr_t>(base + kRendererWrapperRva)) {
        SetSignature("renderer call target mismatch");
        SetError("renderer wrapper destination changed");
        return false;
    }

    SetSignature("matched exact analyzed build");
    SetError("");
    return true;
}

std::uintptr_t AlignUp(
    std::uintptr_t value,
    std::uintptr_t alignment
) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void* AllocateNear(void* target, SIZE_T size) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);

    const std::uintptr_t granularity =
        static_cast<std::uintptr_t>(info.dwAllocationGranularity);

    const std::uintptr_t targetAddress =
        reinterpret_cast<std::uintptr_t>(target);

    constexpr std::uintptr_t reach = 0x70000000ull;

    const std::uintptr_t processMin =
        reinterpret_cast<std::uintptr_t>(
            info.lpMinimumApplicationAddress
        );
    const std::uintptr_t processMax =
        reinterpret_cast<std::uintptr_t>(
            info.lpMaximumApplicationAddress
        );

    const std::uintptr_t minimum =
        targetAddress > reach
            ? std::max(processMin, targetAddress - reach)
            : processMin;

    const std::uintptr_t maximum =
        std::min(processMax, targetAddress + reach);

    std::uintptr_t cursor = AlignUp(minimum, granularity);

    while (cursor < maximum) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(
                reinterpret_cast<void*>(cursor),
                &mbi,
                sizeof(mbi)) == 0) {
            break;
        }

        const std::uintptr_t regionBase =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t regionEnd =
            regionBase + mbi.RegionSize;

        if (mbi.State == MEM_FREE) {
            const std::uintptr_t candidate =
                AlignUp(std::max(cursor, regionBase), granularity);

            if (candidate + size <= regionEnd &&
                candidate + size <= maximum) {
                if (void* allocation = VirtualAlloc(
                        reinterpret_cast<void*>(candidate),
                        size,
                        MEM_RESERVE | MEM_COMMIT,
                        PAGE_EXECUTE_READWRITE)) {
                    return allocation;
                }
            }
        }

        if (regionEnd <= cursor)
            break;

        cursor = AlignUp(regionEnd, granularity);
    }

    return nullptr;
}

template <typename T>
void WriteImmediate(std::uint8_t* dest, T value) {
    std::memcpy(dest, &value, sizeof(value));
}

bool BuildRelay(std::uint8_t* callSite) {
    constexpr SIZE_T allocationSize = 0x1000;

    g_relay = static_cast<std::uint8_t*>(
        AllocateNear(callSite, allocationSize)
    );

    if (!g_relay) {
        SetError("VirtualAlloc failed for Video Hook relay");
        return false;
    }

    std::memset(g_relay, 0xcc, allocationSize);

    std::size_t p = 0;

    // mov rax, HookRenderer
    g_relay[p++] = 0x48;
    g_relay[p++] = 0xb8;

    WriteImmediate(
        g_relay + p,
        reinterpret_cast<std::uintptr_t>(&HookRenderer)
    );
    p += sizeof(std::uintptr_t);

    // jmp rax
    g_relay[p++] = 0xff;
    g_relay[p++] = 0xe0;

    FlushInstructionCache(
        GetCurrentProcess(),
        g_relay,
        allocationSize
    );

    return true;
}

bool PatchVideoHookCall(HMODULE voice) {
    auto* base = reinterpret_cast<std::uint8_t*>(voice);
    auto* callSite = base + kVideoHookRendererCallRva;

    g_originalRenderer = reinterpret_cast<RendererFn>(
        base + kRendererWrapperRva
    );

    if (!BuildRelay(callSite))
        return false;

    const std::intptr_t from =
        reinterpret_cast<std::intptr_t>(callSite + 5);
    const std::intptr_t to =
        reinterpret_cast<std::intptr_t>(g_relay);

    const std::intptr_t delta = to - from;
    if (delta < std::numeric_limits<std::int32_t>::min() ||
        delta > std::numeric_limits<std::int32_t>::max()) {
        SetError("Video Hook relay is outside CALL rel32 range");
        return false;
    }

    std::uint8_t patch[5] = { 0xe8, 0, 0, 0, 0 };

    WriteImmediate(
        patch + 1,
        static_cast<std::int32_t>(delta)
    );

    DWORD oldProtection = 0;
    if (!VirtualProtect(
            callSite,
            sizeof(patch),
            PAGE_EXECUTE_READWRITE,
            &oldProtection)) {
        SetError("VirtualProtect failed for Video Hook renderer call");
        return false;
    }

    std::memcpy(callSite, patch, sizeof(patch));

    DWORD ignored = 0;
    VirtualProtect(
        callSite,
        sizeof(patch),
        oldProtection,
        &ignored
    );

    FlushInstructionCache(
        GetCurrentProcess(),
        callSite,
        sizeof(patch)
    );

    return true;
}

void WriteStatus() {
    const float white =
        BitsFloat(g_whiteBits.load(std::memory_order_relaxed));
    const float peak =
        BitsFloat(g_peakBits.load(std::memory_order_relaxed));

    const auto sourceMode =
        g_sourceMode.load(std::memory_order_relaxed);

    const auto format =
        g_lastOriginalFormat.load(std::memory_order_relaxed);
    const auto originalPrimaries =
        g_lastOriginalPrimaries.load(std::memory_order_relaxed);
    const auto originalTransfer =
        g_lastOriginalTransfer.load(std::memory_order_relaxed);
    const auto effectivePrimaries =
        g_lastEffectivePrimaries.load(std::memory_order_relaxed);
    const auto effectiveTransfer =
        g_lastEffectiveTransfer.load(std::memory_order_relaxed);

    char json[6144]{};

    _snprintf_s(
        json,
        sizeof(json),
        _TRUNCATE,
        "{\n"
        "  \"version\": \"0.7\",\n"
        "  \"pid\": %lu,\n"
        "  \"mode\": \"hdr-metadata-plus-source-colorspace\",\n"
        "  \"hook_installed\": %s,\n"
        "  \"signature\": \"%s\",\n"
        "  \"renderer_wrapper_rva\": \"0x52cad0\",\n"
        "  \"video_hook_return_rva\": \"0x3fd467\",\n"
        "  \"enabled\": %s,\n"
        "  \"sdr_white_level\": %.3f,\n"
        "  \"input_max_luminance\": %.3f,\n"
        "  \"metadata_state\": 1,\n"
        "  \"metadata_size\": 12,\n"
        "  \"source_color_mode\": \"%s\",\n"
        "  \"source_format\": %u,\n"
        "  \"source_format_name\": \"%s\",\n"
        "  \"original_primaries\": %u,\n"
        "  \"original_primaries_name\": \"%s\",\n"
        "  \"original_transfer\": %u,\n"
        "  \"original_transfer_name\": \"%s\",\n"
        "  \"effective_primaries\": %u,\n"
        "  \"effective_primaries_name\": \"%s\",\n"
        "  \"effective_transfer\": %u,\n"
        "  \"effective_transfer_name\": \"%s\",\n"
        "  \"renderer_calls\": %llu,\n"
        "  \"hdr_metadata_injected\": %llu,\n"
        "  \"source_color_overrides\": %llu,\n"
        "  \"last_original_hdr_metadata_null\": true,\n"
        "  \"error\": \"%s\"\n"
        "}\n",
        GetCurrentProcessId(),
        g_hookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
        g_signature,
        g_enabled.load(std::memory_order_relaxed) ? "true" : "false",
        static_cast<double>(white),
        static_cast<double>(peak),
        SourceModeName(sourceMode),
        format,
        FormatName(format),
        originalPrimaries,
        PrimariesName(originalPrimaries),
        originalTransfer,
        TransferName(originalTransfer),
        effectivePrimaries,
        PrimariesName(effectivePrimaries),
        effectiveTransfer,
        TransferName(effectiveTransfer),
        g_rendererCalls.load(std::memory_order_relaxed),
        g_metadataInjectedCalls.load(std::memory_order_relaxed),
        g_sourceOverrideCalls.load(std::memory_order_relaxed),
        g_error
    );

    wchar_t tempPath[MAX_PATH]{};
    swprintf_s(tempPath, L"%s.tmp", g_statusPath);

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
        static_cast<DWORD>(std::strlen(json)),
        &written,
        nullptr
    );

    FlushFileBuffers(f);
    CloseHandle(f);

    MoveFileExW(
        tempPath,
        g_statusPath,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
    );
}

bool ConfigChanged(const Config& a, const Config& b) {
    return
        a.enabled != b.enabled ||
        a.white != b.white ||
        a.peak != b.peak ||
        a.sourceColorMode != b.sourceColorMode;
}

DWORD WINAPI WorkerThread(void*) {
    BuildPaths();

    g_whiteBits.store(
        FloatBits(200.0f),
        std::memory_order_relaxed
    );
    g_peakBits.store(
        FloatBits(1000.0f),
        std::memory_order_relaxed
    );

    Config config = ReadConfig();
    PublishConfig(config);

    SetSignature("waiting for discord_voice.node");
    SetError("");
    WriteStatus();

    HMODULE voice = nullptr;

    for (int i = 0; i < 3000; ++i) {
        voice = GetModuleHandleW(L"discord_voice.node");
        if (voice)
            break;
        Sleep(100);
    }

    if (!voice) {
        SetError("discord_voice.node was not loaded within 300 seconds");
        WriteStatus();
        return 0;
    }

    if (!VerifyDiscordBuild(voice)) {
        WriteStatus();
        return 0;
    }

    if (!PatchVideoHookCall(voice)) {
        WriteStatus();
        return 0;
    }

    g_hookInstalled.store(true, std::memory_order_release);
    SetError("");
    WriteStatus();

    Config previous = CurrentConfig();

    for (;;) {
        const Config latest = ReadConfig();

        if (ConfigChanged(latest, previous)) {
            PublishConfig(latest);
            previous = CurrentConfig();
        }

        WriteStatus();
        Sleep(500);
    }
}

} // namespace

BOOL WINAPI DllMain(
    HINSTANCE instance,
    DWORD reason,
    LPVOID
) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);

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
