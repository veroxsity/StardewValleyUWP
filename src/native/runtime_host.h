#pragma once

#include <string>
#include <vector>

namespace stardew_uwp
{
bool WriteLog(const std::wstring &path, const std::string &message);
int RunManagedApp(const std::wstring &runtimeRoot, const std::vector<std::wstring> &arguments, const std::wstring &logPath);
}
