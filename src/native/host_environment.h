#pragma once

#include <string>

namespace stardew_uwp
{
bool SetWorkingDirectory(const std::wstring &path, unsigned long &error);
std::wstring GetEnvironmentValue(const std::wstring &name);
bool SetEnvironmentValue(const std::wstring &name, const std::wstring &value, unsigned long &error);
bool AttachOwnConsole(unsigned long &error);

// files that already match in size are skipped
int SyncFolder(const std::wstring &source, const std::wstring &target, std::string &error);
}
