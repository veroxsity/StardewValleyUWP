#include "runtime_host.h"
#include "text.h"

#include <Windows.h>

#include <fstream>
#include <string>
#include <vector>

namespace
{
using HostfxrHandle = void *;

struct HostfxrInitializeParameters
{
    std::size_t size;
    const wchar_t *hostPath;
    const wchar_t *dotnetRoot;
};

using HostfxrInitializeForCommandLine = int(__cdecl *)(int, const wchar_t **, const HostfxrInitializeParameters *, HostfxrHandle *);
using HostfxrRunApp = int(__cdecl *)(HostfxrHandle);
using HostfxrClose = int(__cdecl *)(HostfxrHandle);
using HostfxrErrorWriter = void(__cdecl *)(const wchar_t *);
using HostfxrSetErrorWriter = HostfxrErrorWriter(__cdecl *)(HostfxrErrorWriter);

std::wstring activeLogPath;

void __cdecl WriteHostError(const wchar_t *message)
{
    if (message != nullptr)
    {
        stardew_uwp::WriteLog(activeLogPath, "hostfxr " + stardew_uwp::Narrow(message));
    }
}

template <typename Function>
Function Resolve(HMODULE module, const char *name, const std::wstring &logPath)
{
    const auto function = reinterpret_cast<Function>(GetProcAddress(module, name));
    if (function == nullptr)
    {
        stardew_uwp::WriteLog(logPath, std::string("host missing ") + name + " error=" + std::to_string(GetLastError()));
    }
    return function;
}

HMODULE LoadHostfxr(const std::wstring &runtimeRoot, const std::wstring &logPath)
{
#if defined(STARDEW_UWP_WINDOWS_STORE)
    HMODULE module = LoadPackagedLibrary(L"hostfxr.dll", 0);
#else
    HMODULE module = LoadLibraryW((runtimeRoot + L"\\hostfxr.dll").c_str());
#endif

    if (module == nullptr)
    {
        stardew_uwp::WriteLog(logPath, "host load_hostfxr error=" + std::to_string(GetLastError()));
    }
    return module;
}
}

namespace stardew_uwp
{
bool WriteLog(const std::wstring &path, const std::string &message)
{
    std::ofstream file(path, std::ios::app | std::ios::binary);
    if (!file)
    {
        return false;
    }

    file << message << '\n';
    return static_cast<bool>(file);
}

int RunManagedApp(const std::wstring &runtimeRoot, const std::vector<std::wstring> &arguments, const std::wstring &logPath)
{
    if (arguments.empty())
    {
        WriteLog(logPath, "host no app path");
        return -1;
    }

    const HMODULE hostfxr = LoadHostfxr(runtimeRoot, logPath);
    if (hostfxr == nullptr)
    {
        return static_cast<int>(GetLastError());
    }

    const auto initialize = Resolve<HostfxrInitializeForCommandLine>(hostfxr, "hostfxr_initialize_for_dotnet_command_line", logPath);
    const auto runApp = Resolve<HostfxrRunApp>(hostfxr, "hostfxr_run_app", logPath);
    const auto close = Resolve<HostfxrClose>(hostfxr, "hostfxr_close", logPath);
    const auto setErrorWriter = Resolve<HostfxrSetErrorWriter>(hostfxr, "hostfxr_set_error_writer", logPath);
    if (initialize == nullptr || runApp == nullptr || close == nullptr || setErrorWriter == nullptr)
    {
        FreeLibrary(hostfxr);
        return -1;
    }

    activeLogPath = logPath;
    setErrorWriter(WriteHostError);

    std::vector<const wchar_t *> argv;
    argv.reserve(arguments.size());
    std::string line;
    for (const std::wstring &argument : arguments)
    {
        argv.push_back(argument.c_str());
        line += (line.empty() ? "" : " ") + stardew_uwp::Narrow(argument);
    }
    WriteLog(logPath, "host app " + line);

    const HostfxrInitializeParameters parameters{sizeof(parameters), nullptr, runtimeRoot.c_str()};
    HostfxrHandle context = nullptr;
    const int initializeResult = initialize(static_cast<int>(argv.size()), argv.data(), &parameters, &context);
    WriteLog(logPath, "host initialize=" + std::to_string(initializeResult));
    if (initializeResult != 0 || context == nullptr)
    {
        if (context != nullptr)
        {
            close(context);
        }
        FreeLibrary(hostfxr);
        return initializeResult == 0 ? -1 : initializeResult;
    }

    WriteLog(logPath, "host run enter");
    const int exitCode = runApp(context);
    WriteLog(logPath, "host run exit=" + std::to_string(exitCode));
    close(context);
    FreeLibrary(hostfxr);
    return exitCode;
}
}
