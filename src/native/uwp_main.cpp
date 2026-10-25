#include "runtime_host.h"
#include "host_environment.h"
#include "launcher.h"
#include "text.h"

#include <Windows.h>
#include <SDL.h>

#include <fcntl.h>
#include <io.h>

#include <windows.foundation.h>
#include <windows.ui.core.h>
#include <wrl.h>
#include <wrl/event.h>

#include <string>
#include <vector>

namespace
{
// an app container drops stdio and freopen makes the CLR fatal error reporter fail fast
void RedirectStdio(const std::wstring &stdioPath, const std::wstring &logPath)
{
    CREATEFILE2_EXTENDED_PARAMETERS parameters{};
    parameters.dwSize = sizeof(parameters);
    parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;

    const HANDLE file = CreateFile2(stdioPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, &parameters);
    if (file == INVALID_HANDLE_VALUE)
    {
        stardew_uwp::WriteLog(logPath, "stdio open error=" + std::to_string(GetLastError()));
        return;
    }

    const BOOL errorHandle = SetStdHandle(STD_ERROR_HANDLE, file);
    const BOOL outputHandle = SetStdHandle(STD_OUTPUT_HANDLE, file);

    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_APPEND | _O_WRONLY);
    if (descriptor == -1)
    {
        stardew_uwp::WriteLog(logPath, "stdio osfhandle failed");
        return;
    }

    const int errorFd = _dup2(descriptor, 2);
    const int outputFd = _dup2(descriptor, 1);
    stardew_uwp::WriteLog(
        logPath,
        "stdio redirect handles=" + std::to_string(errorHandle) + std::to_string(outputHandle) +
            " fd=" + std::to_string(descriptor) + " stderr=" + std::to_string(errorFd) + " stdout=" + std::to_string(outputFd));
}

void ClaimBackButton(const std::wstring &logPath)
{
    Microsoft::WRL::ComPtr<ABI::Windows::UI::Core::ISystemNavigationManagerStatics> statics;
    HRESULT result = ::Windows::Foundation::GetActivationFactory(
        Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_UI_Core_SystemNavigationManager).Get(),
        statics.GetAddressOf());

    Microsoft::WRL::ComPtr<ABI::Windows::UI::Core::ISystemNavigationManager> manager;
    if (SUCCEEDED(result))
    {
        result = statics->GetForCurrentView(manager.GetAddressOf());
    }

    if (SUCCEEDED(result))
    {
        EventRegistrationToken token{};
        result = manager->add_BackRequested(
            Microsoft::WRL::Callback<ABI::Windows::Foundation::IEventHandler<ABI::Windows::UI::Core::BackRequestedEventArgs *>>(
                [](IInspectable *, ABI::Windows::UI::Core::IBackRequestedEventArgs *args) -> HRESULT
                {
                    if (args != nullptr)
                    {
                        args->put_Handled(TRUE);
                    }
                    return S_OK;
                })
                .Get(),
            &token);
    }

    stardew_uwp::WriteLog(logPath, std::string(SUCCEEDED(result) ? "back handled" : "back failed") + " code=" + std::to_string(result));
}
}

int main(int, char **)
{
    if (SDL_Init(0) != 0)
    {
        return 60;
    }

    char *basePathValue = SDL_GetBasePath();
    const char *localStateValue = SDL_WinRTGetFSPathUTF8(SDL_WINRT_PATH_LOCAL_FOLDER);
    if (basePathValue == nullptr || localStateValue == nullptr)
    {
        SDL_free(basePathValue);
        SDL_Quit();
        return 61;
    }

    const std::wstring packageRoot = stardew_uwp::Widen(basePathValue);
    const std::wstring localState = stardew_uwp::Widen(localStateValue);
    SDL_free(basePathValue);
    if (packageRoot.empty() || localState.empty())
    {
        SDL_Quit();
        return 62;
    }

    const std::wstring logPath = localState + L"\\stardew-host.log";
    stardew_uwp::WriteLog(logPath, "host start");

    unsigned long consoleError = 0;
    const bool console = stardew_uwp::AttachOwnConsole(consoleError);
    stardew_uwp::WriteLog(logPath, std::string("console ") + (console ? "attached" : "unavailable") + " error=" + std::to_string(consoleError));

    RedirectStdio(localState + L"\\stardew-stdio.log", logPath);

    // GetFolderPath for ApplicationData is empty in a container, so the working directory decides where the game writes
    unsigned long workingDirectoryError = 0;
    const bool workingDirectory = stardew_uwp::SetWorkingDirectory(localState, workingDirectoryError);
    stardew_uwp::WriteLog(
        logPath,
        std::string("workdir ") + (workingDirectory ? "ok" : "failed") + " error=" + std::to_string(workingDirectoryError));

    ClaimBackButton(logPath);

    stardew_uwp::RunLauncher(packageRoot, localState, logPath);

    const std::wstring gameRoot = stardew_uwp::ResolveGameRoot(packageRoot, localState);
    stardew_uwp::WriteLog(
        logPath,
        std::string("game root ") + (gameRoot == packageRoot ? "package" : "localstate") + " " + stardew_uwp::Narrow(gameRoot));

    // MonoMod needs windir to name a real directory, otherwise it picks a libc backend and dies
    const std::wstring windir = stardew_uwp::GetEnvironmentValue(L"windir");
    stardew_uwp::WriteLog(logPath, "windir was " + (windir.empty() ? std::string("unset") : stardew_uwp::Narrow(windir)));
    if (windir.empty() || GetFileAttributesW(windir.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        unsigned long windirError = 0;
        const bool set = stardew_uwp::SetEnvironmentValue(L"windir", localState, windirError);
        stardew_uwp::WriteLog(logPath, std::string("windir set ") + (set ? "ok" : "failed") + " error=" + std::to_string(windirError));
    }

    // SMAPI and the patched FNA ship in the package, and a depot install has neither
    if (gameRoot != packageRoot)
    {
        std::string overlayError;
        const int written = stardew_uwp::SyncFolder(packageRoot + L"overlay", gameRoot, overlayError);
        stardew_uwp::WriteLog(logPath, "overlay wrote=" + std::to_string(written) + (overlayError.empty() ? "" : " " + overlayError));

        // SMAPI runs on the game's own dependency list under its name, which is what its installer does on a pc
        const HRESULT deps = CopyFile2((gameRoot + L"Stardew Valley.deps.json").c_str(), (gameRoot + L"StardewModdingAPI.deps.json").c_str(), nullptr);
        stardew_uwp::WriteLog(logPath, "smapi deps " + std::string(SUCCEEDED(deps) ? "ok" : "failed") + " code=" + std::to_string(deps));
    }

    // the game needs Xbox Paths to list saves, and SMAPI's own Mods folder is read only
    std::string modsError;
    const int modsWritten = stardew_uwp::SyncFolder(packageRoot + L"bundled-mods", localState + L"\\Mods", modsError);
    stardew_uwp::WriteLog(logPath, "bundled mods wrote=" + std::to_string(modsWritten) + (modsError.empty() ? "" : " " + modsError));

    // SMAPI refuses to start outside the game folder and its own Mods folder would be read only
    std::vector<std::wstring> arguments;
    const std::wstring smapi = gameRoot + L"StardewModdingAPI.dll";
    if (GetFileAttributesW(smapi.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        arguments.push_back(smapi);
        arguments.push_back(L"--mods-path");
        arguments.push_back(localState + L"\\Mods");
        arguments.push_back(L"--no-terminal");
    }
    else
    {
        arguments.push_back(gameRoot + L"Stardew Valley.dll");
    }

    // hostfxr resolves hostpolicy against the root it is given, so that has to be the game folder
    const int result = stardew_uwp::RunManagedApp(gameRoot, arguments, logPath);
    SDL_Quit();
    return result;
}
