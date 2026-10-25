#pragma once

#include <string>
#include <vector>

namespace stardew_uwp
{
struct InstalledMod
{
    std::string folder;
    std::string name;
    std::string version;
    std::string uniqueId;
};

std::string ReadSmallFile(const std::wstring &path);

std::vector<InstalledMod> ScanMods(const std::wstring &modsRoot);

// writes only subtrees holding a manifest.json, so docs folders and mac resource forks stay out
bool InstallModZip(const std::wstring &zipPath, const std::wstring &modsRoot, std::vector<std::string> &folders, std::string &error);

// the name must be a plain child of the mods root so a crafted one cannot walk out of it
bool RemoveMod(const std::wstring &modsRoot, const std::string &folder, std::string &error);
}
