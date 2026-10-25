#pragma once

#include <string>

struct ID3D11Device;
struct IDXGISwapChain1;

// GpuShutdown has to run before the game starts, a CoreWindow allows one swapchain
namespace stardew_uwp
{
bool GpuInit(void *coreWindow, std::string &log);
ID3D11Device *GpuDevice();
IDXGISwapChain1 *GpuSwapchain();
void GpuPresent();
void GpuShutdown();
}
