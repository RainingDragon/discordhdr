#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    bool autoFind = false;
    DWORD pid = 0;
    std::wstring dllPath;
    int waitSeconds = 300;
};

std::wstring ToLower(std::wstring value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); }
    );
    return value;
}

std::wstring BaseName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool IsDiscordExe(const wchar_t* name) {
    if (!name) return false;
    const std::wstring lower = ToLower(name);
    return lower == L"discord.exe"
        || lower == L"discordcanary.exe"
        || lower == L"discordptb.exe"
        || lower == L"discorddevelopment.exe";
}

std::optional<std::uintptr_t> RemoteModuleBase(
    DWORD pid,
    const std::wstring& wantedModule
) {
    HANDLE snap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        pid
    );

    if (snap == INVALID_HANDLE_VALUE)
        return std::nullopt;

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);

    const std::wstring wantedLower = ToLower(wantedModule);

    if (Module32FirstW(snap, &module)) {
        do {
            const std::wstring current =
                ToLower(BaseName(module.szModule));

            if (current == wantedLower) {
                const auto base =
                    reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
                CloseHandle(snap);
                return base;
            }
        } while (Module32NextW(snap, &module));
    }

    CloseHandle(snap);
    return std::nullopt;
}

bool ProcessHasModule(DWORD pid, const std::wstring& moduleName) {
    return RemoteModuleBase(pid, moduleName).has_value();
}

std::optional<DWORD> FindDiscordVoiceProcess() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return std::nullopt;

    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);

    if (Process32FirstW(snap, &process)) {
        do {
            if (!IsDiscordExe(process.szExeFile))
                continue;

            if (ProcessHasModule(process.th32ProcessID, L"discord_voice.node")) {
                const DWORD pid = process.th32ProcessID;
                CloseHandle(snap);
                return pid;
            }
        } while (Process32NextW(snap, &process));
    }

    CloseHandle(snap);
    return std::nullopt;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool InjectDll(DWORD pid, const std::wstring& dllPath, std::wstring& error) {
    if (!FileExists(dllPath)) {
        error = L"DLL does not exist: " + dllPath;
        return false;
    }

    // If already loaded, don't add another LoadLibrary reference.
    if (ProcessHasModule(pid, BaseName(dllPath)))
        return true;

    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD
        | PROCESS_QUERY_INFORMATION
        | PROCESS_VM_OPERATION
        | PROCESS_VM_WRITE
        | PROCESS_VM_READ,
        FALSE,
        pid
    );

    if (!process) {
        error = L"OpenProcess failed: " + std::to_wstring(GetLastError());
        return false;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(
        process,
        nullptr,
        bytes,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );

    if (!remotePath) {
        error = L"VirtualAllocEx failed: " + std::to_wstring(GetLastError());
        CloseHandle(process);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(
            process,
            remotePath,
            dllPath.c_str(),
            bytes,
            &written
        ) || written != bytes) {
        error = L"WriteProcessMemory failed: " + std::to_wstring(GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    auto* localLoadLibrary =
        reinterpret_cast<std::uint8_t*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")
        );

    if (!localLoadLibrary) {
        error = L"Could not resolve local LoadLibraryW";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE localContainingModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(localLoadLibrary),
            &localContainingModule
        )) {
        error = L"GetModuleHandleExW failed for LoadLibraryW";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    wchar_t localModulePath[MAX_PATH]{};
    GetModuleFileNameW(
        localContainingModule,
        localModulePath,
        MAX_PATH
    );

    const std::wstring localModuleName = BaseName(localModulePath);
    const auto remoteModule =
        RemoteModuleBase(pid, localModuleName);

    if (!remoteModule) {
        error = L"Could not find remote " + localModuleName;
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const auto localBase =
        reinterpret_cast<std::uintptr_t>(localContainingModule);
    const auto localFunction =
        reinterpret_cast<std::uintptr_t>(localLoadLibrary);
    const auto offset = localFunction - localBase;

    auto remoteLoadLibrary =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            *remoteModule + offset
        );

    HANDLE thread = CreateRemoteThread(
        process,
        nullptr,
        0,
        remoteLoadLibrary,
        remotePath,
        0,
        nullptr
    );

    if (!thread) {
        error = L"CreateRemoteThread failed: " + std::to_wstring(GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);

    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);

    // Confirm by module enumeration rather than relying on the thread's
    // 32-bit exit code for a 64-bit module handle.
    for (int i = 0; i < 40; ++i) {
        if (ProcessHasModule(pid, BaseName(dllPath)))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    error = L"LoadLibrary thread finished but the probe DLL was not observed in the target process";
    return false;
}

void PrintUsage() {
    std::wcerr
        << L"DiscordHDRFix.Injector.exe --auto --dll <path> [--wait seconds]\n"
        << L"DiscordHDRFix.Injector.exe --pid <pid> --dll <path>\n";
}

std::optional<Options> ParseOptions(int argc, wchar_t** argv) {
    Options out;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];

        if (arg == L"--auto") {
            out.autoFind = true;
        } else if (arg == L"--pid" && i + 1 < argc) {
            out.pid = static_cast<DWORD>(_wtoi(argv[++i]));
        } else if (arg == L"--dll" && i + 1 < argc) {
            out.dllPath = argv[++i];
        } else if (arg == L"--wait" && i + 1 < argc) {
            out.waitSeconds = std::max(0, _wtoi(argv[++i]));
        } else {
            PrintUsage();
            return std::nullopt;
        }
    }

    if (out.dllPath.empty()) {
        PrintUsage();
        return std::nullopt;
    }

    if (!out.autoFind && out.pid == 0) {
        PrintUsage();
        return std::nullopt;
    }

    return out;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto options = ParseOptions(argc, argv);
    if (!options)
        return 2;

    DWORD targetPid = options->pid;

    if (options->autoFind) {
        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(options->waitSeconds);

        while (std::chrono::steady_clock::now() <= deadline) {
            const auto found = FindDiscordVoiceProcess();
            if (found) {
                targetPid = *found;
                break;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(250)
            );
        }
    }

    if (targetPid == 0) {
        std::wcerr
            << L"{\"ok\":false,\"error\":\"No Discord process with discord_voice.node loaded was found\"}\n";
        return 3;
    }

    std::wstring error;
    if (!InjectDll(targetPid, options->dllPath, error)) {
        std::wcerr
            << L"{\"ok\":false,\"pid\":" << targetPid
            << L",\"error\":\"" << error << L"\"}\n";
        return 4;
    }

    std::wcout
        << L"{\"ok\":true,\"pid\":" << targetPid
        << L",\"dll\":\"" << options->dllPath << L"\"}\n";

    return 0;
}
