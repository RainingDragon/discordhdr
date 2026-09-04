#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr std::uint32_t kExpectedTimeDateStamp = 0x6A95B8A4u;
constexpr std::uint32_t kExpectedSizeOfImage = 0x00FD8000u;
constexpr std::uintptr_t kRendererWrapperRva = 0x0052CAD0u;
constexpr std::uintptr_t kVideoHookReturnRva = 0x003FD467u;
constexpr std::uintptr_t kVideoHookNullMetadataRva = 0x003FD443u;
constexpr std::size_t kHookPatchLength = 16;

constexpr unsigned char kRendererPrologue[kHookPatchLength] = {
    0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
    0x56,0x57,0x55,0x53,0x48,0x83,0xEC,0x68
};

constexpr unsigned char kVideoHookSequence[] = {
    0x48,0xC7,0x44,0x24,0x40,0x00,0x00,0x00,0x00,
    0x48,0x8D,0x8C,0x24,0xF0,0x00,0x00,0x00,
    0x4C,0x8D,0x8C,0x24,0x18,0x03,0x00,0x00,
    0x4C,0x89,0xFA,
    0x49,0x89,0xE8,
    0xE8,0x69,0xF6,0x12,0x00
};

struct HdrMetadata {
    float sdrWhiteLevel;
    float inputMaxLuminance;
    std::uint8_t state;
    std::uint8_t padding[3];
};
static_assert(sizeof(HdrMetadata) == 12);

using RendererWrapperFn = void* (*)(
    void* result,
    void* renderer,
    void* source,
    void* output,
    void* arg5,
    void* arg6,
    void* arg7,
    void* arg8,
    HdrMetadata* hdrMetadata
);

HMODULE g_voiceModule = nullptr;
RendererWrapperFn g_originalRenderer = nullptr;
void* g_trampoline = nullptr;

std::atomic<bool> g_hookInstalled{false};
std::atomic<bool> g_enabled{true};
std::atomic<float> g_sdrWhite{200.0f};
std::atomic<float> g_inputMax{1000.0f};
std::atomic<std::uint64_t> g_rendererCalls{0};
std::atomic<std::uint64_t> g_videoHookCalls{0};
std::atomic<std::uint64_t> g_injectedFrames{0};
std::atomic<bool> g_lastOriginalMetadataWasNull{false};

wchar_t g_statusPath[MAX_PATH]{};
wchar_t g_configPath[MAX_PATH]{};
char g_error[384]{};
char g_signatureStatus[96] = "not checked";

std::uintptr_t ModuleBase() {
    return reinterpret_cast<std::uintptr_t>(g_voiceModule);
}

void BuildPaths() {
    wchar_t temp[MAX_PATH]{};
    DWORD len = GetTempPathW(MAX_PATH, temp);
    if (len == 0 || len >= MAX_PATH)
        wcscpy_s(temp, L".\\");

    swprintf_s(
        g_statusPath,
        L"%sDiscordHDRFix-v06-%lu.json",
        temp,
        GetCurrentProcessId()
    );

    wchar_t localAppData[MAX_PATH]{};
    DWORD envLen = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (envLen == 0 || envLen >= MAX_PATH) {
        g_configPath[0] = L'\0';
        return;
    }

    swprintf_s(
        g_configPath,
        L"%s\\DiscordHDRFix\\tone-map.cfg",
        localAppData
    );
}

void SetError(const char* text) {
    strncpy_s(g_error, text ? text : "", _TRUNCATE);
}

void WriteAbsoluteJump(unsigned char* destination, const void* target) {
    destination[0] = 0xFF;
    destination[1] = 0x25;
    destination[2] = 0x00;
    destination[3] = 0x00;
    destination[4] = 0x00;
    destination[5] = 0x00;
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(destination + 6, &address, sizeof(address));
}

bool ValidateDiscordBuild() {
    if (!g_voiceModule) {
        SetError("discord_voice.node is not loaded");
        return false;
    }

    auto* base = reinterpret_cast<unsigned char*>(g_voiceModule);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        SetError("discord_voice.node DOS header mismatch");
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        SetError("discord_voice.node PE32+ header mismatch");
        return false;
    }

    if (nt->FileHeader.TimeDateStamp != kExpectedTimeDateStamp ||
        nt->OptionalHeader.SizeOfImage != kExpectedSizeOfImage) {
        strcpy_s(g_signatureStatus, "unsupported discord_voice.node build");
        SetError("discord_voice.node version changed; refusing to patch");
        return false;
    }

    const auto* renderer = base + kRendererWrapperRva;
    if (std::memcmp(renderer, kRendererPrologue, sizeof(kRendererPrologue)) != 0) {
        strcpy_s(g_signatureStatus, "renderer signature mismatch");
        SetError("renderer wrapper signature did not match");
        return false;
    }

    const auto* videoHookSequence = base + kVideoHookNullMetadataRva;
    if (std::memcmp(videoHookSequence, kVideoHookSequence, sizeof(kVideoHookSequence)) != 0) {
        strcpy_s(g_signatureStatus, "video-hook signature mismatch");
        SetError("video-hook HDR metadata callsite signature did not match");
        return false;
    }

    strcpy_s(g_signatureStatus, "matched exact analyzed build");
    SetError("");
    return true;
}

void LoadConfig() {
    if (g_configPath[0] == L'\0')
        return;

    std::FILE* file = nullptr;
    if (_wfopen_s(&file, g_configPath, L"rt") != 0 || !file)
        return;

    bool enabled = g_enabled.load(std::memory_order_relaxed);
    float white = g_sdrWhite.load(std::memory_order_relaxed);
    float peak = g_inputMax.load(std::memory_order_relaxed);

    char line[256]{};
    while (std::fgets(line, sizeof(line), file)) {
        int enabledInt = enabled ? 1 : 0;
        float value = 0.0f;

        if (std::sscanf(line, "enabled=%d", &enabledInt) == 1) {
            enabled = enabledInt != 0;
            continue;
        }
        if (std::sscanf(line, "sdr_white=%f", &value) == 1) {
            white = value;
            continue;
        }
        if (std::sscanf(line, "input_max=%f", &value) == 1) {
            peak = value;
            continue;
        }
    }

    std::fclose(file);
    white = std::clamp(white, 40.0f, 1000.0f);
    peak = std::clamp(peak, 100.0f, 10000.0f);

    g_enabled.store(enabled, std::memory_order_relaxed);
    g_sdrWhite.store(white, std::memory_order_relaxed);
    g_inputMax.store(peak, std::memory_order_relaxed);
}

void WriteStatus() {
    if (g_statusPath[0] == L'\0')
        BuildPaths();

    char json[3072]{};
    _snprintf_s(
        json,
        sizeof(json),
        _TRUNCATE,
        "{\n"
        "  \"version\": \"0.6\",\n"
        "  \"pid\": %lu,\n"
        "  \"mode\": \"discord-native-hdr-metadata-injection\",\n"
        "  \"hook_installed\": %s,\n"
        "  \"signature\": \"%s\",\n"
        "  \"renderer_wrapper_rva\": \"0x52cad0\",\n"
        "  \"video_hook_return_rva\": \"0x3fd467\",\n"
        "  \"enabled\": %s,\n"
        "  \"sdr_white_level\": %.3f,\n"
        "  \"input_max_luminance\": %.3f,\n"
        "  \"renderer_calls\": %llu,\n"
        "  \"video_hook_calls\": %llu,\n"
        "  \"hdr_metadata_injected\": %llu,\n"
        "  \"last_original_hdr_metadata_null\": %s,\n"
        "  \"error\": \"%s\"\n"
        "}\n",
        GetCurrentProcessId(),
        g_hookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
        g_signatureStatus,
        g_enabled.load(std::memory_order_relaxed) ? "true" : "false",
        g_sdrWhite.load(std::memory_order_relaxed),
        g_inputMax.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(g_rendererCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_videoHookCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_injectedFrames.load(std::memory_order_relaxed)),
        g_lastOriginalMetadataWasNull.load(std::memory_order_relaxed) ? "true" : "false",
        g_error
    );

    wchar_t tmpPath[MAX_PATH]{};
    swprintf_s(tmpPath, L"%s.tmp", g_statusPath);

    HANDLE file = CreateFileW(
        tmpPath,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(file, json, static_cast<DWORD>(std::strlen(json)), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    MoveFileExW(tmpPath, g_statusPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

extern "C" void* HookRenderer(
    void* result,
    void* renderer,
    void* source,
    void* output,
    void* arg5,
    void* arg6,
    void* arg7,
    void* arg8,
    HdrMetadata* hdrMetadata
) {
    g_rendererCalls.fetch_add(1, std::memory_order_relaxed);

    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool isVideoHookCall =
        g_voiceModule && returnAddress == ModuleBase() + kVideoHookReturnRva;

    if (isVideoHookCall) {
        g_videoHookCalls.fetch_add(1, std::memory_order_relaxed);
        const bool wasNull = hdrMetadata == nullptr;
        g_lastOriginalMetadataWasNull.store(wasNull, std::memory_order_relaxed);

        if (wasNull && g_enabled.load(std::memory_order_relaxed)) {
            HdrMetadata injected{
                g_sdrWhite.load(std::memory_order_relaxed),
                g_inputMax.load(std::memory_order_relaxed),
                1,
                { 0, 0, 0 }
            };

            g_injectedFrames.fetch_add(1, std::memory_order_relaxed);
            return g_originalRenderer(
                result, renderer, source, output,
                arg5, arg6, arg7, arg8, &injected
            );
        }
    }

    return g_originalRenderer(
        result, renderer, source, output,
        arg5, arg6, arg7, arg8, hdrMetadata
    );
}

bool InstallRendererHook() {
    auto* target = reinterpret_cast<unsigned char*>(ModuleBase() + kRendererWrapperRva);
    constexpr std::size_t jumpSize = 14;
    constexpr std::size_t trampolineSize = kHookPatchLength + jumpSize;

    auto* trampoline = reinterpret_cast<unsigned char*>(
        VirtualAlloc(nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)
    );
    if (!trampoline) {
        SetError("VirtualAlloc failed for renderer trampoline");
        return false;
    }

    std::memcpy(trampoline, target, kHookPatchLength);
    WriteAbsoluteJump(trampoline + kHookPatchLength, target + kHookPatchLength);

    g_trampoline = trampoline;
    g_originalRenderer = reinterpret_cast<RendererWrapperFn>(trampoline);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, kHookPatchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        SetError("VirtualProtect failed for renderer wrapper");
        return false;
    }

    unsigned char patch[kHookPatchLength]{};
    WriteAbsoluteJump(patch, reinterpret_cast<const void*>(&HookRenderer));
    for (std::size_t i = jumpSize; i < kHookPatchLength; ++i)
        patch[i] = 0x90;

    std::memcpy(target, patch, kHookPatchLength);
    FlushInstructionCache(GetCurrentProcess(), target, kHookPatchLength);

    DWORD ignored = 0;
    VirtualProtect(target, kHookPatchLength, oldProtect, &ignored);

    g_hookInstalled.store(true, std::memory_order_release);
    SetError("");
    return true;
}

DWORD WINAPI WorkerThread(void*) {
    BuildPaths();
    LoadConfig();

    strcpy_s(g_signatureStatus, "waiting for discord_voice.node");
    WriteStatus();

    for (int i = 0; i < 3000; ++i) {
        g_voiceModule = GetModuleHandleW(L"discord_voice.node");
        if (g_voiceModule)
            break;
        Sleep(100);
    }

    if (!g_voiceModule) {
        SetError("discord_voice.node was not loaded within 300 seconds");
        WriteStatus();
        return 0;
    }

    if (!ValidateDiscordBuild()) {
        WriteStatus();
        return 0;
    }

    if (!InstallRendererHook()) {
        WriteStatus();
        return 0;
    }

    for (;;) {
        LoadConfig();
        WriteStatus();
        Sleep(500);
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, &WorkerThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
