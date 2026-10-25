#include "runtime_host.h"

#include <string>
#include <vector>

// the runtime root and app path are separate because the game no longer sits beside the runtime
int wmain(int argc, wchar_t **argv)
{
    if (argc < 3)
    {
        return 64;
    }

    const std::wstring runtimeRoot = argv[1];

    std::vector<std::wstring> arguments{argv[2]};
    for (int i = 3; i < argc; i += 1)
    {
        arguments.emplace_back(argv[i]);
    }

    return stardew_uwp::RunManagedApp(runtimeRoot, arguments, runtimeRoot + L"\\host.log");
}
