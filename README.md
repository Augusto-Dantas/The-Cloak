# DXGI Overlay Plane Injector

Renders a transparent, always-on-top overlay on **any DirectX application** (D3D11/D3D12)
by hooking `IDXGISwapChain::Present` and compositing onto a **DXGI hardware overlay plane**
(plane index 1) — the same mechanism used by NVIDIA GeForce Experience, MSI Afterburner,
Discord, Xbox Game Bar, and Steam.

```
┌─────────────────────────────────────────────────────┐
│  Target .exe  (game / app)                          │
│                                                     │
│   D3D11 / D3D12 swap chain                          │
│        │ Present()    ← hooked here                 │
│        ▼                                            │
│   [Hooked_Present]                                  │
│        │                                            │
│        ├──▶ DrawOverlay() via D2D / DirectWrite     │
│        │         │                                  │
│        │         ▼                                  │
│        │   IDXGISwapChain1 (plane=1, alpha=premult) │
│        │         │ Present()                        │
│        │         ▼                                  │
│        │   GPU Hardware Overlay Plane               │
│        │         │                                  │
│        ▼         ▼                                  │
│   Original Present() (plane 0)                      │
│                                                     │
│   ═══ Display Output ══════════════════════════     │
│   [ game frame ] + [ overlay ] composited by GPU    │
└─────────────────────────────────────────────────────┘
```

## Architecture

| Component | File | Role |
|---|---|---|
| **Payload DLL** | `dxgi_hook.dll` | Injected into target process; patches VMT |
| **Launcher** | `DXGIOverlayLauncher.exe` | Injects DLL, IPC config, log server |
| **Renderer** | `overlay_renderer.h` | D2D + DirectWrite text rendering |
| **Compositor** | `shaders.hlsl` | HLSL fullscreen quad for scene blend |

## How the Hook Works

1. **Bootstrap swap chain** — launcher creates a tiny hidden D3D11 device + swap chain.
2. **VMT patch** — `vtable[8]` (Present) is overwritten with `Hooked_Present`.
3. **Hook fires** — on every game frame, our code runs inside the game's GPU timeline.
4. **Overlay render** — DirectWrite draws FPS / time / custom text to a D2D-backed texture.
5. **Overlay plane** — if the GPU supports `IDXGIOutput2::SupportsOverlays()`, we create a
   second swap chain on **plane index 1** with `DXGI_ALPHA_MODE_PREMULTIPLIED`.
   The GPU compositor merges planes in hardware — zero CPU alpha-blend cost.
6. **Fallback** — GPUs without overlay plane support fall back to rendering on top of the
   regular back buffer.

## Requirements

- Windows 10 / 11 (x64)
- Visual Studio 2022 with "Desktop development with C++" workload
- Windows 10 SDK (10.0.19041+)
- CMake 3.20+
- Target application must use D3D11 or D3D12 (not OpenGL / Vulkan without DXGI wrapper)
- For overlay plane: GPU + driver with DXGI overlay plane support
  (most NVIDIA/AMD discrete GPUs, recent Intel Arc)

## Build

```powershell
# Clone and configure
git clone https://github.com/your-repo/dxgi-overlay
cd dxgi-overlay
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Outputs:
#   build/bin/dxgi_hook.dll
#   build/bin/DXGIOverlayLauncher.exe
```

## Usage

### Launch a new .exe with overlay
```
DXGIOverlayLauncher.exe dxgi_hook.dll "C:\Games\MyGame\game.exe"
```

### Attach to a running process
```
DXGIOverlayLauncher.exe        # interactive menu → option 2 or 3
```

### Interactive menu options
```
[1] Launch .exe and inject overlay
[2] Attach to running process by name (e.g. game.exe)
[3] Attach to running process by PID
[4] Configure overlay text
[5] Configure overlay color (R G B A)
[6] Toggle FPS counter
[7] Toggle clock
```

## IPC / Configuration

The launcher writes an `OverlayConfig` struct to a named shared memory section
`"DXGIOverlayCfg"`. The injected DLL reads this at startup. To update the overlay
at runtime, re-write the shared memory — the DLL polls it each frame.

Log messages from the DLL are piped back to the launcher via
`\\.\pipe\DXGIOverlayLog`.

## Extending

### Add custom rendering
Implement your draw calls inside `DrawOverlayContent()` in `dxgi_hook.cpp`.
The `OverlayRenderer` class in `overlay_renderer.h` provides a ready-made
D2D + DirectWrite pipeline. You have a full `ID3D11DeviceContext` available.

### D3D12 support
Hook `ID3D12CommandQueue::ExecuteCommandLists` and wrap with a D3D11on12 interop
layer to reuse the same D2D renderer. Replace `bootstrapDev` cast guard.

### Vulkan / OpenGL
Use a DXGI wrapper (DXVK / vkd3d) that exposes `IDXGISwapChain`, then the
same VMT hook applies transparently.

### Anti-cheat considerations
This uses `CreateRemoteThread` + `LoadLibraryA` injection which is detected by
most anti-cheat systems (EAC, BattlEye, VAC). Use only on single-player games,
your own executables, or in development environments.

## DXGI Overlay Plane — Technical Notes

Overlay planes are a GPU/OS feature exposed via `IDXGIOutput2::SupportsOverlays()`.
Hardware planes let the GPU compositor layer surfaces without a pixel-shader blend pass,
reducing latency and GPU overhead compared to traditional "render on top of backbuffer"
approaches.

```
DXGI_SWAP_CHAIN_DESC1 overlayDesc;
overlayDesc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;  // KEY
overlayDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
// Plane index is implicit: second swap chain on same HWND = plane 1
```

Plane support is exposed per-output, not per-adapter. Check:
```cpp
IDXGIOutput2::SupportsOverlays() → BOOL
IDXGIOutput3::CheckOverlaySupport(format, device, flags)  // more granular
IDXGIOutput6::CheckHardwareCompositionSupport(flags)       // Windows 10 1803+
```

