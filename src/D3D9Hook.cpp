#include "ScoreTransfer.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace ScoreTransfer {
namespace {
static_assert(sizeof(void*) == 4, "ScoreTransfer must be built as Win32/x86 for th123.exe.");

constexpr uintptr_t kPreferredImageBase = 0x00400000;
constexpr uintptr_t RvaFromPreferredVa(uintptr_t preferredVa) { return preferredVa - kPreferredImageBase; }
constexpr uintptr_t kEndSceneCallRva = RvaFromPreferredVa(0x0040104C);
constexpr uintptr_t kResetPatchRva = RvaFromPreferredVa(0x004151A6);
constexpr uintptr_t kResetCallRva = RvaFromPreferredVa(0x004151AC);
constexpr uintptr_t kPresentParametersRva = RvaFromPreferredVa(0x008A0F68);
constexpr size_t kResetVtableIndex = 16;
constexpr size_t kEndSceneVtableIndex = 42;
constexpr size_t kDeviceVtableEntries = 119;
constexpr size_t kMaxPatchBytes = 16;
constexpr HRESULT kEndSceneHookReturnValue = static_cast<HRESULT>(0x008A0E14);

using ResetFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using EndSceneFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);

struct CallHookState {
    uintptr_t patchBase = 0;
    uintptr_t callSite = 0;
    size_t patchSize = 0;
    size_t callOffset = 0;
    uintptr_t originalTarget = 0;
    uintptr_t hookTarget = 0;
    bool injectedPattern = false;
    bool installed = false;
    uint8_t originalBytes[kMaxPatchBytes]{};
    uint8_t patchedBytes[kMaxPatchBytes]{};
};

std::atomic<EndSceneFunction> g_originalEndScene{nullptr};
std::atomic<ResetFunction> g_originalReset{nullptr};
CallHookState g_endSceneHook{};
CallHookState g_resetHook{};
volatile LONG g_installState = 0; // 0=not installed, 1=installing, 2=installed
volatile LONG g_hookMutationLock = 0;

class MutationGuard {
public:
    MutationGuard() { while (InterlockedCompareExchange(&g_hookMutationLock, 1, 0) != 0) Sleep(0); }
    ~MutationGuard() { InterlockedExchange(&g_hookMutationLock, 0); }
    MutationGuard(const MutationGuard&) = delete;
    MutationGuard& operator=(const MutationGuard&) = delete;
};

uintptr_t GameAddress(uintptr_t rva)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base ? base + rva : 0;
}

bool IsReadableAddress(const void* address, size_t size)
{
    if (!address || size == 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(info.BaseAddress);
    return begin >= regionBegin && size <= info.RegionSize && begin - regionBegin <= info.RegionSize - size;
}

bool IsExecutableAddress(const void* address)
{
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    switch (info.Protect & 0xFF) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

bool IsExecutableGameAddress(uintptr_t address)
{
    HMODULE exe = GetModuleHandleW(nullptr);
    MEMORY_BASIC_INFORMATION info{};
    return exe && address && VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) &&
        info.State == MEM_COMMIT && info.AllocationBase == exe && IsExecutableAddress(reinterpret_cast<void*>(address));
}

bool SafeReadMemory(uintptr_t address, void* output, size_t size)
{
    if (!address || !output || !size) return false;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
        output, size, &bytesRead) != FALSE && bytesRead == size;
}

bool ReadCallTarget(uintptr_t callSite, uintptr_t& target)
{
    uint8_t opcode = 0;
    int32_t displacement = 0;
    if (!SafeReadMemory(callSite, &opcode, sizeof(opcode)) || opcode != 0xE8 ||
        !SafeReadMemory(callSite + 1, &displacement, sizeof(displacement)))
        return false;
    const int64_t signedTarget = static_cast<int64_t>(callSite + 5) + displacement;
    if (signedTarget <= 0) return false;
    target = static_cast<uintptr_t>(signedTarget);
    return IsExecutableAddress(reinterpret_cast<void*>(target));
}

bool MakeRel32(uintptr_t callSite, uintptr_t target, int32_t& displacement)
{
    const int64_t value = static_cast<int64_t>(target) - static_cast<int64_t>(callSite + 5);
    if (value < INT32_MIN || value > INT32_MAX) return false;
    displacement = static_cast<int32_t>(value);
    return true;
}

bool WriteCodeBytes(uintptr_t address, const void* bytes, size_t size)
{
    if (!IsExecutableGameAddress(address) || !IsReadableAddress(reinterpret_cast<void*>(address), size))
        return false;
    DWORD oldProtection = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;
    std::memcpy(reinterpret_cast<void*>(address), bytes, size);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    DWORD ignored = 0;
    (void)VirtualProtect(reinterpret_cast<void*>(address), size, oldProtection, &ignored);
    return true;
}

bool SafeGetCreationParameters(IDirect3DDevice9* device, D3DDEVICE_CREATION_PARAMETERS* parameters)
{
    // Keep SEH in this POD-only function to avoid MSVC C2712.
    __try { return SUCCEEDED(device->GetCreationParameters(parameters)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ValidateDeviceAndReadVtable(IDirect3DDevice9* device, void** resetFallback, void** endSceneFallback)
{
    if (!device || !IsReadableAddress(device, sizeof(void*))) return false;
    void** vtable = nullptr;
    if (!SafeReadMemory(reinterpret_cast<uintptr_t>(device), &vtable, sizeof(vtable)) || !vtable ||
        !IsReadableAddress(vtable, sizeof(void*) * kDeviceVtableEntries))
        return false;
    void* reset = nullptr;
    void* endScene = nullptr;
    if (!SafeReadMemory(reinterpret_cast<uintptr_t>(vtable) + kResetVtableIndex * sizeof(void*), &reset, sizeof(reset)) ||
        !SafeReadMemory(reinterpret_cast<uintptr_t>(vtable) + kEndSceneVtableIndex * sizeof(void*), &endScene, sizeof(endScene)) ||
        !IsExecutableAddress(reset) || !IsExecutableAddress(endScene))
        return false;
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (!SafeGetCreationParameters(device, &creation)) return false;
    if (creation.hFocusWindow) {
        if (!IsWindow(creation.hFocusWindow)) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(creation.hFocusWindow, &pid);
        if (pid != GetCurrentProcessId()) return false;
    }
    if (resetFallback) *resetFallback = reset;
    if (endSceneFallback) *endSceneFallback = endScene;
    return true;
}

bool PrepareCallHook(CallHookState& state, uintptr_t patchBase, uintptr_t callSite,
    size_t patchSize, size_t callOffset, uintptr_t hookTarget, uintptr_t fallbackTarget,
    const uint8_t* injectionTemplate)
{
    if (patchSize > kMaxPatchBytes || patchSize < 5 || callOffset + 5 > patchSize ||
        !IsExecutableGameAddress(patchBase) || !IsReadableAddress(reinterpret_cast<void*>(patchBase), patchSize) ||
        !IsExecutableGameAddress(callSite))
        return false;

    state = {};
    state.patchBase = patchBase;
    state.callSite = callSite;
    state.patchSize = patchSize;
    state.callOffset = callOffset;
    state.hookTarget = hookTarget;
    if (!SafeReadMemory(patchBase, state.originalBytes, patchSize)) return false;

    uint8_t opcode = 0;
    if (!SafeReadMemory(callSite, &opcode, sizeof(opcode))) return false;
    if (opcode == 0xE8) {
        uintptr_t previousTarget = 0;
        if (!ReadCallTarget(callSite, previousTarget) || previousTarget == hookTarget)
            return false;
        state.patchBase = callSite;
        state.patchSize = 5;
        state.callOffset = 0;
        if (!SafeReadMemory(callSite, state.originalBytes, 5)) return false;
        std::memcpy(state.patchedBytes, state.originalBytes, 5);
        int32_t rel = 0;
        if (!MakeRel32(callSite, hookTarget, rel)) return false;
        std::memcpy(state.patchedBytes + 1, &rel, sizeof(rel));
        state.originalTarget = previousTarget;
        state.injectedPattern = false;
    } else {
        // This fallback is only for the known th123 build, after validating the live D3D9 vtable.
        if (!injectionTemplate || !IsExecutableAddress(reinterpret_cast<void*>(fallbackTarget)))
            return false;
        std::memcpy(state.patchedBytes, injectionTemplate, patchSize);
        int32_t rel = 0;
        if (!MakeRel32(callSite, hookTarget, rel)) return false;
        std::memcpy(state.patchedBytes + callOffset + 1, &rel, sizeof(rel));
        state.originalTarget = fallbackTarget;
        state.injectedPattern = true;
    }
    return true;
}

bool ApplyCallHook(CallHookState& state)
{
    if (!state.patchBase || !state.patchSize || !state.originalTarget) return false;
    // Refuse to overwrite bytes that changed after validation; another mod may have hooked first.
    uint8_t current[kMaxPatchBytes]{};
    if (!SafeReadMemory(state.patchBase, current, state.patchSize) ||
        std::memcmp(current, state.originalBytes, state.patchSize) != 0) {
        OutputDebugStringW(L"[ScoreTransfer] Hook site changed during install; refusing to overwrite it.\n");
        return false;
    }
    if (!WriteCodeBytes(state.patchBase, state.patchedBytes, state.patchSize)) return false;
    state.installed = true;
    return true;
}

void RollbackHook(CallHookState& state)
{
    if (!state.installed) return;
    uintptr_t currentTarget = 0;
    if (!ReadCallTarget(state.callSite, currentTarget) || currentTarget != state.hookTarget)
        return; // Someone chained after us. Never overwrite their hook.
    if (state.injectedPattern) {
        (void)WriteCodeBytes(state.patchBase, state.originalBytes, state.patchSize);
    } else {
        int32_t oldDisplacement = 0;
        std::memcpy(&oldDisplacement, state.originalBytes + 1, sizeof(oldDisplacement));
        (void)WriteCodeBytes(state.callSite + 1, &oldDisplacement, sizeof(oldDisplacement));
    }
    state.installed = false;
}

HRESULT STDMETHODCALLTYPE HookedEndScene(IDirect3DDevice9* device)
{
    RenderCounter(device);
    const auto next = g_originalEndScene.load(std::memory_order_acquire);
    if (next) (void)next(device);
    // th123's surrounding call-site sequence expects this value in EAX, as in OIOverlay/InGameHostlist.
    return kEndSceneHookReturnValue;
}

HRESULT STDMETHODCALLTYPE HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
{
    RendererBeforeReset(device);
    const auto next = g_originalReset.load(std::memory_order_acquire);
    const HRESULT result = next ? next(device, parameters) : D3DERR_INVALIDCALL;
    RendererAfterReset(device, result);
    return result;
}
} // namespace

bool TryInstallD3D9Hooks(IDirect3DDevice9* device)
{
    if (InterlockedCompareExchange(&g_installState, 0, 0) == 2) return true;
    if (InterlockedCompareExchange(&g_installState, 1, 0) != 0) return false;
    auto resetToIdle = []() { InterlockedExchange(&g_installState, 0); };
    MutationGuard guard;

    void* resetFallback = nullptr;
    void* endSceneFallback = nullptr;
    if (!ValidateDeviceAndReadVtable(device, &resetFallback, &endSceneFallback)) {
        resetToIdle();
        OutputDebugStringW(L"[ScoreTransfer] D3D9 device validation failed; hooks not installed.\n");
        return false;
    }

    const uintptr_t endSceneCall = GameAddress(kEndSceneCallRva);
    const uintptr_t resetCall = GameAddress(kResetCallRva);
    const uintptr_t resetPatch = GameAddress(kResetPatchRva);
    const uintptr_t presentParams = GameAddress(kPresentParametersRva);
    if (!endSceneCall || !resetCall || !resetPatch || presentParams > UINT32_MAX) {
        resetToIdle();
        return false;
    }

    const uint8_t endSceneTemplate[7] = { 0xE8, 0, 0, 0, 0, 0x50, 0x90 };
    uint8_t resetTemplate[13] = { 0x68, 0, 0, 0, 0, 0x50, 0xE8, 0, 0, 0, 0, 0x90, 0x90 };
    const uint32_t params32 = static_cast<uint32_t>(presentParams);
    std::memcpy(resetTemplate + 1, &params32, sizeof(params32));

    uint8_t resetOpcode = 0, endSceneOpcode = 0;
    if (!SafeReadMemory(resetCall, &resetOpcode, sizeof(resetOpcode)) ||
        !SafeReadMemory(endSceneCall, &endSceneOpcode, sizeof(endSceneOpcode))) {
        resetToIdle(); return false;
    }
    const bool resetAlreadyCall = resetOpcode == 0xE8;
    const bool endSceneAlreadyCall = endSceneOpcode == 0xE8;

    CallHookState resetCandidate{};
    CallHookState endSceneCandidate{};
    if (!PrepareCallHook(resetCandidate,
            resetAlreadyCall ? resetCall : resetPatch, resetCall,
            resetAlreadyCall ? 5 : 13, resetAlreadyCall ? 0 : 6,
            reinterpret_cast<uintptr_t>(&HookedReset), reinterpret_cast<uintptr_t>(resetFallback),
            resetAlreadyCall ? nullptr : resetTemplate)) {
        resetToIdle();
        OutputDebugStringW(L"[ScoreTransfer] Reset call-site signature check failed.\n");
        return false;
    }
    if (!PrepareCallHook(endSceneCandidate,
            endSceneCall, endSceneCall, endSceneAlreadyCall ? 5 : 7, 0,
            reinterpret_cast<uintptr_t>(&HookedEndScene), reinterpret_cast<uintptr_t>(endSceneFallback),
            endSceneAlreadyCall ? nullptr : endSceneTemplate)) {
        resetToIdle();
        OutputDebugStringW(L"[ScoreTransfer] EndScene call-site signature check failed.\n");
        return false;
    }

    g_originalReset.store(reinterpret_cast<ResetFunction>(resetCandidate.originalTarget), std::memory_order_release);
    g_originalEndScene.store(reinterpret_cast<EndSceneFunction>(endSceneCandidate.originalTarget), std::memory_order_release);
    if (!ApplyCallHook(resetCandidate)) {
        resetToIdle(); return false;
    }
    if (!ApplyCallHook(endSceneCandidate)) {
        RollbackHook(resetCandidate);
        resetToIdle(); return false;
    }
    g_resetHook = resetCandidate;
    g_endSceneHook = endSceneCandidate;
    InterlockedExchange(&g_installState, 2);
    OutputDebugStringW(L"[ScoreTransfer] EndScene/Reset hooks installed with call-chain support.\n");
    return true;
}
} // namespace ScoreTransfer
