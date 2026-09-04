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
             bool keyFrame,
             std::int64_t renderTimeMs,
             void* frameHandle);

std::atomic<std::uint64_t> g_framesSeen{0};
std::atomic<std::uintptr_t> g_lastFrameHandle{0};
std::atomic<bool> g_hookInstalled{false};

EncoderAddFrameNativeFn g_originalAddFrameNative = nullptr;

wchar_t g_statusPath[MAX_PATH]{};
char g_importModule[128]{};
char g_error[256]{};

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

    const std::string importedFrom = EscapeJson(g_importModule);
    const std::string error = EscapeJson(g_error);

    char json[2048]{};
    _snprintf_s(
        json,
        sizeof(json),
        _TRUNCATE,
        "{\n"
        "  \"version\": \"0.5\",\n"
        "  \"pid\": %lu,\n"
        "  \"hook_installed\": %s,\n"
        "  \"symbol\": \"cc_encoder_add_frame_native\",\n"
        "  \"import_module\": \"%s\",\n"
        "  \"frames_seen\": %llu,\n"
        "  \"last_frame_handle\": \"0x%llx\",\n"
        "  \"mode\": \"pass-through\",\n"
        "  \"error\": \"%s\"\n"
        "}\n",
        GetCurrentProcessId(),
        g_hookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
        importedFrom.c_str(),
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

bool HookAddFrameNative(
    void* encoderHandle,
    const void* frameInfo,
    bool keyFrame,
    std::int64_t renderTimeMs,
    void* frameHandle
) {
    const auto count = g_framesSeen.fetch_add(1, std::memory_order_relaxed) + 1;
    g_lastFrameHandle.store(
        reinterpret_cast<std::uintptr_t>(frameHandle),
        std::memory_order_relaxed
    );

    // Roughly once per second at 60 fps. Diagnostic build only.
    if ((count % 60u) == 1u)
        WriteStatus();

    return g_originalAddFrameNative(
        encoderHandle,
        frameInfo,
        keyFrame,
        renderTimeMs,
        frameHandle
    );
}

bool PatchImport(
    HMODULE module,
    const char* wantedSymbol,
    void* replacement,
    void** originalOut
) {
    auto* base = reinterpret_cast<std::uint8_t*>(module);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        strcpy_s(g_error, "discord_voice.node has an invalid DOS header");
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        strcpy_s(g_error, "discord_voice.node has an invalid NT header");
        return false;
    }

    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        strcpy_s(g_error, "discord_voice.node is not a PE32+ x64 image");
        return false;
    }

    const auto& importDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
        strcpy_s(g_error, "discord_voice.node has no normal PE import directory");
        return false;
    }

    auto* descriptor =
        reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);

    for (; descriptor->Name != 0; ++descriptor) {
        const char* dllName =
            reinterpret_cast<const char*>(base + descriptor->Name);

        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            base + descriptor->FirstThunk
        );

        auto* names = descriptor->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA64*>(
                base + descriptor->OriginalFirstThunk
              )
            : nullptr;

        if (!names)
            continue;

        for (; names->u1.AddressOfData != 0; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;

            auto* importByName =
                reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + names->u1.AddressOfData
                );

            const char* name =
                reinterpret_cast<const char*>(importByName->Name);

            if (std::strcmp(name, wantedSymbol) != 0)
                continue;

            void* original =
                reinterpret_cast<void*>(
                    static_cast<std::uintptr_t>(iat->u1.Function)
                );

            // Publish the original target before the IAT swap. Another
            // encoder thread can enter the hook immediately after the swap.
            g_originalAddFrameNative =
                reinterpret_cast<EncoderAddFrameNativeFn>(original);
            *originalOut = original;

            DWORD oldProtect = 0;
            if (!VirtualProtect(
                    &iat->u1.Function,
                    sizeof(iat->u1.Function),
                    PAGE_READWRITE,
                    &oldProtect
                )) {
                strcpy_s(g_error, "VirtualProtect failed while patching IAT");
                return false;
            }

            InterlockedExchangePointer(
                reinterpret_cast<void* volatile*>(&iat->u1.Function),
                replacement
            );

            DWORD ignored = 0;
            VirtualProtect(
                &iat->u1.Function,
                sizeof(iat->u1.Function),
                oldProtect,
                &ignored
            );

            FlushInstructionCache(
                GetCurrentProcess(),
                &iat->u1.Function,
                sizeof(iat->u1.Function)
            );

            strncpy_s(g_importModule, dllName, _TRUNCATE);
            g_error[0] = '\0';
            return true;
        }
    }

    strcpy_s(
        g_error,
        "cc_encoder_add_frame_native was not found in discord_voice.node's normal import table"
    );
    return false;
}

DWORD WINAPI ProbeThread(void*) {
    BuildStatusPath();
    strcpy_s(g_error, "waiting for discord_voice.node");
    WriteStatus();

    HMODULE voice = nullptr;

    // Give Discord time to load the voice module / start a Go Live session.
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

    void* original = nullptr;
    if (!PatchImport(
            voice,
            "cc_encoder_add_frame_native",
            reinterpret_cast<void*>(&HookAddFrameNative),
            &original
        )) {
        WriteStatus();
        return 0;
    }

    // PatchImport publishes g_originalAddFrameNative before swapping
    // the IAT entry, so the hook cannot observe a null original pointer.
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
