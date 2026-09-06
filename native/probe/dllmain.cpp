#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>


// Exact analyzed Windows discord_voice.node supplied by the tester.
// SHA-256:
// 54d452eefb5bd20f022dca1f93e3a92cf641e14283761720e4276f3fb0585db9
constexpr DWORD kExpectedTimeDateStamp = 0x6a95b8a4;
constexpr DWORD kExpectedSizeOfImage = 0x00fd8000;

constexpr std::uintptr_t kVideoHookFrameSaveRva = 0x003fcdca;
constexpr std::uintptr_t kVideoHookNullMetadataRva = 0x003fd443;
constexpr std::uintptr_t kVideoHookRendererCallRva = 0x003fd462;
constexpr std::uintptr_t kVideoHookReturnRva = 0x003fd467;
constexpr std::uintptr_t kRendererWrapperRva = 0x0052cad0;

// Independent use of the same WumpusFrame::is_source_hdr byte elsewhere in
// discord_voice.node. We verify this instruction before patching so an update
// cannot silently change the layout we depend on.
constexpr std::uintptr_t kIndependentIsSourceHdrReadRva = 0x005ddaa1;

// At the Video Hook renderer callsite:
//   r12 = WumpusFrame*
//   byte [r12 + 0x1da] = WumpusFrame::is_source_hdr
constexpr std::size_t kWumpusIsSourceHdrOffset = 0x1da;

// Renderer source descriptor fields:
//   +0x178 = DXGI_FORMAT (u32)
//   +0x17c = primaries/gamut enum
//   +0x17d = transfer-function enum
constexpr std::size_t kSourceFormatOffset = 0x178;
constexpr std::size_t kSourcePrimariesOffset = 0x17c;
constexpr std::size_t kSourceTransferOffset = 0x17d;

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

enum class DetectionMode : LONG {
    Automatic = 0,
    ForceSdr = 1,
    ForceHdr10 = 2,
    ForceScRgb = 3
};

enum class Decision : LONG {
    DisabledBypass = 0,
    SdrBypass = 1,
    Hdr10 = 2,
    ScRgb = 3,
    HdrUnknownFormat = 4
};

struct Config {
    bool enabled = true;
    float white = 460.0f;
    float peak = 1000.0f;
    DetectionMode detectionMode = DetectionMode::Automatic;
};

// These symbols are intentionally C linkage because probe/relay.asm references
// them directly. LONG/LONG64 are naturally aligned globals on x64.
extern "C" {

volatile LONG g_DH_RuntimeEnabled = 1;
volatile LONG g_DH_DetectionMode = static_cast<LONG>(DetectionMode::Automatic);

void* volatile g_DH_ActiveMetadataPtr = nullptr;
void* volatile g_DH_OriginalRenderer = nullptr;

volatile LONG64 g_DH_RendererCalls = 0;
volatile LONG64 g_DH_DisabledBypassFrames = 0;
volatile LONG64 g_DH_SdrFramesBypassed = 0;
volatile LONG64 g_DH_HdrFramesCorrected = 0;
volatile LONG64 g_DH_Hdr10Frames = 0;
volatile LONG64 g_DH_ScRgbFrames = 0;
volatile LONG64 g_DH_HdrUnknownFormatFrames = 0;
volatile LONG64 g_DH_MetadataInjected = 0;
volatile LONG64 g_DH_SourceColorOverrides = 0;

volatile LONG g_DH_LastSourceIsHdr = -1;
volatile LONG g_DH_LastSourceFormat = -1;
volatile LONG g_DH_LastOriginalPrimaries = -1;
volatile LONG g_DH_LastOriginalTransfer = -1;
volatile LONG g_DH_LastEffectivePrimaries = -1;
volatile LONG g_DH_LastEffectiveTransfer = -1;
volatile LONG g_DH_LastDecision = -1;
volatile LONG g_DH_LastOriginalHdrMetadataNull = -1;

void DiscordHDRFixRelay();

} // extern "C"

alignas(16) HdrMetadata g_metadataRing[256]{};
volatile LONG g_metadataWriteIndex = 0;

volatile LONG g_hookInstalled = 0;
volatile LONG g_whiteBits = 0;
volatile LONG g_peakBits = 0;

std::uint8_t* g_nearRelay = nullptr;

wchar_t g_statusPath[MAX_PATH]{};
wchar_t g_configPath[MAX_PATH]{};
char g_signature[192] = "waiting for discord_voice.node";
char g_error[512]{};

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

void StoreFloatBits(volatile LONG* target, float value) {
    InterlockedExchange(target, static_cast<LONG>(FloatBits(value)));
}

float LoadFloatBits(volatile LONG* source, float fallback) {
    const auto bits = static_cast<std::uint32_t>(
        InterlockedCompareExchange(source, 0, 0)
    );
    const float value = BitsFloat(bits);
    return value > 0.0f ? value : fallback;
}

void SetSignature(const char* text) {
    strncpy_s(g_signature, text ? text : "", _TRUNCATE);
}

void SetError(const char* text) {
    strncpy_s(g_error, text ? text : "", _TRUNCATE);
}

const char* FormatName(LONG format) {
    switch (format) {
    case 10: return "R16G16B16A16_FLOAT";
    case 24: return "R10G10B10A2_UNORM";
    case 29: return "R8G8B8A8_UNORM_SRGB";
    case 91: return "B8G8R8A8_UNORM_SRGB";
    case 93: return "B8G8R8X8_UNORM_SRGB";
    default: return "other";
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

const char* DetectionModeName(LONG value) {
    switch (static_cast<DetectionMode>(value)) {
    case DetectionMode::Automatic: return "automatic";
    case DetectionMode::ForceSdr: return "force_sdr";
    case DetectionMode::ForceHdr10: return "force_hdr10_pq";
    case DetectionMode::ForceScRgb: return "force_scrgb";
    default: return "automatic";
    }
}

const char* DecisionName(LONG value) {
    switch (static_cast<Decision>(value)) {
    case Decision::DisabledBypass: return "disabled_bypass";
    case Decision::SdrBypass: return "sdr_bypass";
    case Decision::Hdr10: return "hdr10_rec2020_pq";
    case Decision::ScRgb: return "scrgb_rec709_linear";
    case Decision::HdrUnknownFormat: return "hdr_unknown_format_metadata_only";
    default: return "not_observed";
    }
}

void BuildPaths() {
    wchar_t temp[MAX_PATH]{};
    DWORD tempLen = GetTempPathW(MAX_PATH, temp);
    if (tempLen == 0 || tempLen >= MAX_PATH)
        wcscpy_s(temp, L".\\");

    swprintf_s(
        g_statusPath,
        L"%sDiscordHDRFix-v101-%lu.json",
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

    c.enabled =
        InterlockedCompareExchange(
            &g_DH_RuntimeEnabled,
            0,
            0
        ) != 0;

    c.white = LoadFloatBits(&g_whiteBits, 460.0f);
    c.peak = LoadFloatBits(&g_peakBits, 1000.0f);

    LONG mode = InterlockedCompareExchange(
        &g_DH_DetectionMode,
        0,
        0
    );

    if (mode < static_cast<LONG>(DetectionMode::Automatic) ||
        mode > static_cast<LONG>(DetectionMode::ForceScRgb)) {
        mode = static_cast<LONG>(DetectionMode::Automatic);
    }

    c.detectionMode = static_cast<DetectionMode>(mode);
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
        unsigned detectionMode = 0;
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

        if (sscanf_s(line, "detection_mode=%u", &detectionMode) == 1) {
            if (detectionMode <=
                static_cast<unsigned>(DetectionMode::ForceScRgb)) {
                c.detectionMode =
                    static_cast<DetectionMode>(detectionMode);
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
    const LONG sequence = InterlockedIncrement(&g_metadataWriteIndex);
    const unsigned index =
        static_cast<unsigned>(sequence) % 256u;

    HdrMetadata& m = g_metadataRing[index];
    m.sdrWhiteLevel = c.white;
    m.inputMaxLuminance = c.peak;
    m.state = 1;
    m.padding[0] = 0;
    m.padding[1] = 0;
    m.padding[2] = 0;

    MemoryBarrier();

    InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(&g_DH_ActiveMetadataPtr),
        static_cast<void*>(&m)
    );

    InterlockedExchange(
        &g_DH_RuntimeEnabled,
        c.enabled ? 1 : 0
    );

    InterlockedExchange(
        &g_DH_DetectionMode,
        static_cast<LONG>(c.detectionMode)
    );

    StoreFloatBits(&g_whiteBits, c.white);
    StoreFloatBits(&g_peakBits, c.peak);
}

bool ConfigChanged(const Config& a, const Config& b) {
    return
        a.enabled != b.enabled ||
        a.white != b.white ||
        a.peak != b.peak ||
        a.detectionMode != b.detectionMode;
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

    // Function establishes r12 = rdx (WumpusFrame*) near entry.
    constexpr std::uint8_t frameSave[] = {
        0x49, 0x89, 0xd4
    };

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

    // Independent direct bool read:
    // movzx eax, byte ptr [r13+0x1da]
    constexpr std::uint8_t independentHdrRead[] = {
        0x41, 0x0f, 0xb6, 0x85,
        0xda, 0x01, 0x00, 0x00
    };

    if (!EqualBytes(
            base + kVideoHookFrameSaveRva,
            frameSave,
            sizeof(frameSave))) {
        SetSignature("Video Hook WumpusFrame register signature mismatch");
        SetError("r12=WumpusFrame signature changed; refusing to patch");
        return false;
    }

    if (!EqualBytes(
            base + kVideoHookNullMetadataRva,
            nullArg,
            sizeof(nullArg))) {
        SetSignature("Video Hook null HDR metadata signature mismatch");
        SetError("Video Hook HDR metadata callsite changed");
        return false;
    }

    if (!EqualBytes(
            base + kVideoHookRendererCallRva,
            callBytes,
            sizeof(callBytes))) {
        SetSignature("Video Hook renderer call signature mismatch");
        SetError("Video Hook renderer CALL changed");
        return false;
    }

    if (!EqualBytes(
            base + kRendererWrapperRva,
            wrapperPrologue,
            sizeof(wrapperPrologue))) {
        SetSignature("renderer signature mismatch");
        SetError("renderer wrapper signature changed");
        return false;
    }

    if (!EqualBytes(
            base + kIndependentIsSourceHdrReadRva,
            independentHdrRead,
            sizeof(independentHdrRead))) {
        SetSignature("is_source_hdr layout signature mismatch");
        SetError("WumpusFrame is_source_hdr offset changed; refusing to patch");
        return false;
    }

    // Verify original rel32 CALL resolves to the renderer wrapper we analyzed.
    const auto* call = base + kVideoHookRendererCallRva;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, call + 1, sizeof(displacement));

    const std::uintptr_t destination =
        reinterpret_cast<std::uintptr_t>(call + 5) +
        static_cast<std::intptr_t>(displacement);

    if (destination !=
        reinterpret_cast<std::uintptr_t>(
            base + kRendererWrapperRva
        )) {
        SetSignature("renderer call target mismatch");
        SetError("renderer wrapper destination changed");
        return false;
    }

    SetSignature("matched exact analyzed build + is_source_hdr layout");
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
        static_cast<std::uintptr_t>(
            info.dwAllocationGranularity
        );

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
                AlignUp(
                    std::max(cursor, regionBase),
                    granularity
                );

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

bool BuildNearRelay(std::uint8_t* callSite) {
    constexpr SIZE_T allocationSize = 0x1000;

    g_nearRelay = static_cast<std::uint8_t*>(
        AllocateNear(callSite, allocationSize)
    );

    if (!g_nearRelay) {
        SetError("VirtualAlloc failed for near relay");
        return false;
    }

    std::memset(g_nearRelay, 0xcc, allocationSize);

    std::size_t p = 0;

    // mov rax, DiscordHDRFixRelay
    g_nearRelay[p++] = 0x48;
    g_nearRelay[p++] = 0xb8;

    WriteImmediate(
        g_nearRelay + p,
        reinterpret_cast<std::uintptr_t>(&DiscordHDRFixRelay)
    );
    p += sizeof(std::uintptr_t);

    // jmp rax
    g_nearRelay[p++] = 0xff;
    g_nearRelay[p++] = 0xe0;

    FlushInstructionCache(
        GetCurrentProcess(),
        g_nearRelay,
        allocationSize
    );

    return true;
}

bool PatchVideoHookCall(HMODULE voice) {
    auto* base = reinterpret_cast<std::uint8_t*>(voice);
    auto* callSite = base + kVideoHookRendererCallRva;

    InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(&g_DH_OriginalRenderer),
        static_cast<void*>(base + kRendererWrapperRva)
    );

    if (!BuildNearRelay(callSite))
        return false;

    const std::intptr_t from =
        reinterpret_cast<std::intptr_t>(callSite + 5);

    const std::intptr_t to =
        reinterpret_cast<std::intptr_t>(g_nearRelay);

    const std::intptr_t delta = to - from;

    if (delta < static_cast<std::intptr_t>(INT32_MIN) ||
        delta > static_cast<std::intptr_t>(INT32_MAX)) {
        SetError("near relay is outside CALL rel32 range");
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

unsigned long long ReadCounter(volatile LONG64* value) {
    return static_cast<unsigned long long>(
        InterlockedCompareExchange64(value, 0, 0)
    );
}

LONG ReadLong(volatile LONG* value) {
    return InterlockedCompareExchange(value, 0, 0);
}

void WriteStatus() {
    const float white =
        LoadFloatBits(&g_whiteBits, 460.0f);

    const float peak =
        LoadFloatBits(&g_peakBits, 1000.0f);

    const LONG mode = ReadLong(&g_DH_DetectionMode);
    const LONG format = ReadLong(&g_DH_LastSourceFormat);
    const LONG sourceIsHdr = ReadLong(&g_DH_LastSourceIsHdr);
    const LONG originalPrimaries =
        ReadLong(&g_DH_LastOriginalPrimaries);
    const LONG originalTransfer =
        ReadLong(&g_DH_LastOriginalTransfer);
    const LONG effectivePrimaries =
        ReadLong(&g_DH_LastEffectivePrimaries);
    const LONG effectiveTransfer =
        ReadLong(&g_DH_LastEffectiveTransfer);
    const LONG decision =
        ReadLong(&g_DH_LastDecision);
    const LONG originalMetadataNull =
        ReadLong(&g_DH_LastOriginalHdrMetadataNull);

    char json[8192]{};

    _snprintf_s(
        json,
        sizeof(json),
        _TRUNCATE,
        "{\n"
        "  \"version\": \"1.0.1-auto-test\",\n"
        "  \"pid\": %lu,\n"
        "  \"mode\": \"automatic-hdr-sdr-detection\",\n"
        "  \"hook_installed\": %s,\n"
        "  \"signature\": \"%s\",\n"
        "  \"detection_source\": \"WumpusFrame.is_source_hdr\",\n"
        "  \"wumpus_is_source_hdr_offset\": \"0x1da\",\n"
        "  \"renderer_wrapper_rva\": \"0x52cad0\",\n"
        "  \"video_hook_return_rva\": \"0x3fd467\",\n"
        "  \"enabled\": %s,\n"
        "  \"detection_mode\": \"%s\",\n"
        "  \"sdr_white_level\": %.3f,\n"
        "  \"input_max_luminance\": %.3f,\n"
        "  \"metadata_state\": 1,\n"
        "  \"metadata_size\": 12,\n"
        "  \"last_source_is_hdr\": %s,\n"
        "  \"last_decision\": \"%s\",\n"
        "  \"source_format\": %ld,\n"
        "  \"source_format_name\": \"%s\",\n"
        "  \"original_primaries\": %ld,\n"
        "  \"original_primaries_name\": \"%s\",\n"
        "  \"original_transfer\": %ld,\n"
        "  \"original_transfer_name\": \"%s\",\n"
        "  \"effective_primaries\": %ld,\n"
        "  \"effective_primaries_name\": \"%s\",\n"
        "  \"effective_transfer\": %ld,\n"
        "  \"effective_transfer_name\": \"%s\",\n"
        "  \"renderer_calls\": %llu,\n"
        "  \"disabled_bypass_frames\": %llu,\n"
        "  \"sdr_frames_bypassed\": %llu,\n"
        "  \"hdr_frames_corrected\": %llu,\n"
        "  \"hdr10_frames\": %llu,\n"
        "  \"scrgb_frames\": %llu,\n"
        "  \"hdr_unknown_format_frames\": %llu,\n"
        "  \"hdr_metadata_injected\": %llu,\n"
        "  \"source_color_overrides\": %llu,\n"
        "  \"last_original_hdr_metadata_null\": %s,\n"
        "  \"error\": \"%s\"\n"
        "}\n",
        GetCurrentProcessId(),
        ReadLong(&g_hookInstalled) ? "true" : "false",
        g_signature,
        ReadLong(&g_DH_RuntimeEnabled) ? "true" : "false",
        DetectionModeName(mode),
        static_cast<double>(white),
        static_cast<double>(peak),
        sourceIsHdr < 0
            ? "null"
            : (sourceIsHdr ? "true" : "false"),
        DecisionName(decision),
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
        ReadCounter(&g_DH_RendererCalls),
        ReadCounter(&g_DH_DisabledBypassFrames),
        ReadCounter(&g_DH_SdrFramesBypassed),
        ReadCounter(&g_DH_HdrFramesCorrected),
        ReadCounter(&g_DH_Hdr10Frames),
        ReadCounter(&g_DH_ScRgbFrames),
        ReadCounter(&g_DH_HdrUnknownFormatFrames),
        ReadCounter(&g_DH_MetadataInjected),
        ReadCounter(&g_DH_SourceColorOverrides),
        originalMetadataNull < 0
            ? "null"
            : (originalMetadataNull ? "true" : "false"),
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

DWORD WINAPI WorkerThread(void*) {
    BuildPaths();

    StoreFloatBits(&g_whiteBits, 460.0f);
    StoreFloatBits(&g_peakBits, 1000.0f);

    Config initial = ReadConfig();
    PublishConfig(initial);

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

    InterlockedExchange(&g_hookInstalled, 1);
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
