#include "ScoreTransfer.hpp"
#include <Hash.hpp>
#include <cstring>

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16])
{
    // Follow SokuDev/ModTemplate: refuse unsupported game builds before patching fixed addresses.
    return hash && std::memcmp(hash, SokuLib::targetHash, sizeof(SokuLib::targetHash)) == 0;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE /*hParentModule*/)
{
    ScoreTransfer::g_module = hMyModule;
    ScoreTransfer::g_stopRequested.store(false, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, &ScoreTransfer::WorkerThreadEntry, nullptr, 0, nullptr);
    if (!thread) {
        OutputDebugStringW(L"[ScoreTransfer] Failed to create worker thread.\n");
        return false;
    }
    CloseHandle(thread);
    OutputDebugStringW(L"[ScoreTransfer] Initialized; waiting for the th123 D3D9 device.\n");
    return true;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Normal SWRSToys lifetime is process-wide. Do not try D3D/COM work under loader lock.
        ScoreTransfer::g_stopRequested.store(true, std::memory_order_release);
    }
    return TRUE;
}
