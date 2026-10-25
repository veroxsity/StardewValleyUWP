#pragma once

#include <string>

namespace stardew_uwp
{
// its swapchain is gone before this returns so SDL and FNA3D can take the window
void RunLauncher(const std::wstring &packageRoot, const std::wstring &localState, const std::wstring &logPath);

// the folder holding Stardew Valley.dll, with a trailing separator
std::wstring ResolveGameRoot(const std::wstring &packageRoot, const std::wstring &localState);
bool GameInstalled(const std::wstring &packageRoot, const std::wstring &localState);
}
