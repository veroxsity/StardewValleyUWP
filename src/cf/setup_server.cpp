#define _CRT_RAND_S
// winsock2 must come before windows.h or the 1.1 headers define every socket struct twice
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "setup_server.h"

#include "text.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace stardew_uwp
{
namespace
{
SOCKET g_listener = INVALID_SOCKET;
std::thread g_thread;
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_saved{false};
std::wstring g_localState;
std::string g_url;
std::string g_error;
std::string g_secret;
bool g_winsock = false;

std::string RandomSecret()
{
    static const char *digits = "0123456789abcdef";
    std::string value;
    for (int at = 0; at < 8; at++)
    {
        unsigned int number = 0;
        if (rand_s(&number) != 0)
        {
            number = GetTickCount();
        }
        value.push_back(digits[number & 0x0F]);
    }
    return value;
}

// a vpn or tailscale adapter can answer first and a phone cannot reach it, so home ranges rank higher
int AddressRank(const std::string &address)
{
    if (address.compare(0, 8, "192.168.") == 0)
    {
        return 0;
    }
    if (address.compare(0, 3, "10.") == 0)
    {
        return 1;
    }
    if (address.compare(0, 4, "172.") == 0)
    {
        const int second = std::atoi(address.c_str() + 4);
        return second >= 16 && second <= 31 ? 1 : 3;
    }
    if (address.compare(0, 4, "100.") == 0)
    {
        return 4;
    }
    return 3;
}

std::string LocalAddress()
{
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) != 0)
    {
        return {};
    }

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &found) != 0)
    {
        return host;
    }

    std::string address;
    int best = 99;
    for (const addrinfo *at = found; at != nullptr; at = at->ai_next)
    {
        const sockaddr_in *inet = reinterpret_cast<const sockaddr_in *>(at->ai_addr);
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &inet->sin_addr, text, sizeof(text)) == nullptr)
        {
            continue;
        }
        if (std::string(text).compare(0, 4, "127.") == 0)
        {
            continue;
        }
        const int rank = AddressRank(text);
        if (rank < best)
        {
            best = rank;
            address = text;
        }
    }
    freeaddrinfo(found);
    return address.empty() ? std::string(host) : address;
}

std::string UrlDecode(const std::string &value)
{
    std::string out;
    for (std::size_t at = 0; at < value.size(); at++)
    {
        const char ch = value[at];
        if (ch == '+')
        {
            out.push_back(' ');
            continue;
        }
        if (ch != '%' || at + 2 >= value.size())
        {
            out.push_back(ch);
            continue;
        }

        const std::string hex = value.substr(at + 1, 2);
        out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
        at += 2;
    }
    return out;
}

std::string Trim(const std::string &value)
{
    std::size_t first = value.find_first_not_of(" \t\r\n");
    std::size_t last = value.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
}

std::string JsonEscape(const std::string &value)
{
    std::string out;
    for (const char ch : value)
    {
        if (ch == '"' || ch == '\\')
        {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

bool WriteConfig(const std::string &key)
{
    const std::wstring path = g_localState + L"\\curseforge.json";
    const std::string body = "{\n  \"apiKey\": \"" + JsonEscape(key) + "\"\n}\n";

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD written = 0;
    const bool ok = WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &written, nullptr) != FALSE && written == body.size();
    CloseHandle(file);
    return ok;
}

void Send(SOCKET client, const std::string &status, const std::string &type, const std::string &body)
{
    const std::string response = "HTTP/1.1 " + status + "\r\nContent-Type: " + type +
                                 "\r\nContent-Length: " + std::to_string(body.size()) +
                                 "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
    send(client, response.data(), static_cast<int>(response.size()), 0);
}

std::string Page(const std::string &message)
{
    return "<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>Stardew on Xbox</title></head>"
           "<body style='background:#0d0e10;color:#f4f2ed;font-family:\"Segoe UI\",sans-serif;margin:0;padding:24px'>"
           "<h2>CurseForge api key</h2>"
           "<p style='color:#a5a29c'>Paste the key from console.curseforge.com. It is stored on the console only.</p>" +
           message +
           "<form method=post>"
           "<input name=key autocomplete=off autocapitalize=off spellcheck=false "
           "style='width:100%;box-sizing:border-box;font-size:18px;padding:14px;border-radius:12px;"
           "border:1px solid #2c2f35;background:#1b1d21;color:#f4f2ed'>"
           "<button style='margin-top:16px;width:100%;font-size:18px;font-weight:600;padding:14px;border:0;border-radius:12px;"
           "background:#e3a96b;color:#121315'>Save</button>"
           "</form></body></html>";
}

void Serve(SOCKET client)
{
    std::string request;
    char buffer[2048] = {};
    std::size_t headerEnd = std::string::npos;

    while (request.size() < 16 * 1024)
    {
        const int read = recv(client, buffer, sizeof(buffer), 0);
        if (read <= 0)
        {
            break;
        }
        request.append(buffer, static_cast<std::size_t>(read));

        headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos)
        {
            continue;
        }

        std::size_t length = 0;
        const std::size_t at = request.find("Content-Length:");
        if (at != std::string::npos && at < headerEnd)
        {
            length = static_cast<std::size_t>(std::strtoul(request.c_str() + at + 15, nullptr, 10));
        }
        if (request.size() >= headerEnd + 4 + length)
        {
            break;
        }
    }

    if (headerEnd == std::string::npos)
    {
        return;
    }

    const std::size_t firstSpace = request.find(' ');
    const std::size_t secondSpace = firstSpace == std::string::npos ? std::string::npos : request.find(' ', firstSpace + 1);
    if (secondSpace == std::string::npos)
    {
        return;
    }

    const std::string method = request.substr(0, firstSpace);
    const std::string path = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);

    // the secret path stops anything else on the network reading the page or posting a key
    if (path != "/k/" + g_secret)
    {
        Send(client, "404 Not Found", "text/plain", "no");
        return;
    }

    if (method == "GET")
    {
        Send(client, "200 OK", "text/html; charset=utf-8", Page(std::string()));
        return;
    }

    if (method != "POST")
    {
        Send(client, "405 Method Not Allowed", "text/plain", "no");
        return;
    }

    const std::string body = request.substr(headerEnd + 4);
    std::string key;
    const std::size_t field = body.find("key=");
    if (field != std::string::npos)
    {
        const std::size_t end = body.find('&', field);
        key = Trim(UrlDecode(body.substr(field + 4, end == std::string::npos ? std::string::npos : end - field - 4)));
    }

    if (key.empty())
    {
        Send(client, "200 OK", "text/html; charset=utf-8", Page("<p style='color:#ec7462'>That was empty.</p>"));
        return;
    }

    // a stray control character from a paste would otherwise end up inside the json
    for (const char ch : key)
    {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F)
        {
            Send(client, "200 OK", "text/html; charset=utf-8", Page("<p style='color:#ec7462'>That does not look like a key, try pasting it again.</p>"));
            return;
        }
    }

    if (!WriteConfig(key))
    {
        Send(client, "200 OK", "text/html; charset=utf-8", Page("<p style='color:#ec7462'>Could not write to the console.</p>"));
        return;
    }

    g_saved = true;
    Send(
        client,
        "200 OK",
        "text/html; charset=utf-8",
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'></head>"
        "<body style='background:#0d0e10;color:#f4f2ed;font-family:\"Segoe UI\",sans-serif;padding:24px'>"
        "<h2>Saved</h2><p style='color:#a5a29c'>Back to the console.</p></body></html>");
}

void Accept()
{
    while (!g_stopping)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(g_listener, &readable);
        timeval timeout = {0, 300000};

        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready <= 0)
        {
            continue;
        }

        const SOCKET client = accept(g_listener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            continue;
        }

        DWORD receiveTimeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&receiveTimeout), sizeof(receiveTimeout));
        Serve(client);
        shutdown(client, SD_BOTH);
        closesocket(client);
    }
}
}

bool SetupServerStart(const std::wstring &localState, unsigned short port)
{
    SetupServerStop();

    g_localState = localState;
    g_saved = false;
    g_stopping = false;
    g_url.clear();
    g_error.clear();

    WSADATA winsock = {};
    if (!g_winsock && WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
    {
        g_error = "winsock failed";
        return false;
    }
    g_winsock = true;

    g_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listener == INVALID_SOCKET)
    {
        g_error = "socket failed " + std::to_string(WSAGetLastError());
        return false;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (bind(g_listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        g_error = "bind failed " + std::to_string(WSAGetLastError());
        closesocket(g_listener);
        g_listener = INVALID_SOCKET;
        return false;
    }

    if (listen(g_listener, 4) == SOCKET_ERROR)
    {
        g_error = "listen failed " + std::to_string(WSAGetLastError());
        closesocket(g_listener);
        g_listener = INVALID_SOCKET;
        return false;
    }

    sockaddr_in bound = {};
    int boundSize = sizeof(bound);
    unsigned short actual = port;
    if (getsockname(g_listener, reinterpret_cast<sockaddr *>(&bound), &boundSize) == 0)
    {
        actual = ntohs(bound.sin_port);
    }

    g_secret = RandomSecret();
    g_url = "http://" + LocalAddress() + ":" + std::to_string(actual) + "/k/" + g_secret;
    g_thread = std::thread(Accept);
    return true;
}

void SetupServerStop()
{
    g_stopping = true;
    if (g_thread.joinable())
    {
        g_thread.join();
    }
    if (g_listener != INVALID_SOCKET)
    {
        closesocket(g_listener);
        g_listener = INVALID_SOCKET;
    }
    g_url.clear();
}

std::string SetupServerUrl()
{
    return g_url;
}

std::string SetupServerError()
{
    return g_error;
}

bool SetupServerSaved()
{
    return g_saved;
}
}
