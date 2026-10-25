#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// blocking https through WinHTTP, shared by the steam and curseforge clients, desktop family because WinHTTP sits there
namespace stardew_uwp
{
struct HttpResult
{
    bool ok = false;
    int status = 0;
    std::string error;
};

using HttpProgress = void (*)(void *context, uint64_t received, uint64_t total);

// headers are whole lines without the line terminator
HttpResult HttpGetString(const std::string &url, const std::vector<std::string> &headers, std::string &body, std::size_t maxBytes = 8u * 1024u * 1024u);

// streams to disk so a 40 MB pack never sits in memory, cancel may be null
HttpResult HttpDownloadFile(
    const std::string &url,
    const std::vector<std::string> &headers,
    const std::wstring &destination,
    HttpProgress progress,
    void *context,
    const std::atomic<bool> *cancel);
}
