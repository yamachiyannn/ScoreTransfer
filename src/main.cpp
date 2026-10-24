#include "ScoreTransfer.hpp"
#include <Hash.hpp>
#include <Windows.h>
#include <wincrypt.h>
#include <cstring>
#include <cwchar>

namespace {
// One shared initialization path serves both SWRSToys and the standalone loader.
// 0 = not started, 1 = starting, 2 = started.
volatile LONG g_initializeState = 0;

bool ComputeCurrentExecutableMd5(BYTE output[16])
{
    if (!output)
        return false;

    wchar_t executablePath[MAX_PATH]{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    if (!pathLength || pathLength >= MAX_PATH)
        return false;

    const wchar_t* baseName = wcsrchr(executablePath, L'\\');
    baseName = baseName ? baseName + 1 : executablePath;
    if (_wcsicmp(baseName, L"th123.exe") != 0)
        return false;

    HANDLE file = CreateFileW(executablePath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool success = false;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_MD5, 0, 0, &hash)) {
        BYTE buffer[64 * 1024];
        DWORD bytesRead = 0;
        success = true;
        for (;;) {
            if (!ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr)) {
                success = false;
                break;
            }
            if (bytesRead == 0)
                break;
            if (!CryptHashData(hash, buffer, bytesRead, 0)) {
                success = false;
                break;
            }
        }
        DWORD digestLength = 16;
        if (success && (!CryptGetHashParam(hash, HP_HASHVAL, output, &digestLength, 0) || digestLength != 16))
            success = false;
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    return success;
}

bool IsSupportedExecutable()
{
    BYTE actualHash[16]{};
    if (!ComputeCurrentExecutableMd5(actualHash)) {
        OutputDebugStringW(L"[ScoreTransfer] Standalone loader could not read/hash th123.exe.\n");
        return false;
    }

    const bool matches = std::memcmp(actualHash, SokuLib::targetHash, sizeof(actualHash)) == 0;
    if (!matches)
        OutputDebugStringW(L"[ScoreTransfer] Unsupported th123.exe MD5. Refusing fixed-address hooks.\n");
    return matches;
}

bool InitializeCore(HMODULE hMyModule)
{
    // If two load paths race, wait for the first path or retry if it failed to start.
    // In normal usage only one path is selected, but this makes accidental overlap safe.
    for (;;) {
        const LONG previous = InterlockedCompareExchange(&g_initializeState, 1, 0);
        if (previous == 0)
            break;
        if (previous == 2)
            return true;

        bool retry = false;
        for (int attempt = 0; attempt < 500; ++attempt) {
            const LONG state = InterlockedCompareExchange(&g_initializeState, 0, 0);
            if (state == 2)
                return true;
            if (state == 0) {
                retry = true;
                break;
            }
            Sleep(10);
        }
        if (!retry) {
            OutputDebugStringW(L"[ScoreTransfer] Timed out waiting for another initialization path.\n");
            return false;
        }
    }

    ScoreTransfer::g_module = hMyModule;
    ScoreTransfer::g_stopRequested.store(false, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, &ScoreTransfer::WorkerThreadEntry, nullptr, 0, nullptr);
    if (!thread) {
        OutputDebugStringW(L"[ScoreTransfer] Failed to create worker thread.\n");
        InterlockedExchange(&g_initializeState, 0);
        return false;
    }
    CloseHandle(thread);
    InterlockedExchange(&g_initializeState, 2);
    OutputDebugStringW(L"[ScoreTransfer] Initialized; waiting for the th123 D3D9 device.\n");
    return true;
}
}

// Standard SWRSToys/SokuDev ModTemplate entry points. Keep these exports intact.
extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16])
{
    return hash && std::memcmp(hash, SokuLib::targetHash, sizeof(SokuLib::targetHash)) == 0;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE /*hParentModule*/)
{
    return InitializeCore(hMyModule);
}

// Additional entry point used only by ScoreTransfer-loader. This mode has no host
// calling CheckVersion first, so it validates the running game's on-disk MD5 itself.
extern "C" __declspec(dllexport) bool InitializeByLoader(HMODULE hMyModule)
{
    if (!IsSupportedExecutable())
        return false;
    return InitializeCore(hMyModule);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Do not attempt D3D/COM work under the loader lock.
        ScoreTransfer::g_stopRequested.store(true, std::memory_order_release);
    }
    return TRUE;
}
