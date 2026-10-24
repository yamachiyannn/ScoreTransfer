#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cwchar>
#include <cstdio>
#include <cstdlib>

namespace {
using InitializeByLoaderFn = bool (__cdecl *)(HMODULE);

void ShowFailure(const wchar_t* message, DWORD error = 0)
{
    wchar_t fullMessage[1024]{};
    if (error) {
        _snwprintf_s(fullMessage, _countof(fullMessage), _TRUNCATE,
            L"%s\nWin32 error: %lu", message, error);
    } else {
        _snwprintf_s(fullMessage, _countof(fullMessage), _TRUNCATE, L"%s", message);
    }
    MessageBoxW(nullptr, fullMessage, L"ScoreTransfer standalone loader", MB_OK | MB_ICONERROR);
}

DWORD WINAPI LoadScoreTransferThread(void* parameter)
{
    HMODULE self = static_cast<HMODULE>(parameter);
    wchar_t path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (!len || len >= MAX_PATH) {
        ShowFailure(L"Could not determine ScoreTransferBootstrap.dll path.", GetLastError());
        FreeLibraryAndExitThread(self, 1);
        return 1;
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        ShowFailure(L"The bootstrap DLL path has no directory separator.");
        FreeLibraryAndExitThread(self, 1);
        return 1;
    }
    *(slash + 1) = L'\0';
    if (wcscat_s(path, MAX_PATH, L"ScoreTransfer.dll") != 0) {
        ShowFailure(L"ScoreTransfer.dll path is too long.");
        FreeLibraryAndExitThread(self, 1);
        return 1;
    }

    HMODULE plugin = LoadLibraryW(path);
    if (!plugin) {
        ShowFailure(L"Could not load ScoreTransfer.dll beside the bootstrap DLL.", GetLastError());
        FreeLibraryAndExitThread(self, 1);
        return 1;
    }

    // An x86 MSVC C export may be present with a leading underscore. Try the plain
    // export first, then the decorated name; the normal SWRSToys exports are untouched.
    auto initialize = reinterpret_cast<InitializeByLoaderFn>(GetProcAddress(plugin, "InitializeByLoader"));
    if (!initialize)
        initialize = reinterpret_cast<InitializeByLoaderFn>(GetProcAddress(plugin, "_InitializeByLoader"));
    if (!initialize) {
        ShowFailure(L"InitializeByLoader export was not found in ScoreTransfer.dll.", GetLastError());
        FreeLibrary(plugin);
        FreeLibraryAndExitThread(self, 1);
        return 1;
    }

    if (!initialize(plugin)) {
        ShowFailure(L"ScoreTransfer initialization was refused. The game may be unsupported, or initialization failed.");
        FreeLibrary(plugin);
        FreeLibraryAndExitThread(self, 1);
        return 1;
    }

    OutputDebugStringW(L"[ScoreTransfer] Standalone bootstrap initialized ScoreTransfer.dll.\n");
    // ScoreTransfer.dll keeps its own loader reference. The temporary bootstrap can now unload.
    FreeLibraryAndExitThread(self, 0);
    return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, &LoadScoreTransferThread, instance, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
