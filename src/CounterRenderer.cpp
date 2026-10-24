#include "ScoreTransfer.hpp"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cwchar>
#include <objbase.h>
#include <wincodec.h>

namespace ScoreTransfer {
namespace {
ImGuiContext* g_context = nullptr;
IDirect3DDevice9* g_renderDevice = nullptr;
IDirect3DTexture9* g_backgroundTexture = nullptr; // D3DPOOL_MANAGED survives Reset.
ImFont* g_digitFont = nullptr;
bool g_resetInvalidated = false;

void SetContext()
{
    if (g_context) ImGui::SetCurrentContext(g_context);
}

bool GetBackgroundPath(wchar_t (&path)[MAX_PATH])
{
    if (!g_module)
        return false;
    const DWORD length = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    wchar_t* separator = wcsrchr(path, L'\\');
    if (!separator)
        return false;
    *(separator + 1) = L'\0';
    const size_t directoryLength = wcslen(path);
    return wcscat_s(path + directoryLength, MAX_PATH - directoryLength, L"bg.png") == 0;
}

bool LoadBackgroundPixels(std::vector<unsigned char>& pixels)
{
    wchar_t path[MAX_PATH]{};
    if (!GetBackgroundPath(path)) {
        OutputDebugStringW(L"[ScoreTransfer] Could not resolve the DLL directory for bg.png.\n");
        return false;
    }

    // CoInitializeEx can return RPC_E_CHANGED_MODE when the game already initialized
    // COM on this thread. In that case COM is still initialized, so WIC remains usable;
    // we simply must not call CoUninitialize for that pre-existing initialization.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        OutputDebugStringW(L"[ScoreTransfer] COM initialization failed while loading bg.png.\n");
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool success = false;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr))
        hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, &frame);

    UINT width = 0, height = 0;
    if (SUCCEEDED(hr))
        hr = frame->GetSize(&width, &height);
    if (SUCCEEDED(hr) && (width != 32 || height != 32)) {
        OutputDebugStringW(L"[ScoreTransfer] bg.png must be exactly 32x32 pixels.\n");
        hr = E_INVALIDARG;
    }
    if (SUCCEEDED(hr))
        hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        pixels.resize(static_cast<size_t>(width) * height * 4);
        hr = converter->CopyPixels(nullptr, width * 4,
            static_cast<UINT>(pixels.size()), pixels.data());
        success = SUCCEEDED(hr);
        if (success) {
            // Keep the PNG's straight alpha channel all the way into D3DFMT_A8R8G8B8.
            // If someone accidentally replaces bg.png with an opaque export, refuse to
            // draw an opaque square over the match HUD instead of silently hiding it.
            bool hasTransparency = false;
            for (size_t i = 3; i < pixels.size(); i += 4) {
                if (pixels[i] != 255) {
                    hasTransparency = true;
                    break;
                }
            }
            if (!hasTransparency) {
                OutputDebugStringW(L"[ScoreTransfer] bg.png has no non-opaque pixels; refusing to draw an opaque background.\n");
                success = false;
                hr = E_INVALIDARG;
            }
        }
    }

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (uninitializeCom) CoUninitialize();

    if (!success) {
        pixels.clear();
        OutputDebugStringW(L"[ScoreTransfer] Failed to load bg.png beside ScoreTransfer.dll.\n");
    }
    return success;
}

bool CreateBackgroundTexture(IDirect3DDevice9* device)
{
    if (!device) return false;
    if (g_backgroundTexture) {
        g_backgroundTexture->Release();
        g_backgroundTexture = nullptr;
    }

    std::vector<unsigned char> pixels;
    if (!LoadBackgroundPixels(pixels))
        return false;

    IDirect3DTexture9* texture = nullptr;
    // D3DFMT_A8R8G8B8 uses BGRA byte order on Win32 little-endian systems, matching
    // WIC's GUID_WICPixelFormat32bppBGRA output. ImGui's DX9 backend enables SRCALPHA /
    // INVSRCALPHA blending when it submits this texture, so PNG alpha is actually applied.
    const HRESULT result = device->CreateTexture(32, 32, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr);
    if (FAILED(result) || !texture) return false;
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0)) || !locked.pBits) {
        texture->Release();
        return false;
    }
    for (unsigned int y = 0; y < 32; ++y) {
        auto* destination = static_cast<unsigned char*>(locked.pBits) + y * locked.Pitch;
        std::memcpy(destination, pixels.data() + y * 32 * 4, 32 * 4);
    }
    texture->UnlockRect(0);
    g_backgroundTexture = texture;
    return true;
}

bool InitializeRenderer(IDirect3DDevice9* device)
{
    if (!device) return false;
    if (g_context && g_renderDevice == device) return true;

    if (g_context) {
        SetContext();
        if (g_backgroundTexture) { g_backgroundTexture->Release(); g_backgroundTexture = nullptr; }
        ImGui_ImplDX9_Shutdown();
        ImGui::DestroyContext(g_context);
        g_context = nullptr;
        g_digitFont = nullptr;
    }

    IMGUI_CHECKVERSION();
    g_context = ImGui::CreateContext();
    if (!g_context) return false;
    ImGui::SetCurrentContext(g_context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags = 0;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    ImFontConfig config{};
    config.SizePixels = 20.0f;
    g_digitFont = io.Fonts->AddFontDefault(&config); // Embedded ImGui font; no Japanese/external font dependency.
    if (!g_digitFont || !ImGui_ImplDX9_Init(device)) {
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext(g_context);
        g_context = nullptr;
        g_digitFont = nullptr;
        return false;
    }
    g_renderDevice = device;
    if (!CreateBackgroundTexture(device)) {
        // Keep the renderer alive so the counters still render if the sidecar asset
        // is missing; the user-facing instructions require bg.png beside this DLL.
        OutputDebugStringW(L"[ScoreTransfer] Continuing without background texture; place bg.png beside ScoreTransfer.dll.\n");
    }
    OutputDebugStringW(L"[ScoreTransfer] Independent ImGui DX9 renderer initialized.\n");
    return true;
}

void DrawCenteredNumber(ImDrawList* drawList, int value, float centerX, float centerY, float scale)
{
    char buffer[32]{};
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%d", (value < 0) ? 0 : value);
    const float size = 16.0f * 1.3f * scale; // 1.3x the original rendered size.
    ImVec2 extent = g_digitFont->CalcTextSizeA(size, FLT_MAX, 0.0f, buffer);
    // Shrink only to stay inside the visible frame if unexpectedly large counts become many digits.
    if (extent.x > 23.0f * scale && extent.x > 0.0f) {
        const float adjustment = (23.0f * scale) / extent.x;
        extent = g_digitFont->CalcTextSizeA(size * adjustment, FLT_MAX, 0.0f, buffer);
        const ImVec2 pos(centerX - extent.x * 0.5f, centerY - extent.y * 0.5f);
        drawList->AddText(g_digitFont, size * adjustment, ImVec2(pos.x + 1.0f * scale, pos.y + 1.0f * scale),
            IM_COL32(0, 0, 0, 240), buffer);
        drawList->AddText(g_digitFont, size * adjustment, pos, IM_COL32(255, 255, 255, 255), buffer);
        return;
    }
    const ImVec2 pos(centerX - extent.x * 0.5f, centerY - extent.y * 0.5f);
    // One-pixel black drop shadow, followed by the original-style yellow/gold digits.
    drawList->AddText(g_digitFont, size, ImVec2(pos.x + 1.0f * scale, pos.y + 1.0f * scale),
        IM_COL32(0, 0, 0, 240), buffer);
    drawList->AddText(g_digitFont, size, pos, IM_COL32(255, 255, 255, 255), buffer);
}
} // namespace

void RenderCounter(IDirect3DDevice9* device)
{
    const int scene = g_sceneId.load(std::memory_order_acquire);
    if (!IsDisplayScene(scene) || !device)
        return;
    if (!InitializeRenderer(device))
        return;
    SetContext();

    D3DVIEWPORT9 viewport{};
    if (FAILED(device->GetViewport(&viewport)) || viewport.Width == 0 || viewport.Height == 0)
        return;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(viewport.Width), static_cast<float>(viewport.Height));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float scaleX = static_cast<float>(viewport.Width) / 640.0f;
    const float scaleY = static_cast<float>(viewport.Height) / 480.0f;
    if (g_backgroundTexture) {
        const ImTextureID texture = reinterpret_cast<ImTextureID>(g_backgroundTexture);
        draw->AddImage(texture, ImVec2(227.0f * scaleX, 3.0f * scaleY),
            ImVec2(259.0f * scaleX, 35.0f * scaleY));
        draw->AddImage(texture, ImVec2(373.0f * scaleX, 3.0f * scaleY),
            ImVec2(405.0f * scaleX, 35.0f * scaleY));
    }
    DrawCenteredNumber(draw, g_leftWins.load(std::memory_order_acquire),
        247.0f * scaleX, 23.0f * scaleY, scaleY);
    DrawCenteredNumber(draw, g_rightWins.load(std::memory_order_acquire),
        393.0f * scaleX, 23.0f * scaleY, scaleY);

    ImGui::Render();
    IDirect3DStateBlock9* stateBlock = nullptr;
    if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
        (void)stateBlock->Capture();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    if (stateBlock) {
        (void)stateBlock->Apply();
        stateBlock->Release();
    }
}

void RendererBeforeReset(IDirect3DDevice9* device)
{
    if (!g_context || device != g_renderDevice) return;
    SetContext();
    ImGui_ImplDX9_InvalidateDeviceObjects();
    g_resetInvalidated = true;
}

void RendererAfterReset(IDirect3DDevice9* device, HRESULT result)
{
    if (!g_context || device != g_renderDevice || !g_resetInvalidated) return;
    SetContext();
    if (SUCCEEDED(result))
        (void)ImGui_ImplDX9_CreateDeviceObjects();
    g_resetInvalidated = false;
}

void RendererShutdown()
{
    if (!g_context) return;
    SetContext();
    if (g_backgroundTexture) { g_backgroundTexture->Release(); g_backgroundTexture = nullptr; }
    ImGui_ImplDX9_Shutdown();
    ImGui::DestroyContext(g_context);
    g_context = nullptr;
    g_renderDevice = nullptr;
    g_digitFont = nullptr;
}
} // namespace ScoreTransfer
