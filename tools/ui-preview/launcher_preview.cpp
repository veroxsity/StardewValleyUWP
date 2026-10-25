// renders every launcher screen to png on a pc, args are the out dir then an optional backdrop and logo

#include "native/launcher_screens.h"
#include "native/launcher_ui.h"

#include <windows.h>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace stardew_uwp;

namespace
{
std::vector<CatalogueMod> SampleCatalogue()
{
    return {
        {1, "Content Patcher", "Pathoschild", "", 21480312, true},
        {2, "Stardew Valley Expanded", "FlashShifter", "", 9871204, true},
        {3, "Lookup Anything", "Pathoschild", "", 8120554, true},
        {4, "Generic Mod Config Menu", "spacechase0", "", 7310298, true},
        {5, "CJB Cheats Menu", "CJBok", "", 6002137, true},
        {6, "NPC Map Locations", "Bouhm", "", 4120876, false},
        {7, "Automate", "Pathoschild", "", 3987001, true},
        {8, "Ridgeside Village", "Rafseazz", "", 2654310, true},
    };
}

std::vector<InstalledMod> SampleInstalled()
{
    return {
        {"CJBCheatsMenu", "CJB Cheats Menu", "1.42.0", "CJBok.CheatsMenu"},
        {"ConsoleCommands", "Console Commands", "4.5.2", "SMAPI.ConsoleCommands"},
        {"ContentPatcher", "Content Patcher", "2.9.1", "Pathoschild.ContentPatcher"},
        {"LookupAnything", "Lookup Anything", "1.49.2", "Pathoschild.LookupAnything"},
        {"SaveBackup", "Save Backup", "4.5.2", "SMAPI.SaveBackup"},
        {"TractorMod", "Tractor Mod", "4.24.4", "Pathoschild.TractorMod"},
        {"XboxPaths", "Xbox Paths", "1.1.0", "BanditVault.XboxPaths"},
        {"GenericModConfigMenu", "Generic Mod Config Menu", "1.14.1", "spacechase0.GenericModConfigMenu"},
        {"NPCMapLocations", "NPC Map Locations", "3.1.0", "Bouhm.NPCMapLocations"},
    };
}

struct Scene
{
    const char *name;
    std::function<void(LauncherView &)> setup;
};
}

int Render(int argc, wchar_t **argv);

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        fwprintf(stderr, L"usage: stardew_launcher_preview <out dir> [backdrop png] [logo png]\n");
        return 2;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // the renderer holds wic objects, so it has to be gone before CoUninitialize
    const int result = Render(argc, argv);
    CoUninitialize();
    return result;
}

int Render(int argc, wchar_t **argv)
{
    const std::wstring outDir = argv[1];
    const std::wstring backdrop = argc > 2 ? argv[2] : L"";
    const std::wstring logo = argc > 3 ? argv[3] : L"";
    CreateDirectoryW(outDir.c_str(), nullptr);

    Ui ui;
    std::string log;
    if (!ui.CreateOffscreen(1920, 1080, log))
    {
        fprintf(stderr, "offscreen failed: %s\n", log.c_str());
        return 1;
    }

    const std::vector<CatalogueMod> catalogue = SampleCatalogue();
    const std::vector<InstalledMod> installed = SampleInstalled();
    const std::vector<InstalledMod> none;
    const std::map<int64_t, Bitmap> thumbs;

    LauncherView base;
    base.gameInstalled = true;
    base.logoPath = logo;
    if (!backdrop.empty())
    {
        base.backdrops.push_back(backdrop);
    }
    base.curseForgeConfigured = true;
    base.curseForgeReady = true;
    base.curseForge.state = "ready";
    base.installed = &installed;
    base.catalogue = &catalogue;
    base.thumbs = &thumbs;
    base.steam.signedIn = true;
    base.steam.accountName = "farmer_dan";

    const std::string steamQr = "https://s.team/q/1/2786501734927788193";
    const std::string pairing = "http://192.168.5.236:8099/k/7f3c9a12";

    const std::vector<Scene> scenes = {
        {"menu", [](LauncherView &v) {}},
        {"menu-mods-focus", [](LauncherView &v) { v.menuIndex = 1; }},
        {"menu-no-game", [&](LauncherView &v) {
             v.gameInstalled = false;
             v.steam.signedIn = false;
             v.steam.accountName.clear();
             v.installed = &none;
             v.backdrops.clear();
             v.menuIndex = 2;
         }},
        {"menu-notice", [](LauncherView &v) {
             v.gameInstalled = false;
             v.steam.signedIn = false;
             v.steam.accountName.clear();
             v.backdrops.clear();
             v.notice = "No game installed yet. Choose Download game to get it.";
         }},
        {"steam-sign-in", [&](LauncherView &v) {
             v.screen = Screen::Steam;
             v.steam.signedIn = false;
             v.steam.accountName.clear();
             v.steam.challengeUrl = steamQr;
         }},
        {"steam-waiting", [](LauncherView &v) {
             v.screen = Screen::Steam;
             v.steam.signedIn = false;
             v.steam.accountName.clear();
         }},
        {"steam-ready", [](LauncherView &v) {
             v.screen = Screen::Steam;
             v.gameInstalled = false;
         }},
        {"steam-downloading", [](LauncherView &v) {
             v.screen = Screen::Steam;
             v.steam.installState = "downloading";
             v.steam.transferredBytes = 262144000ull;
             v.steam.totalBytes = 639212544ull;
             v.steam.filesVerified = 1204;
             v.steam.totalFiles = 3758;
         }},
        {"steam-installed", [](LauncherView &v) {
             v.screen = Screen::Steam;
             v.steam.installState = "installed";
             v.steam.installedBytes = 639212544ull;
         }},
        {"mods-setup", [](LauncherView &v) {
             v.screen = Screen::Mods;
             v.curseForgeConfigured = false;
             v.curseForgeReady = false;
         }},
        {"mods-search", [](LauncherView &v) { v.screen = Screen::Mods; }},
        {"mods-popular", [](LauncherView &v) {
             v.screen = Screen::Mods;
             v.modsIndex = 1;
         }},
        {"mods-installed", [](LauncherView &v) {
             v.screen = Screen::Mods;
             v.modsIndex = 2;
         }},
        {"mods-key-rejected", [](LauncherView &v) {
             v.screen = Screen::Mods;
             v.modsIndex = 1;
             v.curseForgeReady = false;
             v.curseForgeHint = "CurseForge turned the key down. Check it at console.curseforge.com.";
         }},
        {"browse-popular", [](LauncherView &v) {
             v.screen = Screen::Browse;
             v.browseIndex = 1;
         }},
        {"browse-search", [](LauncherView &v) {
             v.screen = Screen::Browse;
             v.query = "content patcher";
             v.browseIndex = 0;
         }},
        {"browse-installing", [](LauncherView &v) {
             v.screen = Screen::Browse;
             v.browseIndex = 2;
             v.curseForge.state = "working";
             v.curseForge.message = "Downloading Lookup Anything";
             v.curseForge.transferred = 412000;
             v.curseForge.total = 1048576;
         }},
        {"browse-blocked", [](LauncherView &v) {
             v.screen = Screen::Browse;
             v.browseIndex = 5;
             v.page = 2;
         }},
        {"keyboard", [](LauncherView &v) {
             v.screen = Screen::Keyboard;
             v.query = "lookup";
             v.keyIndex = 13;
         }},
        {"keyboard-search", [](LauncherView &v) {
             v.screen = Screen::Keyboard;
             v.query = "tractor mod";
             v.keyIndex = 48;
         }},
        {"installed", [](LauncherView &v) {
             v.screen = Screen::Installed;
             v.installedIndex = 2;
         }},
        {"installed-scrolled", [](LauncherView &v) {
             v.screen = Screen::Installed;
             v.installedIndex = 7;
             v.notice = "Deleted TractorMod";
         }},
        {"installed-empty", [&](LauncherView &v) {
             v.screen = Screen::Installed;
             v.installed = &none;
         }},
        {"confirm-delete", [](LauncherView &v) {
             v.screen = Screen::Confirm;
             v.installedIndex = 5;
         }},
        {"confirm-delete-needed", [](LauncherView &v) {
             v.screen = Screen::Confirm;
             v.installedIndex = 6;
         }},
        {"pairing", [&](LauncherView &v) {
             v.screen = Screen::Pairing;
             v.pairingUrl = pairing;
         }},
        {"pairing-error", [](LauncherView &v) {
             v.screen = Screen::Pairing;
             v.pairingError = "Port 8099 is already in use on this console.";
         }},
    };

    int failed = 0;
    for (const Scene &scene : scenes)
    {
        LauncherView view = base;
        scene.setup(view);
        const std::wstring path = outDir + L"\\" + std::wstring(scene.name, scene.name + strlen(scene.name)) + L".png";
        if (!ui.Begin())
        {
            return 1;
        }
        DrawLauncher(ui, view);
        if (!ui.End() || !ui.SavePng(path))
        {
            fprintf(stderr, "failed %s\n", scene.name);
            failed++;
        }
    }

    printf("rendered %d screens to %ls\n", static_cast<int>(scenes.size()) - failed, outDir.c_str());
    return failed == 0 ? 0 : 1;
}
