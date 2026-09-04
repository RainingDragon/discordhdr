#include <windows.h>
#include <winnt.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using EncoderAddFrameNativeFn =
    bool (*)(void* encoderHandle,
             const void* frameInfo,
             const void* encodeOptions,
             void* frameHandle);

constexpr std::uint8_t kTargetSignature[] = {
    0x56,
    0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00,
    0x4D, 0x89, 0xCA,
    0x48, 0x89, 0xC8,
    0x41, 0x0F, 0xB7, 0x08,
    0x4D, 0x8B, 0x48, 0x08
};

constexpr SIZE_T kPatchBytes = 14;

std::atomic<std::uint64_t> g_framesSeen{0};
std::atomic<std::uintptr_t> g_lastFrameHandle{0};
std::atomic<bool> g_hookInstalled{false};

EncoderAddFrameNativeFn g_originalAddFrameNative = nullptr;
DWORD g_targetRva = 0;
std::uint32_t g_signatureMatches = 0;

wchar_t g_statusPath[MAX_PATH]{};
char g_error[320]{};

void BuildStatusPath() {
    wchar_t temp[MAX_PATH]{};
    DWORD len = GetTempPathW(MAX_PATH, temp);
    if (len == 0 || len >= MAX_PATH) {
        wcscpy_s(temp, L".\\");
    }

    swprintf_s(
        g_statusPath,
        L"%sDiscordHDRFix-probe-%lu.json",
        temp,
        GetCurrentProcessId()
    );
}

std::string EscapeJson(const char* input) {
    std::string out;
    if (!input) return out;

    for (const char* p = input; *p; ++p) {
        switch (*p) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += *p; break;
        }
    }
    return out;
}

void WriteStatus() {
    if (g_statusPath[0] == L'\0')
        BuildStatusPath();

    const auto frames = g_framesSeen.load(std::memory_order_relaxed);
    const auto lastHandle = g_lastFrameHandle.load(std::memory_order_relaxed);
    const std::string error = EscapeJson(g_error);

    char json[2300]{};
    _snprintf_s(
        json,
        sizeof(json),
        _TRUNCATE,
        "{\n"
        "  \"version\": \"0.5.1\",\n"
        "  \"pid\": %lu,\n"
        "  \"hook_installed\": %s,\n"
        "  \"probe_method\": \"inline-detour\",\n"
        "  \"target\": \"windows-static-native-frame-submit\",\n"
        "  \"target_rva\": \"0x%lx\",\n"
        "  \"signature_matches\": %u,\n"
        "  \"frames_seen\": %llu,\n"
        "  \"last_frame_handle\": \"0x%llx\",\n"
        "  \"mode\": \"pass-through\",\n"
        "  \"error\": \"%s\"\n"
        "}\n",
        GetCurrentProcessId(),
        g_hookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
        static_cast<unsigned long>(g_targetRva),
        static_cast<unsigned>(g_signatureMatches),
        static_cast<unsigned long long>(frames),
        static_cast<unsigned long long>(lastHandle),
        error.c_str()
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

    MoveFileExW(
        tmpPath,
        g_statusPath,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
    );
}

void EmitAbsoluteJump(std::uint8_t* dst, const void* target) {
    dst[0] = 0xFF;
    dst[1] = 0x25;
    dst[2] = 0x00;
    dst[3] = 0x00;
    dst[4] = 0x00;
    dst[5] = 0x00;

    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(dst + 6, &address, sizeof(address));
}

bool HookAddFrameNative(
    void* encoderHandle,
    const void* frameInfo,
    const void* encodeOptions,
    void* frameHandle
) {
    const auto count = g_framesSeen.fetch_add(1, std::memory_order_relaxed) + 1;
    g_lastFrameHandle.store(
        reinterpret_cast<std::uintptr_t>(frameHandle),
        std::memory_order_relaxed
    );

    if ((count % 60u) == 1u)
        WriteStatus();

    return g_originalAddFrameNative(
        encoderHandle,
        frameInfo,
        encodeOptions,
        frameHandle
    );
}

bool FindTextSection(
    HMODULE module,
    std::uint8_t** textBaseOut,
    SIZE_T* textSizeOut
) {
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        strcpy_s(g_error, "discord_voice.node has an invalid DOS header");
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        strcpy_s(g_error, "discord_voice.node is not a valid x64 PE image");
        return false;
    }

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        char name[IMAGE_SIZEOF_SHORT_NAME + 1]{};
        std::memcpy(name, section->Name, IMAGE_SIZEOF_SHORT_NAME);

        if (std::strcmp(name, ".text") == 0) {
            *textBaseOut = base + section->VirtualAddress;
            *textSizeOut = static_cast<SIZE_T>(section->Misc.VirtualSize);
            return true;
        }
    }

    strcpy_s(g_error, "discord_voice.node .text section was not found");
    return false;
}

void* FindUniqueSignature(HMODULE module) {
    std::uint8_t* text = nullptr;
    SIZE_T textSize = 0;

    if (!FindTextSection(module, &text, &textSize))
        return nullptr;

    void* match = nullptr;
    g_signatureMatches = 0;

    if (textSize < sizeof(kTargetSignature)) {
        strcpy_s(g_error, "discord_voice.node .text section is unexpectedly small");
        return nullptr;
    }

    for (SIZE_T i = 0; i <= textSize - sizeof(kTargetSignature); ++i) {
        if (std::memcmp(text + i, kTargetSignature, sizeof(kTargetSignature)) == 0) {
            ++g_signatureMatches;
            match = text + i;
        }
    }

    if (g_signatureMatches != 1) {
        _snprintf_s(
            g_error,
            sizeof(g_error),
            _TRUNCATE,
            "native frame-submit signature match count was %u (expected exactly 1)",
            static_cast<unsigned>(g_signatureMatches)
        );
        return nullptr;
    }

    g_targetRva = static_cast<DWORD>(
        reinterpret_cast<std::uint8_t*>(match) -
        reinterpret_cast<std::uint8_t*>(module)
    );

    return match;
}

bool InstallInlineHook(void* target) {
    auto* targetBytes = reinterpret_cast<std::uint8_t*>(target);

    auto* trampoline = reinterpret_cast<std::uint8_t*>(VirtualAlloc(
        nullptr,
        kPatchBytes + 14,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    ));

    if (!trampoline) {
        strcpy_s(g_error, "VirtualAlloc failed while creating the trampoline");
        return false;
    }

    std::memcpy(trampoline, targetBytes, kPatchBytes);
    EmitAbsoluteJump(trampoline + kPatchBytes, targetBytes + kPatchBytes);

    DWORD trampolineProtect = 0;
    if (!VirtualProtect(
            trampoline,
            kPatchBytes + 14,
            PAGE_EXECUTE_READ,
            &trampolineProtect
        )) {
        strcpy_s(g_error, "VirtualProtect failed while finalizing the trampoline");
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    FlushInstructionCache(
        GetCurrentProcess(),
        trampoline,
        kPatchBytes + 14
    );

    g_originalAddFrameNative =
        reinterpret_cast<EncoderAddFrameNativeFn>(trampoline);

    std::uint8_t patch[kPatchBytes]{};
    EmitAbsoluteJump(patch, reinterpret_cast<void*>(&HookAddFrameNative));

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            targetBytes,
            kPatchBytes,
            PAGE_EXECUTE_READWRITE,
            &oldProtect
        )) {
        strcpy_s(g_error, "VirtualProtect failed while patching the native frame-submit function");
        return false;
    }

    std::memcpy(targetBytes, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), targetBytes, sizeof(patch));

    DWORD ignored = 0;
    VirtualProtect(targetBytes, kPatchBytes, oldProtect, &ignored);

    g_error[0] = '\0';
    return true;
}

DWORD WINAPI ProbeThread(void*) {
    BuildStatusPath();
    strcpy_s(g_error, "waiting for discord_voice.node");
    WriteStatus();

    HMODULE voice = nullptr;

    for (int i = 0; i < 3000; ++i) {
        voice = GetModuleHandleW(L"discord_voice.node");
        if (voice)
            break;
        Sleep(100);
    }

    if (!voice) {
        strcpy_s(g_error, "discord_voice.node was not loaded within 300 seconds");
        WriteStatus();
        return 0;
    }

    void* target = FindUniqueSignature(voice);
    if (!target) {
        WriteStatus();
        return 0;
    }

    if (!InstallInlineHook(target)) {
        WriteStatus();
        return 0;
    }

    g_hookInstalled.store(true, std::memory_order_release);
    WriteStatus();
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);

        HANDLE thread = CreateThread(
            nullptr,
            0,
            &ProbeThread,
            nullptr,
            0,
            nullptr
        );

        if (thread)
            CloseHandle(thread);
    }

    return TRUE;
}
