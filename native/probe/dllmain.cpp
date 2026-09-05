#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cwchar>
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

// Discord's own valid HDR metadata builder writes:
//   float +0x00
//   float +0x04
//   byte  +0x08 = 1
//
// The renderer reads all three:
//   RVA 0x52d02b -> float +0x00
//   RVA 0x52d246 -> float +0x04
//   RVA 0x52d2c2 -> byte  +0x08
//
// v0.6 only guaranteed the first 8 bytes. v0.6.1 supplies the complete
// structure and explicitly uses Discord's own "valid HDR metadata" state.
struct HdrMetadata {
    float sdrWhiteLevel;       // +0x00
    float inputMaxLuminance;   // +0x04
    std::uint8_t state;        // +0x08; 1 = valid HDR metadata
    std::uint8_t padding[3];   // +0x09..+0x0b
};

static_assert(sizeof(HdrMetadata) == 12);
static_assert(offsetof(HdrMetadata, state) == 8);

constexpr std::uint8_t kValidHdrMetadataState = 1;
constexpr float kDefaultSdrWhite = 200.0f;
constexpr float kDefaultInputMax = 1000.0f;

// Ring avoids mutating metadata currently being consumed by Discord's render
// thread. Configuration changes are human-speed, so 256 immutable snapshots
// provide a very large safety margin before a slot can ever be reused.
alignas(16) HdrMetadata g_metadataRing[256]{};
std::atomic<unsigned> g_metadataWriteIndex{0};

void* volatile g_activeMetadataPtr = nullptr;

std::atomic<bool> g_enabled{true};
std::atomic<std::uint32_t> g_whiteBits{0};
std::atomic<std::uint32_t> g_peakBits{0};
std::atomic<bool> g_hookInstalled{false};

std::uint8_t* g_relay = nullptr;
volatile LONG64* g_rendererCalls = nullptr;
volatile LONG64* g_injectedCalls = nullptr;

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

void BuildPaths() {
    wchar_t temp[MAX_PATH]{};
    DWORD tempLen = GetTempPathW(MAX_PATH, temp);
    if (tempLen == 0 || tempLen >= MAX_PATH)
        wcscpy_s(temp, L".\\");

    // Keep the v06 prefix so the existing Vencord native helper continues to
    // discover the status file without changing its IPC contract.
    swprintf_s(
        g_statusPath,
        L"%sDiscordHDRFix-v06-%lu.json",
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

struct Config {
    bool enabled = true;
    float white = kDefaultSdrWhite;
    float peak = kDefaultInputMax;
};

Config CurrentConfig() {
    Config c;
    c.enabled = g_enabled.load(std::memory_order_relaxed);

    c.white = BitsFloat(g_whiteBits.load(std::memory_order_relaxed));
    if (!(c.white > 0.0f))
        c.white = kDefaultSdrWhite;

    c.peak = BitsFloat(g_peakBits.load(std::memory_order_relaxed));
    if (!(c.peak > 0.0f))
        c.peak = kDefaultInputMax;

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
    }

    fclose(f);

    // Match the plugin's independent ranges. In particular, do NOT force
    // input_max >= sdr_white: the tester is intentionally evaluating values
    // such as 600 / 460 and Discord's native structure itself does not encode
    // that constraint.
    c.white = std::clamp(c.white, 40.0f, 1000.0f);
    c.peak = std::clamp(c.peak, 100.0f, 10000.0f);

    return c;
}

void PublishConfig(const Config& c) {
    const unsigned index =
        g_metadataWriteIndex.fetch_add(1, std::memory_order_relaxed)
        % 256u;

    HdrMetadata& m = g_metadataRing[index];
    m.sdrWhiteLevel = c.white;
    m.inputMaxLuminance = c.peak;
    m.state = kValidHdrMetadataState;
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

    // Verify rel32 resolves to the wrapper we analyzed.
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

bool BuildRelay(
    std::uint8_t* callSite,
    std::uint8_t* rendererWrapper
) {
    constexpr SIZE_T allocationSize = 0x1000;
    constexpr SIZE_T rendererCounterOffset = 0x100;
    constexpr SIZE_T injectedCounterOffset = 0x108;

    g_relay = static_cast<std::uint8_t*>(
        AllocateNear(callSite, allocationSize)
    );

    if (!g_relay) {
        SetError("VirtualAlloc failed for Video Hook relay");
        return false;
    }

    std::memset(g_relay, 0xcc, allocationSize);

    g_rendererCalls =
        reinterpret_cast<volatile LONG64*>(
            g_relay + rendererCounterOffset
        );

    g_injectedCalls =
        reinterpret_cast<volatile LONG64*>(
            g_relay + injectedCounterOffset
        );

    *g_rendererCalls = 0;
    *g_injectedCalls = 0;

    std::size_t p = 0;

    auto emitRipLockInc = [&](SIZE_T counterOffset) -> bool {
        // lock inc qword ptr [rip + disp32]
        g_relay[p++] = 0xf0;
        g_relay[p++] = 0x48;
        g_relay[p++] = 0xff;
        g_relay[p++] = 0x05;

        const std::size_t dispOffset = p;
        p += sizeof(std::int32_t);

        const std::intptr_t target =
            reinterpret_cast<std::intptr_t>(
                g_relay + counterOffset
            );

        const std::intptr_t after =
            reinterpret_cast<std::intptr_t>(
                g_relay + dispOffset + sizeof(std::int32_t)
            );

        const std::intptr_t delta = target - after;
        if (delta < std::numeric_limits<std::int32_t>::min() ||
            delta > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }

        WriteImmediate(
            g_relay + dispOffset,
            static_cast<std::int32_t>(delta)
        );

        return true;
    };

    if (!emitRipLockInc(rendererCounterOffset)) {
        SetError("internal renderer counter displacement overflow");
        return false;
    }

    // mov rax, &g_activeMetadataPtr
    g_relay[p++] = 0x48;
    g_relay[p++] = 0xb8;
    WriteImmediate(
        g_relay + p,
        reinterpret_cast<std::uintptr_t>(&g_activeMetadataPtr)
    );
    p += sizeof(std::uintptr_t);

    // mov rax, [rax]
    g_relay[p++] = 0x48;
    g_relay[p++] = 0x8b;
    g_relay[p++] = 0x00;

    // test rax, rax
    g_relay[p++] = 0x48;
    g_relay[p++] = 0x85;
    g_relay[p++] = 0xc0;

    // je skipInjection
    const std::size_t jeOpcode = p;
    g_relay[p++] = 0x74;
    const std::size_t jeDisp = p;
    g_relay[p++] = 0x00;

    // At the relay entry the original CALL has already pushed its return
    // address. Discord's ninth argument was [caller_rsp + 0x40], therefore it
    // is [relay_rsp + 0x48].
    //
    // mov [rsp + 0x48], rax
    g_relay[p++] = 0x48;
    g_relay[p++] = 0x89;
    g_relay[p++] = 0x44;
    g_relay[p++] = 0x24;
    g_relay[p++] = 0x48;

    if (!emitRipLockInc(injectedCounterOffset)) {
        SetError("internal injected counter displacement overflow");
        return false;
    }

    const std::size_t skipInjection = p;

    const std::ptrdiff_t shortJump =
        static_cast<std::ptrdiff_t>(skipInjection) -
        static_cast<std::ptrdiff_t>(jeDisp + 1);

    if (shortJump < -128 || shortJump > 127) {
        SetError("internal relay short jump overflow");
        return false;
    }

    g_relay[jeDisp] =
        static_cast<std::uint8_t>(
            static_cast<std::int8_t>(shortJump)
        );

    (void)jeOpcode;

    // mov rax, rendererWrapper
    g_relay[p++] = 0x48;
    g_relay[p++] = 0xb8;
    WriteImmediate(
        g_relay + p,
        reinterpret_cast<std::uintptr_t>(rendererWrapper)
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
    auto* wrapper = base + kRendererWrapperRva;

    if (!BuildRelay(callSite, wrapper))
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

unsigned long long ReadCounter(volatile LONG64* counter) {
    if (!counter)
        return 0;

    return static_cast<unsigned long long>(
        InterlockedCompareExchange64(counter, 0, 0)
    );
}

void WriteStatus() {
    const float white =
        BitsFloat(g_whiteBits.load(std::memory_order_relaxed));
    const float peak =
        BitsFloat(g_peakBits.load(std::memory_order_relaxed));

    const unsigned long long rendererCalls =
        ReadCounter(g_rendererCalls);
    const unsigned long long injectedCalls =
        ReadCounter(g_injectedCalls);

    char json[4096]{};

    _snprintf_s(
        json,
        sizeof(json),
        _TRUNCATE,
        "{\n"
        "  \"version\": \"0.6.1\",\n"
        "  \"pid\": %lu,\n"
        "  \"mode\": \"discord-native-hdr-metadata-injection\",\n"
        "  \"hook_installed\": %s,\n"
        "  \"signature\": \"%s\",\n"
        "  \"renderer_wrapper_rva\": \"0x52cad0\",\n"
        "  \"video_hook_return_rva\": \"0x3fd467\",\n"
        "  \"enabled\": %s,\n"
        "  \"sdr_white_level\": %.3f,\n"
        "  \"input_max_luminance\": %.3f,\n"
        "  \"metadata_state\": 1,\n"
        "  \"metadata_size\": 12,\n"
        "  \"renderer_calls\": %llu,\n"
        "  \"video_hook_calls\": %llu,\n"
        "  \"hdr_metadata_injected\": %llu,\n"
        "  \"last_original_hdr_metadata_null\": true,\n"
        "  \"error\": \"%s\"\n"
        "}\n",
        GetCurrentProcessId(),
        g_hookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
        g_signature,
        g_enabled.load(std::memory_order_relaxed) ? "true" : "false",
        static_cast<double>(white),
        static_cast<double>(peak),
        rendererCalls,
        rendererCalls,
        injectedCalls,
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
        a.peak != b.peak;
}

DWORD WINAPI WorkerThread(void*) {
    BuildPaths();

    g_whiteBits.store(
        FloatBits(kDefaultSdrWhite),
        std::memory_order_relaxed
    );
    g_peakBits.store(
        FloatBits(kDefaultInputMax),
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
