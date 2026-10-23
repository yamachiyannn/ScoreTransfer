#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>

#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kGameExe[] = L"th123.exe";
constexpr wchar_t kBootstrapDll[] = L"ScoreTransferBootstrap.dll";
constexpr wchar_t kPluginDll[] = L"ScoreTransfer.dll";

std::wstring GetLauncherDirectory()
{
    wchar_t path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!len || len >= MAX_PATH) return {};
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return {};
    *(slash + 1) = L'\0';
    return path;
}

std::vector<DWORD> FindGameProcesses()
{
    std::vector<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kGameExe) == 0)
                result.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool IsModuleLoaded(DWORD pid, const wchar_t* moduleName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool InjectBootstrap(DWORD pid, const std::wstring& dllPath, std::wstring& error)
{
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        error = L"OpenProcess failed (Win32 error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        error = L"VirtualAllocEx failed (Win32 error " + std::to_wstring(GetLastError()) + L").";
        CloseHandle(process);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes, &written) || written != bytes) {
        error = L"WriteProcessMemory failed (Win32 error " + std::to_wstring(GetLastError()) + L").";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibrary = kernel ? GetProcAddress(kernel, "LoadLibraryW") : nullptr;
    if (!loadLibrary) {
        error = L"Could not find LoadLibraryW in kernel32.dll.";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary), remotePath, 0, nullptr);
    if (!thread) {
        error = L"CreateRemoteThread failed (Win32 error " + std::to_wstring(GetLastError()) + L").";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 15000);
    if (wait != WAIT_OBJECT_0) {
        const DWORD waitError = GetLastError();
        error = wait == WAIT_TIMEOUT ? L"Timed out while loading ScoreTransferBootstrap.dll." :
            L"WaitForSingleObject failed (Win32 error " + std::to_wstring(waitError) + L").";
        // The remote thread may still read the path buffer after a timeout. Do not free
        // that buffer unless the thread has actually finished; the allocation dies with the game.
        CloseHandle(thread);
        CloseHandle(process);
        return false;
    }

    DWORD remoteModule = 0;
    const bool gotExitCode = GetExitCodeThread(thread, &remoteModule) != FALSE;
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    if (!gotExitCode || remoteModule == 0) {
        error = L"The game process could not load ScoreTransferBootstrap.dll. Check that the files are together and 32-bit.";
        return false;
    }
    return true;
}

void PauseBriefly()
{
    Sleep(5000);
}
}

int wmain()
{
    SetConsoleTitleW(L"ScoreTransfer standalone loader");
    std::wcout << L"ScoreTransfer standalone loader\n"
               << L"Start th123.exe first, then run this loader.\n\n";

    const std::wstring directory = GetLauncherDirectory();
    if (directory.empty()) {
        std::wcerr << L"Could not determine this loader's directory.\n";
        PauseBriefly();
        return 1;
    }

    const std::wstring bootstrapPath = directory + kBootstrapDll;
    const std::wstring pluginPath = directory + kPluginDll;
    if (GetFileAttributesW(bootstrapPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(pluginPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"Required files were not found beside this EXE.\n"
                   << L"Expected: ScoreTransferBootstrap.dll and ScoreTransfer.dll\n";
        PauseBriefly();
        return 2;
    }
    if (GetFileAttributesW((directory + L"bg.png").c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"Warning: bg.png is missing. The counters may appear without their frame.\n\n";
    }

    const std::vector<DWORD> pids = FindGameProcesses();
    if (pids.empty()) {
        std::wcerr << L"Could not find th123.exe. Start the game first, then run this EXE.\n";
        PauseBriefly();
        return 3;
    }
    if (pids.size() != 1) {
        std::wcerr << L"Found " << pids.size() << L" th123.exe processes. Close duplicate game instances and try again.\n";
        PauseBriefly();
        return 4;
    }

    const DWORD pid = pids.front();
    if (IsModuleLoaded(pid, kPluginDll)) {
        std::wcout << L"ScoreTransfer.dll is already loaded in th123.exe (PID " << pid << L").\n";
        PauseBriefly();
        return 0;
    }
    if (IsModuleLoaded(pid, kBootstrapDll)) {
        std::wcerr << L"ScoreTransferBootstrap.dll is already loaded. Wait a few seconds and check the game.\n";
        PauseBriefly();
        return 5;
    }

    std::wcout << L"Injecting bootstrap into th123.exe (PID " << pid << L")...\n";
    std::wstring error;
    if (!InjectBootstrap(pid, bootstrapPath, error)) {
        std::wcerr << L"Injection failed: " << error << L"\n";
        PauseBriefly();
        return 6;
    }

    std::wcout << L"Bootstrap loaded. Waiting for ScoreTransfer initialization...\n";
    for (int i = 0; i < 100; ++i) {
        if (IsModuleLoaded(pid, kPluginDll)) {
            std::wcout << L"ScoreTransfer.dll is loaded. If supported game version and DB are valid, the HUD should appear in supported scenes.\n";
            PauseBriefly();
            return 0;
        }
        if (!IsModuleLoaded(pid, kBootstrapDll)) break; // helper exits after initialization attempt
        Sleep(100);
    }

    std::wcerr << L"The helper did not leave ScoreTransfer.dll loaded. Check the game's version and OutputDebugString logs.\n";
    PauseBriefly();
    return 7;
}
