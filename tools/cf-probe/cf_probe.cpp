#include "curseforge.h"
#include "net/http.h"
#include "json.h"
#include "mods.h"
#include "setup_server.h"
#include "text.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

// desktop harness for the mods tab code that does not need a console
namespace
{
void Print(const std::string &message)
{
    printf("%s\n", message.c_str());
    // stdout to a pipe is fully buffered and serve never exits, so flush or nothing shows
    fflush(stdout);
}

int Usage()
{
    Print("usage:");
    Print("  stardew_cf_probe scan <modsRoot>");
    Print("  stardew_cf_probe zip <zipPath> <modsRoot>");
    Print("  stardew_cf_probe get <url>");
    Print("  stardew_cf_probe download <url> <destination>");
    Print("  stardew_cf_probe serve <stateFolder>");
    Print("  stardew_cf_probe search <stateFolder> [text]");
    Print("  stardew_cf_probe install <stateFolder> <modId>");
    return 64;
}

int Scan(const std::wstring &modsRoot)
{
    const std::vector<stardew_uwp::InstalledMod> mods = stardew_uwp::ScanMods(modsRoot);
    Print(std::to_string(mods.size()) + " mods");
    for (const stardew_uwp::InstalledMod &mod : mods)
    {
        Print("  " + mod.folder + "  " + mod.name + " " + mod.version + " [" + mod.uniqueId + "]");
    }
    return 0;
}

int Zip(const std::wstring &zipPath, const std::wstring &modsRoot)
{
    std::vector<std::string> folders;
    std::string error;
    if (!stardew_uwp::InstallModZip(zipPath, modsRoot, folders, error))
    {
        Print("failed: " + error);
        return 1;
    }
    for (const std::string &folder : folders)
    {
        Print("installed " + folder);
    }
    return Scan(modsRoot);
}

int Get(const std::string &url)
{
    std::string body;
    const stardew_uwp::HttpResult result = stardew_uwp::HttpGetString(url, {"Accept: application/json"}, body);
    Print("status " + std::to_string(result.status) + " bytes " + std::to_string(body.size()) + " " + result.error);
    if (body.empty())
    {
        return 1;
    }

    stardew_uwp::JsonValue value;
    std::string error;
    if (!stardew_uwp::JsonParse(body, value, error))
    {
        Print("json failed: " + error);
        Print(body.substr(0, 200));
        return 1;
    }
    Print("json parsed, " + std::to_string(value.Size()) + " members or items");
    return result.ok ? 0 : 1;
}

void ShowProgress(void *, uint64_t received, uint64_t total)
{
    static uint64_t last = 0;
    if (received - last < 1024 * 1024 && received != total)
    {
        return;
    }
    last = received;
    printf("  %llu of %llu\n", received, total);
}

int Download(const std::string &url, const std::wstring &destination)
{
    const stardew_uwp::HttpResult result = stardew_uwp::HttpDownloadFile(url, {}, destination, ShowProgress, nullptr, nullptr);
    Print("status " + std::to_string(result.status) + " " + (result.ok ? "ok" : result.error));
    return result.ok ? 0 : 1;
}

int Serve(const std::wstring &stateFolder)
{
    if (!stardew_uwp::SetupServerStart(stateFolder, 8099))
    {
        Print("failed: " + stardew_uwp::SetupServerError());
        return 1;
    }

    Print("open " + stardew_uwp::SetupServerUrl());
    while (!stardew_uwp::SetupServerSaved())
    {
        Sleep(200);
    }
    Print("saved a key into curseforge.json");
    stardew_uwp::SetupServerStop();
    return 0;
}

int WaitForIdle()
{
    std::string last;
    for (;;)
    {
        const stardew_uwp::CurseForgeStatus status = stardew_uwp::CurseForgeState();
        std::string line = status.state + " " + status.message;
        if (status.total != 0)
        {
            line += " " + std::to_string(status.transferred * 100 / status.total) + "%";
        }
        if (line != last)
        {
            Print(line);
            last = line;
        }
        if (status.state == "ready" || status.state == "failed")
        {
            return status.state == "ready" ? 0 : 1;
        }
        Sleep(200);
    }
}

bool Start(const std::wstring &stateFolder)
{
    if (!stardew_uwp::CurseForgeStart(stateFolder, Print))
    {
        Print("not configured: " + stardew_uwp::CurseForgeHint());
        return false;
    }

    for (int wait = 0; wait < 150 && !stardew_uwp::CurseForgeReady(); wait++)
    {
        if (stardew_uwp::CurseForgeState().state == "failed")
        {
            break;
        }
        Sleep(200);
    }
    if (!stardew_uwp::CurseForgeReady())
    {
        Print("not ready: " + stardew_uwp::CurseForgeState().message);
        stardew_uwp::CurseForgeStop();
        return false;
    }
    return true;
}
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3)
    {
        return Usage();
    }

    const std::wstring command = argv[1];
    if (command == L"scan")
    {
        return Scan(argv[2]);
    }
    if (command == L"zip")
    {
        return argc < 4 ? Usage() : Zip(argv[2], argv[3]);
    }
    if (command == L"get")
    {
        return Get(stardew_uwp::Narrow(argv[2]));
    }
    if (command == L"download")
    {
        return argc < 4 ? Usage() : Download(stardew_uwp::Narrow(argv[2]), argv[3]);
    }
    if (command == L"serve")
    {
        return Serve(argv[2]);
    }

    if (command == L"search")
    {
        if (!Start(argv[2]))
        {
            return 1;
        }
        stardew_uwp::CurseForgeSearch(argc > 3 ? stardew_uwp::Narrow(argv[3]) : std::string(), 0);
        const int result = WaitForIdle();

        std::vector<stardew_uwp::CatalogueMod> mods;
        int page = 0;
        bool hasMore = false;
        stardew_uwp::CurseForgeTakeResults(mods, page, hasMore);
        for (const stardew_uwp::CatalogueMod &mod : mods)
        {
            Print(std::to_string(mod.modId) + "  " + mod.name + " by " + mod.author +
                  (mod.distributable ? "" : "  [no third party downloads]"));
        }
        stardew_uwp::CurseForgeStop();
        return result;
    }

    if (command == L"install")
    {
        if (argc < 4 || !Start(argv[2]))
        {
            return 1;
        }
        stardew_uwp::CurseForgeInstall(_wtoi64(argv[3]));
        const int result = WaitForIdle();
        stardew_uwp::CurseForgeStop();
        Scan(std::wstring(argv[2]) + L"\\Mods");
        return result;
    }

    return Usage();
}
