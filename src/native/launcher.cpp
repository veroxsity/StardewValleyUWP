#include "launcher.h"

#include "gpu_present.h"
#include "launcher_screens.h"
#include "launcher_ui.h"
#include "runtime_host.h"

#include "cf/curseforge.h"
#include "cf/image.h"
#include "cf/mods.h"
#include "cf/setup_server.h"

#include "steam/steam_cm.h"
#include "steam/steam_service.h"

#include <windows.h>
#include <windows.foundation.h>
#include <windows.gaming.input.h>
#include <windows.ui.core.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace stardew_uwp
{
namespace
{
constexpr unsigned short kSetupPort = 8099;
constexpr uint32_t kStardewAppId = 413150;
constexpr const wchar_t *kGameAssembly = L"Stardew Valley.dll";

std::wstring g_logPath;

void LauncherLog(const std::string &message)
{
    WriteLog(g_logPath, message);
}

void SteamLog(const char *message)
{
    if (message != nullptr)
    {
        WriteLog(g_logPath, std::string("steam ") + message);
    }
}

struct PadState
{
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool accept = false;
    bool cancel = false;
    bool pagePrevious = false;
    bool pageNext = false;
};

class Pad
{
public:
    void Init()
    {
        Microsoft::WRL::Wrappers::HStringReference name(RuntimeClass_Windows_Gaming_Input_Gamepad);
        ::Windows::Foundation::GetActivationFactory(name.Get(), statics_.GetAddressOf());
    }

    // edge triggered, a held stick or dpad should move one item not fifty
    PadState Poll()
    {
        const PadState now = Read();
        PadState pressed;
        pressed.up = now.up && !previous_.up;
        pressed.down = now.down && !previous_.down;
        pressed.left = now.left && !previous_.left;
        pressed.right = now.right && !previous_.right;
        pressed.accept = now.accept && !previous_.accept;
        pressed.cancel = now.cancel && !previous_.cancel;
        pressed.pagePrevious = now.pagePrevious && !previous_.pagePrevious;
        pressed.pageNext = now.pageNext && !previous_.pageNext;
        previous_ = now;
        return pressed;
    }

private:
    PadState Read()
    {
        using namespace ABI::Windows::Gaming::Input;

        PadState state;
        if (!statics_)
        {
            return state;
        }

        Microsoft::WRL::ComPtr<ABI::Windows::Foundation::Collections::IVectorView<Gamepad *>> pads;
        if (FAILED(statics_->get_Gamepads(pads.GetAddressOf())) || !pads)
        {
            return state;
        }

        unsigned int count = 0;
        if (FAILED(pads->get_Size(&count)) || count == 0)
        {
            return state;
        }

        Microsoft::WRL::ComPtr<IGamepad> pad;
        if (FAILED(pads->GetAt(0, pad.GetAddressOf())) || !pad)
        {
            return state;
        }

        GamepadReading reading = {};
        if (FAILED(pad->GetCurrentReading(&reading)))
        {
            return state;
        }

        const auto buttons = reading.Buttons;
        state.up = (buttons & GamepadButtons_DPadUp) != 0 || reading.LeftThumbstickY > 0.5;
        state.down = (buttons & GamepadButtons_DPadDown) != 0 || reading.LeftThumbstickY < -0.5;
        state.left = (buttons & GamepadButtons_DPadLeft) != 0 || reading.LeftThumbstickX < -0.5;
        state.right = (buttons & GamepadButtons_DPadRight) != 0 || reading.LeftThumbstickX > 0.5;
        state.accept = (buttons & GamepadButtons_A) != 0;
        state.cancel = (buttons & GamepadButtons_B) != 0;
        state.pagePrevious = (buttons & GamepadButtons_LeftShoulder) != 0;
        state.pageNext = (buttons & GamepadButtons_RightShoulder) != 0;
        return state;
    }

    Microsoft::WRL::ComPtr<ABI::Windows::Gaming::Input::IGamepadStatics> statics_;
    PadState previous_;
};

Microsoft::WRL::ComPtr<ABI::Windows::UI::Core::ICoreWindow> CurrentCoreWindow()
{
    using namespace ABI::Windows::UI::Core;

    Microsoft::WRL::ComPtr<ICoreWindowStatic> statics;
    Microsoft::WRL::ComPtr<ICoreWindow> window;
    if (SUCCEEDED(::Windows::Foundation::GetActivationFactory(
            Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_UI_Core_CoreWindow).Get(),
            statics.GetAddressOf())))
    {
        statics->GetForCurrentThread(window.GetAddressOf());
    }
    return window;
}

// without this the window stops responding and the shell kills the app for not drawing
void PumpDispatcher(const Microsoft::WRL::ComPtr<ABI::Windows::UI::Core::ICoreWindow> &window)
{
    using namespace ABI::Windows::UI::Core;

    Microsoft::WRL::ComPtr<ICoreDispatcher> dispatcher;
    if (window && SUCCEEDED(window->get_Dispatcher(dispatcher.GetAddressOf())) && dispatcher)
    {
        dispatcher->ProcessEvents(CoreProcessEventsOption_ProcessAllIfPresent);
    }
}

struct Shell
{
    Screen screen = Screen::Menu;
    int menuIndex = 0;
    int modsIndex = 0;
    int browseIndex = 0;
    int installedIndex = 0;
    int keyIndex = 0;

    std::wstring modsRoot;
    std::wstring packageRoot;
    std::wstring localState;
    std::vector<InstalledMod> installed;
    std::vector<CatalogueMod> catalogue;
    std::map<int64_t, Bitmap> thumbs;
    std::string query;
    int page = 0;
    bool hasMore = false;
    std::string lastState;
    std::string notice;
    bool linkRequested = false;
    bool gameInstalled = false;
};

// the game saves screenshots here once Xbox Paths has moved its data folder into LocalState
std::vector<std::wstring> FindScreenshots(const std::wstring &localState)
{
    const std::wstring folder = localState + L"\\StardewValley\\Screenshots\\";
    std::vector<std::wstring> found;
    WIN32_FIND_DATAW entry = {};
    const HANDLE search = FindFirstFileExW((folder + L"*.png").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE)
    {
        return found;
    }
    do
    {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            found.push_back(folder + entry.cFileName);
        }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    std::sort(found.begin(), found.end());
    return found;
}

void RefreshInstalled(Shell &shell)
{
    shell.installed = ScanMods(shell.modsRoot);
}

void HandleMenu(Shell &shell, const PadState &pressed, bool &running)
{
    if (pressed.up)
    {
        shell.menuIndex = (shell.menuIndex + kMenuItems - 1) % kMenuItems;
    }
    if (pressed.down)
    {
        shell.menuIndex = (shell.menuIndex + 1) % kMenuItems;
    }
    if (!pressed.accept)
    {
        return;
    }

    if (shell.menuIndex == 0)
    {
        if (!GameInstalled(shell.packageRoot, shell.localState))
        {
            shell.notice = "No game installed yet. Choose Download game to get it.";
            LauncherLog("launcher play refused, no game");
            return;
        }

        LauncherLog("launcher play");
        running = false;
    }
    else if (shell.menuIndex == 1)
    {
        RefreshInstalled(shell);
        shell.notice.clear();
        shell.screen = Screen::Mods;
    }
    else if (shell.menuIndex == 2)
    {
        shell.notice.clear();
        shell.screen = Screen::Steam;
        LauncherLog("launcher steam open");
    }
    else
    {
        steamSignOut();
        shell.linkRequested = false;
        shell.notice = "Signed out of Steam";
        LauncherLog("launcher steam signout");
    }
}

void HandleSteam(Shell &shell, const PadState &pressed, bool signedIn, bool downloading)
{
    if (pressed.cancel)
    {
        if (downloading)
        {
            steamCmCancelInstall(kStardewAppId);
            LauncherLog("launcher steam install cancelled");
            return;
        }

        shell.screen = Screen::Menu;
        return;
    }

    if (pressed.accept && signedIn && !downloading)
    {
        steamCmRequestInstallPlan(kStardewAppId);
        LauncherLog("launcher steam install requested");
    }
}

void HandleMods(Shell &shell, const PadState &pressed)
{
    if (pressed.cancel)
    {
        shell.screen = Screen::Menu;
        return;
    }

    // with no key the whole screen is one action, so A opens the phone flow rather than a list
    if (!CurseForgeConfigured())
    {
        if (pressed.accept)
        {
            SetupServerStart(shell.localState, kSetupPort);
            LauncherLog(SetupServerUrl().empty() ? ("launcher pairing failed " + SetupServerError()) : "launcher pairing open");
            shell.screen = Screen::Pairing;
        }
        return;
    }

    if (pressed.up)
    {
        shell.modsIndex = (shell.modsIndex + kModsItems - 1) % kModsItems;
    }
    if (pressed.down)
    {
        shell.modsIndex = (shell.modsIndex + 1) % kModsItems;
    }
    if (!pressed.accept)
    {
        return;
    }

    if (shell.modsIndex == 2)
    {
        RefreshInstalled(shell);
        shell.installedIndex = 0;
        shell.notice.clear();
        shell.screen = Screen::Installed;
        return;
    }

    if (!CurseForgeReady())
    {
        return;
    }

    if (shell.modsIndex == 0)
    {
        shell.keyIndex = 0;
        shell.screen = Screen::Keyboard;
        return;
    }

    shell.query.clear();
    shell.catalogue.clear();
    shell.browseIndex = 0;
    shell.page = 0;
    shell.screen = Screen::Browse;
    CurseForgeSearch(shell.query, 0);
}

void HandlePairing(Shell &shell, const PadState &pressed)
{
    // the key landing is what closes this screen, so the server is never left listening
    if (SetupServerSaved())
    {
        SetupServerStop();
        LauncherLog("launcher pairing saved");
        CurseForgeStop();
        CurseForgeStart(shell.localState, LauncherLog);
        shell.screen = Screen::Mods;
        return;
    }

    if (pressed.cancel)
    {
        SetupServerStop();
        shell.screen = Screen::Mods;
    }
}

void HandleInstalled(Shell &shell, const PadState &pressed)
{
    if (pressed.cancel)
    {
        shell.notice.clear();
        shell.screen = Screen::Mods;
        return;
    }

    const int count = static_cast<int>(shell.installed.size());
    if (count == 0)
    {
        return;
    }

    if (pressed.up)
    {
        shell.installedIndex = (shell.installedIndex + count - 1) % count;
    }
    if (pressed.down)
    {
        shell.installedIndex = (shell.installedIndex + 1) % count;
    }
    if (pressed.accept)
    {
        shell.screen = Screen::Confirm;
    }
}

void HandleConfirm(Shell &shell, const PadState &pressed)
{
    if (pressed.cancel)
    {
        shell.screen = Screen::Installed;
        return;
    }
    if (!pressed.accept || shell.installedIndex >= static_cast<int>(shell.installed.size()))
    {
        return;
    }

    const std::string folder = shell.installed[static_cast<std::size_t>(shell.installedIndex)].folder;
    std::string error;
    if (RemoveMod(shell.modsRoot, folder, error))
    {
        LauncherLog("launcher removed " + folder);
        shell.notice = "Deleted " + folder;
    }
    else
    {
        LauncherLog("launcher remove failed " + error);
        shell.notice = error;
    }

    RefreshInstalled(shell);
    if (shell.installedIndex >= static_cast<int>(shell.installed.size()))
    {
        shell.installedIndex = shell.installed.empty() ? 0 : static_cast<int>(shell.installed.size()) - 1;
    }
    shell.screen = Screen::Installed;
}

void HandleBrowse(Shell &shell, const PadState &pressed)
{
    const CurseForgeStatus status = CurseForgeState();
    const bool busy = status.state == "working";

    if (pressed.cancel)
    {
        // B cancels a download in flight, otherwise it is just back
        if (busy && status.total != 0)
        {
            CurseForgeCancel();
            LauncherLog("launcher install cancelled");
        }
        else
        {
            shell.screen = Screen::Mods;
        }
        return;
    }

    const int count = static_cast<int>(shell.catalogue.size());
    if (count != 0)
    {
        int column = shell.browseIndex % kCardColumns;
        int row = shell.browseIndex / kCardColumns;
        if (pressed.left && column > 0)
        {
            column--;
        }
        if (pressed.right && column + 1 < kCardColumns)
        {
            column++;
        }
        if (pressed.up && row > 0)
        {
            row--;
        }
        if (pressed.down && row + 1 < kCardRows)
        {
            row++;
        }
        const int moved = row * kCardColumns + column;
        shell.browseIndex = moved < count ? moved : count - 1;
    }

    if (busy)
    {
        return;
    }

    if (pressed.pageNext && shell.hasMore)
    {
        CurseForgeSearch(shell.query, shell.page + 1);
        return;
    }
    if (pressed.pagePrevious && shell.page > 0)
    {
        CurseForgeSearch(shell.query, shell.page - 1);
        return;
    }

    if (pressed.accept && shell.browseIndex < count)
    {
        const CatalogueMod &mod = shell.catalogue[static_cast<std::size_t>(shell.browseIndex)];
        CurseForgeInstall(mod.modId);
    }
}

void HandleKeyboard(Shell &shell, const PadState &pressed)
{
    if (pressed.cancel)
    {
        shell.screen = Screen::Mods;
        return;
    }

    int column = shell.keyIndex % kKeyColumns;
    int row = shell.keyIndex / kKeyColumns;
    if (pressed.left && column > 0)
    {
        column--;
    }
    if (pressed.right && column + 1 < kKeyColumns)
    {
        column++;
    }
    if (pressed.up && row > 0)
    {
        row--;
    }
    if (pressed.down && row + 1 < kKeyRows)
    {
        row++;
    }
    shell.keyIndex = row * kKeyColumns + column;

    if (!pressed.accept)
    {
        return;
    }

    switch (KeyActionAt(shell.keyIndex))
    {
    case KeyAction::Character:
        if (shell.query.size() < 48)
        {
            shell.query.push_back(kKeyboardRows[row][column]);
        }
        return;
    case KeyAction::Space:
        if (shell.query.size() < 48)
        {
            shell.query.push_back(' ');
        }
        return;
    case KeyAction::Delete:
        if (!shell.query.empty())
        {
            shell.query.pop_back();
        }
        return;
    case KeyAction::Search:
        shell.catalogue.clear();
        shell.browseIndex = 0;
        shell.page = 0;
        shell.screen = Screen::Browse;
        CurseForgeSearch(shell.query, 0);
        return;
    }
}
}

std::wstring ResolveGameRoot(const std::wstring &packageRoot, const std::wstring &localState)
{
    for (const std::wstring &candidate : {localState + L"\\game\\", localState + L"\\apps\\steam-413150\\"})
    {
        if (GetFileAttributesW((candidate + kGameAssembly).c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            return candidate;
        }
    }

    return packageRoot;
}

bool GameInstalled(const std::wstring &packageRoot, const std::wstring &localState)
{
    return GetFileAttributesW((ResolveGameRoot(packageRoot, localState) + kGameAssembly).c_str()) != INVALID_FILE_ATTRIBUTES;
}

void RunLauncher(const std::wstring &packageRoot, const std::wstring &localState, const std::wstring &logPath)
{
    g_logPath = logPath;

    Microsoft::WRL::ComPtr<ABI::Windows::UI::Core::ICoreWindow> window = CurrentCoreWindow();
    std::string gpuLog;
    Ui ui;
    if (!window || !GpuInit(window.Get(), gpuLog) || !ui.AttachSwapchain(GpuDevice(), GpuSwapchain(), gpuLog))
    {
        // without a surface the honest thing is to start the game rather than show nothing
        WriteLog(logPath, "launcher gpu failed " + (window ? gpuLog : std::string("no corewindow")));
        ui.Shutdown();
        GpuShutdown();
        return;
    }
    WriteLog(logPath, "launcher gpu ok " + std::to_string(static_cast<int>(ui.Width())) + "x" + std::to_string(static_cast<int>(ui.Height())));

    Shell shell;
    shell.packageRoot = packageRoot;
    shell.localState = localState;
    shell.modsRoot = localState + L"\\Mods";
    RefreshInstalled(shell);
    WriteLog(logPath, "launcher mods installed=" + std::to_string(shell.installed.size()));

    if (!CurseForgeStart(localState, LauncherLog))
    {
        WriteLog(logPath, "launcher curseforge off, " + CurseForgeHint());
    }

    steamInit(localState.c_str(), SteamLog, nullptr);

    Pad pad;
    pad.Init();

    LauncherView view;
    view.logoPath = packageRoot + L"Assets\\Square150x150Logo.png";
    view.backdrops.push_back(packageRoot + L"Assets\\Backdrop.jpg");
    const std::vector<std::wstring> screenshots = FindScreenshots(localState);
    view.backdrops.insert(view.backdrops.end(), screenshots.begin(), screenshots.end());
    WriteLog(logPath, "launcher screenshots=" + std::to_string(screenshots.size()));

    bool running = true;
    bool drawFailureLogged = false;

    while (running)
    {
        PumpDispatcher(window);
        const PadState pressed = pad.Poll();

        std::vector<CatalogueMod> fresh;
        int page = 0;
        bool hasMore = false;
        if (CurseForgeTakeResults(fresh, page, hasMore))
        {
            shell.catalogue = fresh;
            shell.page = page;
            shell.hasMore = hasMore;
            shell.browseIndex = 0;
            shell.screen = Screen::Browse;
            // a page at a time, so the cache never grows past what is on screen
            shell.thumbs.clear();
            ui.ForgetThumbnails();
            ThumbnailForget();
            for (const CatalogueMod &mod : shell.catalogue)
            {
                ThumbnailRequest(mod.modId, mod.thumbnail);
            }
        }

        std::vector<std::pair<int64_t, Bitmap>> decoded;
        ThumbnailTake(decoded);
        for (std::pair<int64_t, Bitmap> &thumb : decoded)
        {
            shell.thumbs[thumb.first] = std::move(thumb.second);
        }

        // an install that finished has written into the mods folder, so the list behind it is stale
        const CurseForgeStatus curseForge = CurseForgeState();
        if (curseForge.state != shell.lastState)
        {
            if (curseForge.state == "ready")
            {
                RefreshInstalled(shell);
            }
            shell.lastState = curseForge.state;
        }

        const SteamStatus steam = steamStatus();
        const SteamCmStatus cm = steamCmStatus();
        const bool signedIn = cm.state == SteamCmState::Online || steam.state == SteamState::Linked ||
                              steam.state == SteamState::Syncing;
        const bool linking = steam.state == SteamState::Starting || steam.state == SteamState::Linking;

        SteamInstallStatus install;
        steamCmGetInstallStatus(kStardewAppId, install);
        const bool downloading = install.state == "downloading" || install.state == "queued" ||
                                 install.state == "pending" || install.state == "verifying";

        // ask once per visit, a failure after the link lands would otherwise restart with a fresh QR
        if (shell.screen == Screen::Steam && !signedIn && !linking && !shell.linkRequested)
        {
            shell.linkRequested = steamBeginLink();
            WriteLog(logPath, std::string("launcher steam link ") + (shell.linkRequested ? "ok" : "failed"));
        }

        if (shell.screen == Screen::Menu)
        {
            shell.gameInstalled = GameInstalled(packageRoot, localState);
        }

        switch (shell.screen)
        {
        case Screen::Menu: HandleMenu(shell, pressed, running); break;
        case Screen::Mods: HandleMods(shell, pressed); break;
        case Screen::Pairing: HandlePairing(shell, pressed); break;
        case Screen::Browse: HandleBrowse(shell, pressed); break;
        case Screen::Installed: HandleInstalled(shell, pressed); break;
        case Screen::Confirm: HandleConfirm(shell, pressed); break;
        case Screen::Keyboard: HandleKeyboard(shell, pressed); break;
        case Screen::Steam: HandleSteam(shell, pressed, signedIn, downloading); break;
        }

        view.screen = shell.screen;
        view.menuIndex = shell.menuIndex;
        view.modsIndex = shell.modsIndex;
        view.browseIndex = shell.browseIndex;
        view.installedIndex = shell.installedIndex;
        view.keyIndex = shell.keyIndex;
        view.gameInstalled = shell.gameInstalled;
        view.notice = shell.notice;
        view.seconds = static_cast<double>(GetTickCount64()) / 1000.0;
        view.curseForgeConfigured = CurseForgeConfigured();
        view.curseForgeReady = CurseForgeReady();
        view.curseForgeHint = CurseForgeHint();
        view.curseForge = curseForge;
        view.installed = &shell.installed;
        view.catalogue = &shell.catalogue;
        view.thumbs = &shell.thumbs;
        view.query = shell.query;
        view.page = shell.page;
        view.hasMore = shell.hasMore;
        view.pairingUrl = SetupServerUrl();
        view.pairingError = SetupServerError();
        view.steam.signedIn = signedIn;
        view.steam.challengeUrl = steam.challengeUrl;
        view.steam.accountName = steam.accountName;
        view.steam.message = steam.message;
        view.steam.installState = install.state;
        view.steam.installMessage = install.message;
        view.steam.transferredBytes = install.transferredBytes;
        view.steam.totalBytes = install.totalBytes;
        view.steam.filesVerified = install.filesVerified;
        view.steam.totalFiles = install.totalFiles;
        view.steam.installedBytes = install.installedBytes;

        if (ui.Begin())
        {
            DrawLauncher(ui, view);
            if (!ui.End() && !drawFailureLogged)
            {
                WriteLog(logPath, "launcher draw failed");
                drawFailureLogged = true;
            }
        }
        GpuPresent();
        Sleep(16);
    }

    // sessions and sockets held by the workers must be gone before the game starts
    SetupServerStop();
    CurseForgeStop();
    ThumbnailStop();
    ui.Shutdown();
    GpuShutdown();
    WriteLog(logPath, "launcher handoff");
}
}
