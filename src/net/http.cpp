#include "http.h"

#include "text.h"

#include <windows.h>
#include <winhttp.h>

namespace stardew_uwp
{
namespace
{
struct Handle
{
    HINTERNET value = nullptr;
    ~Handle()
    {
        if (value != nullptr)
        {
            WinHttpCloseHandle(value);
        }
    }
};

HttpResult Failure(const char *stage)
{
    HttpResult result;
    result.error = std::string(stage) + " win32=" + std::to_string(GetLastError());
    return result;
}

struct Request
{
    Handle session;
    Handle connect;
    Handle request;
    bool opened = false;
    std::string stage;

    bool Open(const std::string &url, const std::vector<std::string> &headers)
    {
        const std::wstring wide = Widen(url);
        URL_COMPONENTS parts = {};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {};
        wchar_t path[4096] = {};
        wchar_t extra[4096] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = 255;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 4095;
        parts.lpszExtraInfo = extra;
        parts.dwExtraInfoLength = 4095;
        if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts))
        {
            stage = "crack_url";
            return false;
        }

        session.value = WinHttpOpen(
            L"StardewValleyUWP/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (session.value == nullptr)
        {
            stage = "open";
            return false;
        }
        WinHttpSetTimeouts(session.value, 15000, 15000, 30000, 60000);

        connect.value = WinHttpConnect(session.value, host, parts.nPort, 0);
        if (connect.value == nullptr)
        {
            stage = "connect";
            return false;
        }

        // WinHttpCrackUrl returns the query separately, and dropping it turns every search into an unfiltered one
        const std::wstring target = std::wstring(path) + extra;
        request.value = WinHttpOpenRequest(
            connect.value,
            L"GET",
            target.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (request.value == nullptr)
        {
            stage = "open_request";
            return false;
        }

        std::wstring headerBlock;
        for (const std::string &header : headers)
        {
            headerBlock += Widen(header);
            headerBlock += L"\r\n";
        }

        if (!WinHttpSendRequest(
                request.value,
                headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                headerBlock.empty() ? 0 : static_cast<DWORD>(headerBlock.size()),
                WINHTTP_NO_REQUEST_DATA,
                0,
                0,
                0))
        {
            stage = "send";
            return false;
        }
        if (!WinHttpReceiveResponse(request.value, nullptr))
        {
            stage = "receive";
            return false;
        }

        opened = true;
        return true;
    }

    int Status() const
    {
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(
            request.value,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &size,
            WINHTTP_NO_HEADER_INDEX);
        return static_cast<int>(status);
    }

    uint64_t ContentLength() const
    {
        wchar_t buffer[64] = {};
        DWORD size = sizeof(buffer);
        if (!WinHttpQueryHeaders(
                request.value,
                WINHTTP_QUERY_CONTENT_LENGTH,
                WINHTTP_HEADER_NAME_BY_INDEX,
                buffer,
                &size,
                WINHTTP_NO_HEADER_INDEX))
        {
            return 0;
        }
        return static_cast<uint64_t>(_wcstoui64(buffer, nullptr, 10));
    }
};
}

HttpResult HttpGetString(const std::string &url, const std::vector<std::string> &headers, std::string &body, std::size_t maxBytes)
{
    body.clear();

    Request request;
    if (!request.Open(url, headers))
    {
        return Failure(request.stage.c_str());
    }

    HttpResult result;
    result.status = request.Status();

    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.request.value, &available))
        {
            return Failure("query_available");
        }
        if (available == 0)
        {
            break;
        }
        if (body.size() + available > maxBytes)
        {
            result.error = "response over " + std::to_string(maxBytes) + " bytes";
            return result;
        }

        const std::size_t at = body.size();
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.request.value, body.data() + at, available, &read))
        {
            return Failure("read");
        }
        body.resize(at + read);
        if (read == 0)
        {
            break;
        }
    }

    result.ok = result.status == 200;
    if (!result.ok)
    {
        result.error = "http status " + std::to_string(result.status);
    }
    return result;
}

HttpResult HttpDownloadFile(
    const std::string &url,
    const std::vector<std::string> &headers,
    const std::wstring &destination,
    HttpProgress progress,
    void *context,
    const std::atomic<bool> *cancel)
{
    Request request;
    if (!request.Open(url, headers))
    {
        return Failure(request.stage.c_str());
    }

    HttpResult result;
    result.status = request.Status();
    if (result.status != 200)
    {
        result.error = "http status " + std::to_string(result.status);
        return result;
    }

    const HANDLE file = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return Failure("create_file");
    }

    const uint64_t total = request.ContentLength();
    uint64_t received = 0;
    std::vector<char> buffer(64 * 1024);
    bool failed = false;

    while (!failed)
    {
        if (cancel != nullptr && *cancel)
        {
            result.error = "cancelled";
            failed = true;
            break;
        }

        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.request.value, &available))
        {
            result.error = "query_available win32=" + std::to_string(GetLastError());
            failed = true;
            break;
        }
        if (available == 0)
        {
            break;
        }

        DWORD read = 0;
        const DWORD want = available > buffer.size() ? static_cast<DWORD>(buffer.size()) : available;
        if (!WinHttpReadData(request.request.value, buffer.data(), want, &read) || read == 0)
        {
            result.error = "read win32=" + std::to_string(GetLastError());
            failed = true;
            break;
        }

        DWORD written = 0;
        if (!WriteFile(file, buffer.data(), read, &written, nullptr) || written != read)
        {
            result.error = "write win32=" + std::to_string(GetLastError());
            failed = true;
            break;
        }

        received += read;
        if (progress != nullptr)
        {
            progress(context, received, total);
        }
    }

    CloseHandle(file);
    if (failed)
    {
        DeleteFileW(destination.c_str());
        return result;
    }

    result.ok = true;
    return result;
}
}
