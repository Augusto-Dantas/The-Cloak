// overlay_renderer.h - DirectWrite + D2D text rendering on D3D11 texture
// Include this in dxgi_hook.cpp for full text overlay support

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
class OverlayRenderer {
public:
    bool Init(ID3D11Device* d3dDevice, UINT width, UINT height) {
        width_  = width;
        height_ = height;

        // 1. Get DXGI device from D3D11 device
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;

        // 2. Create D2D factory
        D2D1_FACTORY_OPTIONS opts{ D2D1_DEBUG_LEVEL_NONE };
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), &opts, (void**)&d2dFactory_))) return false;

        // 3. Create D2D device + context
        if (FAILED(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_))) return false;
        if (FAILED(d2dDevice_->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_))) return false;

        // 4. Create DirectWrite factory
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), (IUnknown**)&dwFactory_))) return false;

        // 5. Create text formats
        dwFactory_->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 16.f, L"en-us", &textFmt_);
        if (textFmt_) {
            textFmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        // 6. Create brushes
        d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 0.8f, 1.0f), &fgBrush_);
        d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f), &bgBrush_);

        // 7. Create overlay texture (BGRA + D2D-compatible flags)
        D3D11_TEXTURE2D_DESC texDesc{};
        texDesc.Width            = width;
        texDesc.Height           = height;
        texDesc.MipLevels        = 1;
        texDesc.ArraySize        = 1;
        texDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc       = { 1, 0 };
        texDesc.Usage            = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;  // needed for D2D interop

        if (FAILED(d3dDevice->CreateTexture2D(&texDesc, nullptr, &overlayTex_))) return false;

        // 8. Wrap texture as DXGI surface for D2D
        ComPtr<IDXGISurface> surface;
        if (FAILED(overlayTex_.As<IDXGISurface>(&surface))) return false;

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(
            surface.Get(), bmpProps, &d2dTarget_))) return false;

        initialized_ = true;
        return true;
    }

    // Call once per frame with current stats
    void Render(float fps, bool showFps, bool showTime,
                const char* customText, int posX, int posY)
    {
        if (!initialized_) return;

        d2dContext_->SetTarget(d2dTarget_.Get());
        d2dContext_->BeginDraw();
        d2dContext_->Clear(D2D1::ColorF(0, 0, 0, 0));  // transparent clear

        // Build overlay text
        std::wostringstream oss;
        if (customText && customText[0])
            oss << std::wstring(customText, customText + strlen(customText)) << L"\n";

        if (showFps)
            oss << L"FPS: " << std::fixed << std::setprecision(1) << fps << L"\n";

        if (showTime) {
            std::time_t t = std::time(nullptr);
            std::tm tm{}; localtime_s(&tm, &t);
            wchar_t tbuf[32];
            wcsftime(tbuf, 32, L"%H:%M:%S", &tm);
            oss << tbuf;
        }

        std::wstring text = oss.str();
        if (text.empty()) { d2dContext_->EndDraw(); return; }

        // Measure text
        ComPtr<IDWriteTextLayout> layout;
        dwFactory_->CreateTextLayout(text.c_str(), (UINT32)text.size(),
            textFmt_.Get(), 300.f, 120.f, &layout);
        DWRITE_TEXT_METRICS metrics{};
        if (layout) layout->GetMetrics(&metrics);

        float x = (float)posX, y = (float)posY;
        float pad = 6.f;

        // Background pill
        D2D1_ROUNDED_RECT bgRect = D2D1::RoundedRect(
            D2D1::RectF(x - pad, y - pad,
                        x + metrics.width + pad,
                        y + metrics.height + pad), 6.f, 6.f);
        d2dContext_->FillRoundedRectangle(bgRect, bgBrush_.Get());

        // Text
        d2dContext_->DrawTextLayout(D2D1::Point2F(x, y),
            layout.Get(), fgBrush_.Get());

        d2dContext_->EndDraw();
    }

    // Get the overlay texture to composite into the scene
    ID3D11Texture2D* GetTexture() { return overlayTex_.Get(); }
    bool IsInitialized() const    { return initialized_; }

private:
    ComPtr<ID2D1Factory1>       d2dFactory_;
    ComPtr<ID2D1Device>         d2dDevice_;
    ComPtr<ID2D1DeviceContext>  d2dContext_;
    ComPtr<ID2D1Bitmap1>        d2dTarget_;
    ComPtr<ID2D1SolidColorBrush> fgBrush_, bgBrush_;
    ComPtr<IDWriteFactory>      dwFactory_;
    ComPtr<IDWriteTextFormat>   textFmt_;
    ComPtr<ID3D11Texture2D>     overlayTex_;
    UINT width_ = 0, height_ = 0;
    bool initialized_ = false;
};
