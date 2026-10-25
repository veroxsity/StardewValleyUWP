#pragma once

#include "cf/curseforge.h"
#include "cf/image.h"
#include "cf/mods.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// plain data so the console and the pc preview draw through the same code
namespace stardew_uwp
{
class Ui;

enum class Screen
{
    Menu,
    Mods,
    Pairing,
    Browse,
    Installed,
    Confirm,
    Keyboard,
    Steam,
};

constexpr int kMenuItems = 4;
constexpr int kModsItems = 3;

// two across and four down, which is the page size the api is asked for
constexpr int kCardColumns = 2;
constexpr int kCardRows = 4;

constexpr int kKeyColumns = 10;
constexpr int kKeyRows = 5;
extern const char *const kKeyboardRows[4];

// the bottom row is three wide keys sharing ten cells, so navigation stays plain grid maths
enum class KeyAction
{
    Character,
    Space,
    Delete,
    Search,
};
KeyAction KeyActionAt(int index);

struct SteamView
{
    bool signedIn = false;
    std::string challengeUrl;
    std::string accountName;
    std::string message;
    std::string installState;
    std::string installMessage;
    uint64_t transferredBytes = 0;
    uint64_t totalBytes = 0;
    uint64_t filesVerified = 0;
    uint64_t totalFiles = 0;
    uint64_t installedBytes = 0;
};

struct LauncherView
{
    Screen screen = Screen::Menu;
    int menuIndex = 0;
    int modsIndex = 0;
    int browseIndex = 0;
    int installedIndex = 0;
    int keyIndex = 0;

    bool gameInstalled = false;
    std::string notice;

    // the packaged backdrop then the player's screenshots, drawn blurred behind the menu
    std::vector<std::wstring> backdrops;
    std::wstring logoPath;
    double seconds = 0.0;

    bool curseForgeConfigured = false;
    bool curseForgeReady = false;
    std::string curseForgeHint;
    CurseForgeStatus curseForge;

    const std::vector<InstalledMod> *installed = nullptr;
    const std::vector<CatalogueMod> *catalogue = nullptr;
    const std::map<int64_t, Bitmap> *thumbs = nullptr;
    std::string query;
    int page = 0;
    bool hasMore = false;

    std::string pairingUrl;
    std::string pairingError;

    SteamView steam;
};

// the mods the game needs to run on the console at all, so deleting one gets a louder warning
bool LoadBearing(const InstalledMod &mod);

void DrawLauncher(Ui &ui, const LauncherView &view);
}
