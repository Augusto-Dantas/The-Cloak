// dxgi_hook.cpp - DXGI Overlay Plane Hook (injected DLL)
// Hooks IDXGISwapChain::Present to draw on the overlay plane
// Compatible with D3D11/D3D12 swap chains

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <string>
#include <memory>
#include <atomic>
#include <fstream>
#include <ctime>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
// MinHook-compatible trampoline typedef
// ─────────────────────────────────────────────────────────────────────────────
typedef HRESULT(WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

// ─────────────────────────────────────────────────────────────────────────────
// Global overlay state
// ─────────────────────────────────────────────────────────────────────────────
struct OverlayState {
    ComPtr<ID3D11Device>            device;
    ComPtr<ID3D11DeviceContext>     context;
    ComPtr<ID3D11RenderTargetView>  rtv;
    ComPtr<IDXGISwapChain1>         overlaySwapChain;   // plane index 1
    ComPtr<IDXGIOutput2>            output2;            // for overlay support query
    UINT                            width  = 0;
    UINT                            height = 0;
    bool                            initialized = false;
    bool                            overlayPlaneSupported = false;
    HWND                            targetHwnd = nullptr;
};

static OverlayState g_overlay;
static PFN_Present        g_origPresent       = nullptr;
static PFN_ResizeBuffers  g_origResizeBuffers = nullptr;
static std::atomic<bool>  g_insideHook{false};

// Config loaded from shared memory / IPC pipe written by launcher
struct OverlayConfig {
    char  overlayText[256]  = "DXGI Overlay Active";
    float r = 0.0f, g = 1.0f, b = 0.8f, a = 0.85f;  // tint color
    int   posX = 10, posY = 10;
    bool  showFps = true;
    bool  showTime = true;
};
static OverlayConfig g_cfg;

// ─────────────────────────────────────────────────────────────────────────────
// Logging
// ─────────────────────────────────────────────────────────────────────────────
static void Log(const char* fmt, ...) {
    char buf[512];
    va_list va; va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    // Write to named pipe that launcher reads
    HANDLE pipe = CreateFileA("\\\\.\\pipe\\DXGIOverlayLog",
        GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(pipe, buf, (DWORD)strlen(buf), &written, nullptr);
        CloseHandle(pipe);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Check overlay plane support (IDXGIOutput2::SupportsOverlays)
// ─────────────────────────────────────────────────────────────────────────────
static bool CheckOverlayPlaneSupport(IDXGISwapChain* pSwapChain) {
    ComPtr<IDXGIOutput> output;
    if (FAILED(pSwapChain->GetContainingOutput(&output))) return false;
    ComPtr<IDXGIOutput2> output2;
    if (FAILED(output.As(&output2))) return false;
    g_overlay.output2 = output2;
    return output2->SupportsOverlays() == TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw overlay using D3D11 (clear to semi-transparent color + text via GDI+)
// In a full implementation, you would use DirectWrite / D2D / custom shaders
// ─────────────────────────────────────────────────────────────────────────────
static void DrawOverlayContent(float fps) {
    if (!g_overlay.context || !g_overlay.rtv) return;

    // Bind overlay render target
    g_overlay.context->OMSetRenderTargets(1, g_overlay.rtv.GetAddressOf(), nullptr);

    // Clear with configured RGBA (alpha < 1 makes it semi-transparent)
    float clearColor[4] = { g_cfg.r * 0.05f, g_cfg.g * 0.05f, g_cfg.b * 0.05f, g_cfg.a * 0.6f };
    g_overlay.context->ClearRenderTargetView(g_overlay.rtv.Get(), clearColor);

    // TODO: Use DirectWrite to draw text (FPS counter, time, custom label)
    // Full implementation would create a D2D/DWrite pipeline and render text
    // here using the shared D3D11 device texture.
    (void)fps;
}

// ─────────────────────────────────────────────────────────────────────────────
// Initialize overlay swap chain on the DXGI overlay plane (plane index 1)
// ─────────────────────────────────────────────────────────────────────────────
static bool InitOverlayPlane(IDXGISwapChain* pSwapChain) {
    // Get the DXGI device from the swap chain
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IUnknown> deviceUnk;
    pSwapChain->GetDevice(__uuidof(IUnknown), (void**)deviceUnk.GetAddressOf());
    if (FAILED(deviceUnk.As(&dxgiDevice))) {
        Log("[Overlay] Could not get IDXGIDevice\n");
        return false;
    }

    // Get the D3D11 device
    if (FAILED(dxgiDevice.As<ID3D11Device>(&g_overlay.device))) {
        Log("[Overlay] Not a D3D11 device — D3D12 path not yet wired\n");
        return false;
    }
    g_overlay.device->GetImmediateContext(&g_overlay.context);

    // Query swap chain desc
    DXGI_SWAP_CHAIN_DESC desc{};
    pSwapChain->GetDesc(&desc);
    g_overlay.width  = desc.BufferDesc.Width;
    g_overlay.height = desc.BufferDesc.Height;
    g_overlay.targetHwnd = desc.OutputWindow;

    // Check overlay plane support
    g_overlay.overlayPlaneSupported = CheckOverlayPlaneSupport(pSwapChain);
    Log("[Overlay] Overlay plane supported: %s\n",
        g_overlay.overlayPlaneSupported ? "YES" : "NO (fallback to regular RT)");

    if (g_overlay.overlayPlaneSupported) {
        // Create overlay swap chain on plane index 1
        ComPtr<IDXGIFactory2> factory2;
        ComPtr<IDXGIAdapter> adapter;
        dxgiDevice->GetAdapter(&adapter);
        ComPtr<IDXGIFactory> factory;
        adapter->GetParent(__uuidof(IDXGIFactory), (void**)factory.GetAddressOf());
        factory.As<IDXGIFactory2>(&factory2);

        DXGI_SWAP_CHAIN_DESC1 overlayDesc{};
        overlayDesc.Width       = g_overlay.width;
        overlayDesc.Height      = g_overlay.height;
        overlayDesc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        overlayDesc.SampleDesc  = { 1, 0 };
        overlayDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        overlayDesc.BufferCount = 2;
        overlayDesc.Scaling     = DXGI_SCALING_STRETCH;
        overlayDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        overlayDesc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED; // key for transparency!
        overlayDesc.Flags       = 0;

        HRESULT hr = factory2->CreateSwapChainForHwnd(
            g_overlay.device.Get(),
            g_overlay.targetHwnd,
            &overlayDesc,
            nullptr, nullptr,
            g_overlay.overlaySwapChain.GetAddressOf());

        if (FAILED(hr)) {
            Log("[Overlay] CreateSwapChainForHwnd (overlay) FAILED: 0x%08X\n", hr);
            g_overlay.overlayPlaneSupported = false;
        } else {
            Log("[Overlay] Overlay swap chain created OK\n");
            // Set as overlay plane
            ComPtr<IDXGISwapChain2> sc2;
            if (SUCCEEDED(g_overlay.overlaySwapChain.As<IDXGISwapChain2>(&sc2))) {
                // MatrixTransform can be used to position overlay
                DXGI_MATRIX_3X2_F identity = {1,0, 0,1, 0,0};
                sc2->SetMatrixTransform(&identity);
            }
        }
    }

    // Create render target view (overlay plane or regular backbuffer fallback)
    IDXGISwapChain* rtSwapChain = g_overlay.overlayPlaneSupported
        ? g_overlay.overlaySwapChain.Get()
        : pSwapChain;

    ComPtr<ID3D11Texture2D> backbuffer;
    rtSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)backbuffer.GetAddressOf());
    g_overlay.device->CreateRenderTargetView(backbuffer.Get(), nullptr, &g_overlay.rtv);

    g_overlay.initialized = true;
    Log("[Overlay] Init complete — %ux%u\n", g_overlay.width, g_overlay.height);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Hooked IDXGISwapChain::Present
// ─────────────────────────────────────────────────────────────────────────────
static HRESULT WINAPI Hooked_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (g_insideHook.exchange(true)) {
        return g_origPresent(pSwapChain, SyncInterval, Flags);
    }

    if (!g_overlay.initialized) {
        InitOverlayPlane(pSwapChain);
    }

    static DWORD lastTick = GetTickCount();
    static int   frameCount = 0;
    static float fps = 0.f;
    frameCount++;
    DWORD now = GetTickCount();
    if (now - lastTick >= 1000) {
        fps = frameCount * 1000.f / float(now - lastTick);
        frameCount = 0;
        lastTick = now;
    }

    DrawOverlayContent(fps);

    // Present overlay plane first (if separate swap chain)
    if (g_overlay.overlayPlaneSupported && g_overlay.overlaySwapChain) {
        g_overlay.overlaySwapChain->Present(SyncInterval, Flags);
    }

    g_insideHook = false;
    return g_origPresent(pSwapChain, SyncInterval, Flags);
}

// ─────────────────────────────────────────────────────────────────────────────
// VMT Hook: patch the Present pointer in the vtable of the swap chain
// ─────────────────────────────────────────────────────────────────────────────
static void PatchVMT(IDXGISwapChain* pSwapChain) {
    void** vtable = *reinterpret_cast<void***>(pSwapChain);
    // vtable[8]  = Present
    // vtable[13] = ResizeBuffers

    DWORD oldProtect;
    VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    g_origPresent = reinterpret_cast<PFN_Present>(vtable[8]);
    vtable[8] = reinterpret_cast<void*>(Hooked_Present);
    VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);
    Log("[Overlay] VMT Present hook installed\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Bootstrap: create a hidden D3D11 device + swap chain, patch its VMT,
// then discard — the hook persists for all future swap chains in this process.
// ─────────────────────────────────────────────────────────────────────────────
static DWORD WINAPI OverlayThread(LPVOID) {
    // Load config from shared memory
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "DXGIOverlayCfg");
    if (hMap) {
        void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(OverlayConfig));
        if (pView) {
            memcpy(&g_cfg, pView, sizeof(OverlayConfig));
            UnmapViewOfFile(pView);
        }
        CloseHandle(hMap);
    }

    Log("[Overlay] DLL injected, creating bootstrap device...\n");

    // Create an off-screen window for bootstrap swap chain
    WNDCLASSEXA wc{};
    wc.cbSize      = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.lpszClassName = "DXGIOverlayBootstrap";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, "DXGIOverlayBootstrap", "",
        WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, nullptr, nullptr, nullptr, nullptr);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount       = 1;
    scd.BufferDesc.Width  = 800;
    scd.BufferDesc.Height = 600;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = hwnd;
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> bootstrapSC;
    ComPtr<ID3D11Device> bootstrapDev;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION,
        &scd, &bootstrapSC, &bootstrapDev, &fl, nullptr);

    if (FAILED(hr)) {
        Log("[Overlay] Bootstrap D3D11 creation FAILED: 0x%08X\n", hr);
        DestroyWindow(hwnd);
        return 1;
    }

    PatchVMT(bootstrapSC.Get());
    // Bootstrap device/SC released here — hook remains in vtable
    Log("[Overlay] Ready. Waiting for game to call Present...\n");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// DLL entry point
// ─────────────────────────────────────────────────────────────────────────────
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
