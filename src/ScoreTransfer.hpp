#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d9.h>
#include <atomic>
#include <string>

namespace ScoreTransfer {
struct DisplaySnapshot {
    int sceneId = -1;
    int leftWins = 0;
    int rightWins = 0;
};

extern std::atomic<int> g_sceneId;
extern std::atomic<int> g_leftWins;
extern std::atomic<int> g_rightWins;
extern std::atomic<bool> g_stopRequested;
extern HMODULE g_module;

bool IsDisplayScene(int sceneId);
DWORD WINAPI WorkerThreadEntry(void*);

// D3D9 call-site hooks. They are installed only after validating the live game device.
bool TryInstallD3D9Hooks(IDirect3DDevice9* device);
void RenderCounter(IDirect3DDevice9* device);
void RendererBeforeReset(IDirect3DDevice9* device);
void RendererAfterReset(IDirect3DDevice9* device, HRESULT result);
void RendererShutdown();
}
