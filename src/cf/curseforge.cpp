#include "curseforge.h"

#include "net/http.h"
#include "json.h"
#include "mods.h"
#include "text.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace stardew_uwp
{
namespace
{
constexpr const char *kApiRoot = "https://api.curseforge.com/v1";
// eight fits the card grid on a 1080 screen without the rows running into the status line
constexpr int kPageSize = 8;

struct Job
{
    enum class Kind
    {
        None,
        Search,
        Install,
    };

    Kind kind = Kind::None;
    std::string text;
    int page = 0;
    int64_t modId = 0;
};

struct State
{
    std::mutex lock;
    std::condition_variable wake;
    std::thread worker;

    std::string apiKey;
    std::wstring localState;
    CurseForgeLogger logger = nullptr;
    int64_t gameId = 0;
    std::string hint;
    bool configured = false;
    bool ready = false;
    bool stopping = false;
    std::atomic<bool> cancel{false};

    Job pending;
    bool hasPending = false;

    CurseForgeStatus status;
    std::vector<CatalogueMod> results;
    int resultPage = 0;
    bool resultHasMore = false;
    bool resultsFresh = false;
};

State g_state;

void Log(const std::string &message)
{
    if (g_state.logger != nullptr)
    {
        g_state.logger("curseforge " + message);
    }
}

void SetStatus(const std::string &state, const std::string &message)
{
    std::lock_guard<std::mutex> guard(g_state.lock);
    g_state.status.state = state;
    g_state.status.message = message;
    g_state.status.transferred = 0;
    g_state.status.total = 0;
}

std::string UrlEncode(const std::string &value)
{
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    for (const char raw : value)
    {
        const unsigned char ch = static_cast<unsigned char>(raw);
        const bool plain = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (plain)
        {
            out.push_back(static_cast<char>(ch));
            continue;
        }
        out.push_back('%');
        out.push_back(digits[ch >> 4]);
        out.push_back(digits[ch & 0x0F]);
    }
    return out;
}

bool GetJson(const std::string &url, JsonValue &out, std::string &error)
{
    const std::vector<std::string> headers{
        "Accept: application/json",
        "x-api-key: " + g_state.apiKey,
    };

    std::string body;
    const HttpResult result = HttpGetString(url, headers, body);
    if (!result.ok)
    {
        Log("request failed " + result.error);
        // the key is the one failure worth naming, everything else reads the same to a player
        if (result.status == 401 || result.status == 403)
        {
            error = "CurseForge turned the api key down";
        }
        else if (result.status == 0)
        {
            error = "Could not reach CurseForge, check the console is online";
        }
        else
        {
            error = "CurseForge had a problem, try again in a moment";
        }
        return false;
    }

    std::string parseError;
    if (!JsonParse(body, out, parseError))
    {
        Log("unreadable response " + parseError);
        error = "CurseForge sent back something unreadable";
        return false;
    }
    return true;
}

// the game id is looked up by slug once, curseforge.json can pin it if the lookup breaks
bool ResolveGameId(std::string &error)
{
    for (int index = 0; index < 5; index++)
    {
        JsonValue games;
        if (!GetJson(std::string(kApiRoot) + "/games?pageSize=50&index=" + std::to_string(index * 50), games, error))
        {
            return false;
        }

        const JsonValue *data = games.Find("data");
        if (data == nullptr || data->Size() == 0)
        {
            break;
        }

        for (std::size_t at = 0; at < data->Size(); at++)
        {
            const JsonValue *game = data->At(at);
            if (game != nullptr && game->StringField("slug") == "stardewvalley")
            {
                g_state.gameId = game->Int64Field("id");
                Log("game_id " + std::to_string(g_state.gameId));
                return true;
            }
        }
    }

    error = "Stardew Valley is not in the games this key can see";
    return false;
}

CatalogueMod ReadMod(const JsonValue &value)
{
    CatalogueMod mod;
    mod.modId = value.Int64Field("id");
    mod.name = value.StringField("name");
    mod.downloads = value.Int64Field("downloadCount");
    // absent means allowed, which is how every other client reads it
    mod.distributable = value.BoolField("allowModDistribution", true);

    const JsonValue *authors = value.Find("authors");
    const JsonValue *first = authors == nullptr ? nullptr : authors->At(0);
    if (first != nullptr)
    {
        mod.author = first->StringField("name");
    }

    const JsonValue *logo = value.Find("logo");
    if (logo != nullptr)
    {
        mod.thumbnail = logo->StringField("thumbnailUrl");
        if (mod.thumbnail.empty())
        {
            mod.thumbnail = logo->StringField("url");
        }
    }
    return mod;
}

void RunSearch(const Job &job)
{
    SetStatus("working", job.text.empty() ? "Loading popular mods" : ("Searching for " + job.text));

    std::string url = std::string(kApiRoot) + "/mods/search?gameId=" + std::to_string(g_state.gameId) +
                      "&pageSize=" + std::to_string(kPageSize) +
                      "&index=" + std::to_string(job.page * kPageSize) +
                      "&sortField=2&sortOrder=desc";
    if (!job.text.empty())
    {
        url += "&searchFilter=" + UrlEncode(job.text);
    }

    JsonValue response;
    std::string error;
    if (!GetJson(url, response, error))
    {
        Log("search failed " + error);
        SetStatus("failed", error);
        return;
    }

    std::vector<CatalogueMod> found;
    const JsonValue *data = response.Find("data");
    if (data != nullptr)
    {
        for (std::size_t at = 0; at < data->Size(); at++)
        {
            const JsonValue *entry = data->At(at);
            if (entry != nullptr)
            {
                found.push_back(ReadMod(*entry));
            }
        }
    }

    int64_t total = 0;
    const JsonValue *pagination = response.Find("pagination");
    if (pagination != nullptr)
    {
        total = pagination->Int64Field("totalCount");
    }

    {
        std::lock_guard<std::mutex> guard(g_state.lock);
        g_state.results = found;
        g_state.resultPage = job.page;
        g_state.resultHasMore = static_cast<int64_t>((job.page + 1) * kPageSize) < total;
        g_state.resultsFresh = true;
        g_state.status.state = "ready";
        g_state.status.message = found.empty() ? "Nothing matched" : std::to_string(total) + " mods";
    }
    Log("search page=" + std::to_string(job.page) + " got=" + std::to_string(found.size()) + " total=" + std::to_string(total));
}

void OnDownloadProgress(void *, uint64_t received, uint64_t total)
{
    std::lock_guard<std::mutex> guard(g_state.lock);
    g_state.status.transferred = received;
    g_state.status.total = total;
}

bool PickFile(int64_t modId, int64_t &fileId, std::string &fileName, std::string &downloadUrl, std::string &error)
{
    JsonValue response;
    if (!GetJson(std::string(kApiRoot) + "/mods/" + std::to_string(modId) + "/files?pageSize=20", response, error))
    {
        return false;
    }

    const JsonValue *data = response.Find("data");
    if (data == nullptr || data->Size() == 0)
    {
        error = "This mod has no files";
        return false;
    }

    const JsonValue *chosen = nullptr;
    for (std::size_t at = 0; at < data->Size(); at++)
    {
        const JsonValue *file = data->At(at);
        if (file == nullptr || !file->BoolField("isAvailable", true))
        {
            continue;
        }
        if (chosen == nullptr)
        {
            chosen = file;
        }
        // releaseType 1 is a full release, 2 beta, 3 alpha. the list comes back newest first
        if (file->Int64Field("releaseType") == 1)
        {
            chosen = file;
            break;
        }
    }

    if (chosen == nullptr)
    {
        error = "No file on this mod is available";
        return false;
    }

    fileId = chosen->Int64Field("id");
    fileName = chosen->StringField("fileName");
    downloadUrl = chosen->StringField("downloadUrl");
    if (!downloadUrl.empty())
    {
        return true;
    }

    // a null downloadUrl usually means the author opted out, but the search index also lags, so ask once more
    JsonValue direct;
    if (GetJson(std::string(kApiRoot) + "/mods/" + std::to_string(modId) + "/files/" + std::to_string(fileId) + "/download-url", direct, error))
    {
        const JsonValue *value = direct.Find("data");
        downloadUrl = value == nullptr ? std::string() : value->AsString();
    }
    if (downloadUrl.empty())
    {
        error = "The author blocks downloads from other apps, get this one on a PC";
        return false;
    }
    return true;
}

void RunInstall(const Job &job)
{
    SetStatus("working", "Reading the mod");

    JsonValue modResponse;
    std::string error;
    if (!GetJson(std::string(kApiRoot) + "/mods/" + std::to_string(job.modId), modResponse, error))
    {
        Log("install failed " + error);
        SetStatus("failed", error);
        return;
    }

    const JsonValue *modData = modResponse.Find("data");
    const CatalogueMod mod = modData == nullptr ? CatalogueMod() : ReadMod(*modData);
    if (!mod.distributable)
    {
        Log("install refused id=" + std::to_string(job.modId) + " distribution off");
        SetStatus("failed", "The author blocks downloads from other apps");
        return;
    }

    int64_t fileId = 0;
    std::string fileName;
    std::string downloadUrl;
    if (!PickFile(job.modId, fileId, fileName, downloadUrl, error))
    {
        Log("install failed id=" + std::to_string(job.modId) + " " + error);
        SetStatus("failed", error);
        return;
    }

    SetStatus("working", "Downloading " + (mod.name.empty() ? fileName : mod.name));
    Log("install id=" + std::to_string(job.modId) + " file=" + std::to_string(fileId) + " " + fileName);

    const std::wstring temporary = g_state.localState + L"\\cf-download.zip";
    const std::wstring modsRoot = g_state.localState + L"\\Mods";
    CreateDirectoryW(modsRoot.c_str(), nullptr);

    // the cdn wants the key since july 2026, and only forgecdn gets it so a stray downloadUrl never carries it
    std::vector<std::string> headers;
    if (downloadUrl.find(".forgecdn.net/") != std::string::npos)
    {
        headers.push_back("x-api-key: " + g_state.apiKey);
    }

    const HttpResult download = HttpDownloadFile(downloadUrl, headers, temporary, OnDownloadProgress, nullptr, &g_state.cancel);
    if (!download.ok)
    {
        Log("download failed status=" + std::to_string(download.status) + " " + download.error);
        std::string message = "The download failed, try again";
        if (download.error == "cancelled")
        {
            message = "Cancelled";
        }
        else if (download.status == 401)
        {
            message = "The CDN rejected the api key";
        }
        SetStatus("failed", message);
        return;
    }

    SetStatus("working", "Unpacking " + fileName);
    std::vector<std::string> folders;
    if (!InstallModZip(temporary, modsRoot, folders, error))
    {
        DeleteFileW(temporary.c_str());
        Log("unpack failed " + error);
        SetStatus("failed", "Could not unpack the mod, " + error);
        return;
    }
    DeleteFileW(temporary.c_str());

    std::string installed;
    for (const std::string &folder : folders)
    {
        installed += installed.empty() ? folder : (", " + folder);
    }
    Log("installed " + installed);
    SetStatus("ready", "Installed " + installed);
}

void Worker()
{
    std::string error;
    if (g_state.gameId == 0 && !ResolveGameId(error))
    {
        Log("start failed " + error);
        SetStatus("failed", error);
        std::lock_guard<std::mutex> guard(g_state.lock);
        g_state.hint = error;
        g_state.ready = false;
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_state.lock);
        g_state.ready = true;
    }

    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> guard(g_state.lock);
            g_state.wake.wait(guard, [] { return g_state.stopping || g_state.hasPending; });
            if (g_state.stopping)
            {
                return;
            }
            job = g_state.pending;
            g_state.hasPending = false;
        }

        g_state.cancel = false;
        if (job.kind == Job::Kind::Search)
        {
            RunSearch(job);
        }
        else if (job.kind == Job::Kind::Install)
        {
            RunInstall(job);
        }
    }
}
}

bool CurseForgeStart(const std::wstring &localState, CurseForgeLogger logger)
{
    // called again after the phone flow writes a key, so nothing may carry over from the last run
    CurseForgeStop();
    g_state.stopping = false;
    g_state.configured = false;
    g_state.ready = false;
    g_state.hint.clear();
    g_state.logger = logger;
    g_state.localState = localState;

    const std::string text = ReadSmallFile(localState + L"\\curseforge.json");
    if (text.empty())
    {
        g_state.hint = "No curseforge.json in LocalState";
        Log("no config");
        return false;
    }

    JsonValue config;
    std::string error;
    if (!JsonParse(text, config, error))
    {
        g_state.hint = "curseforge.json will not parse, " + error;
        Log("bad config " + error);
        return false;
    }

    g_state.apiKey = config.StringField("apiKey");
    g_state.gameId = config.Int64Field("gameId");
    if (g_state.apiKey.empty())
    {
        g_state.hint = "curseforge.json has no apiKey";
        Log("no api key");
        return false;
    }

    SetStatus("working", "Connecting to CurseForge");
    g_state.configured = true;
    g_state.worker = std::thread(Worker);
    return true;
}

void CurseForgeStop()
{
    if (!g_state.worker.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_state.lock);
        g_state.stopping = true;
    }
    g_state.cancel = true;
    g_state.wake.notify_all();
    g_state.worker.join();
}

bool CurseForgeConfigured()
{
    std::lock_guard<std::mutex> guard(g_state.lock);
    return g_state.configured;
}

bool CurseForgeReady()
{
    std::lock_guard<std::mutex> guard(g_state.lock);
    return g_state.ready;
}

std::string CurseForgeHint()
{
    std::lock_guard<std::mutex> guard(g_state.lock);
    return g_state.hint;
}

CurseForgeStatus CurseForgeState()
{
    std::lock_guard<std::mutex> guard(g_state.lock);
    return g_state.status;
}

void CurseForgeSearch(const std::string &text, int page)
{
    {
        std::lock_guard<std::mutex> guard(g_state.lock);
        g_state.pending = Job();
        g_state.pending.kind = Job::Kind::Search;
        g_state.pending.text = text;
        g_state.pending.page = page < 0 ? 0 : page;
        g_state.hasPending = true;
    }
    g_state.wake.notify_all();
}

void CurseForgeInstall(int64_t modId)
{
    {
        std::lock_guard<std::mutex> guard(g_state.lock);
        g_state.pending = Job();
        g_state.pending.kind = Job::Kind::Install;
        g_state.pending.modId = modId;
        g_state.hasPending = true;
    }
    g_state.wake.notify_all();
}

void CurseForgeCancel()
{
    g_state.cancel = true;
}

bool CurseForgeTakeResults(std::vector<CatalogueMod> &out, int &page, bool &hasMore)
{
    std::lock_guard<std::mutex> guard(g_state.lock);
    if (!g_state.resultsFresh)
    {
        return false;
    }

    out = g_state.results;
    page = g_state.resultPage;
    hasMore = g_state.resultHasMore;
    g_state.resultsFresh = false;
    return true;
}
}
