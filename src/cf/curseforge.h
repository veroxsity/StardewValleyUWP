#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace stardew_uwp
{
struct CatalogueMod
{
    int64_t modId = 0;
    std::string name;
    std::string author;
    std::string thumbnail;
    int64_t downloads = 0;
    bool distributable = true;
};

struct CurseForgeStatus
{
    std::string state;
    std::string message;
    uint64_t transferred = 0;
    uint64_t total = 0;
};

using CurseForgeLogger = void (*)(const std::string &message);

// the key is the player's own, read from LocalState\curseforge.json and never shipped in the package
bool CurseForgeStart(const std::wstring &localState, CurseForgeLogger logger);
void CurseForgeStop();

// false means the setup steps are worth showing rather than an error
bool CurseForgeConfigured();
bool CurseForgeReady();
std::string CurseForgeHint();

void CurseForgeSearch(const std::string &text, int page);
void CurseForgeInstall(int64_t modId);
void CurseForgeCancel();

CurseForgeStatus CurseForgeState();

// true once per new result set, so the launcher is not copying strings every frame
bool CurseForgeTakeResults(std::vector<CatalogueMod> &out, int &page, bool &hasMore);
}
