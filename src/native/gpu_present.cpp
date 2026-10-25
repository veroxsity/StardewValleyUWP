#include "gpu_present.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.foundation.h>
#include <windows.ui.core.h>
#include <wrl.h>

#include <cstdio>

namespace stardew_uwp
{
namespace
{
Microsoft::WRL::ComPtr<ID3D11Device> g_device;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_context;
Microsoft::WRL::ComPtr<IDXGISwapChain1> g_swapchain;

void LogResult(std::string &log, const char *what, HRESULT result)
{
    char buffer[96] = {};
    sprintf_s(buffer, sizeof(buffer), "%s hr=0x%08lx ", what, static_cast<unsigned long>(result));
    log += buffer;
}
}

bool GpuInit(void *coreWindow, std::string &log)
{
    GpuShutdown();

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL level = {};
    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
        D3D11_SDK_VERSION,
        &g_device,
        &level,
        &g_context);
    if (FAILED(result))
    {
        LogResult(log, "create_device", result);
        return false;
    }

    ABI::Windows::Foundation::Rect bounds = {};
    Microsoft::WRL::ComPtr<ABI::Windows::UI::Core::ICoreWindow> window;
    if (SUCCEEDED(Microsoft::WRL::ComPtr<IUnknown>(static_cast<IUnknown *>(coreWindow)).As(&window)) && window)
    {
        window->get_Bounds(&bounds);
    }
    UINT width = static_cast<UINT>(bounds.Width);
    UINT height = static_cast<UINT>(bounds.Height);
    if (width == 0 || height == 0)
    {
        width = 1280;
        height = 720;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    result = g_device.As(&dxgiDevice);
    if (SUCCEEDED(result))
    {
        result = dxgiDevice->GetAdapter(&adapter);
    }
    if (SUCCEEDED(result))
    {
        result = adapter->GetParent(IID_PPV_ARGS(&factory));
    }
    if (FAILED(result))
    {
        LogResult(log, "dxgi_factory", result);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 description = {};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SampleDesc.Count = 1;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    result = factory->CreateSwapChainForCoreWindow(g_device.Get(), static_cast<IUnknown *>(coreWindow), &description, nullptr, &g_swapchain);
    if (FAILED(result))
    {
        LogResult(log, "create_swapchain", result);
        return false;
    }

    return true;
}

ID3D11Device *GpuDevice()
{
    return g_device.Get();
}

IDXGISwapChain1 *GpuSwapchain()
{
    return g_swapchain.Get();
}

void GpuPresent()
{
    if (!g_swapchain)
    {
        return;
    }

    // vsync stalls the first presents for over a second on the Xbox compositor, so the launcher paces with Sleep
    g_swapchain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
}

void GpuShutdown()
{
    // FNA3D needs the one swapchain a CoreWindow allows, and bound pipeline state still holds references
    if (g_context)
    {
        g_context->ClearState();
        g_context->Flush();
    }

    g_swapchain.Reset();
    g_context.Reset();
    g_device.Reset();
}
}
