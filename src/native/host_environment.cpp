// the app family headers hide these calls, kernel32 still exports them and SDL_setenv keeps a private copy
#include "host_environment.h"

#include <Windows.h>

#include <vector>

namespace stardew_uwp
{
bool SetWorkingDirectory(const std::wstring &path, unsigned long &error)
{
    if (SetCurrentDirectoryW(path.c_str()))
    {
        error = 0;
        return true;
    }

    error = GetLastError();
    return false;
}

std::wstring GetEnvironmentValue(const std::wstring &name)
{
    const DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (size == 0)
    {
        return {};
    }

    std::vector<wchar_t> buffer(size);
    const DWORD written = GetEnvironmentVariableW(name.c_str(), buffer.data(), size);
    if (written == 0 || written >= size)
    {
        return {};
    }

    return std::wstring(buffer.data(), written);
}

bool SetEnvironmentValue(const std::wstring &name, const std::wstring &value, unsigned long &error)
{
    if (SetEnvironmentVariableW(name.c_str(), value.c_str()))
    {
        error = 0;
        return true;
    }

    error = GetLastError();
    return false;
}

// SMAPI sets Console.Title before it reads its arguments and that needs a console the process owns
bool AttachOwnConsole(unsigned long &error)
{
    if (AllocConsole())
    {
        error = 0;
        return true;
    }

    error = GetLastError();
    return false;
}

namespace
{
bool SameSize(const std::wstring &left, const std::wstring &right)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    WIN32_FILE_ATTRIBUTE_DATA b{};
    if (!GetFileAttributesExW(left.c_str(), GetFileExInfoStandard, &a) ||
        !GetFileAttributesExW(right.c_str(), GetFileExInfoStandard, &b))
    {
        return false;
    }

    return a.nFileSizeLow == b.nFileSizeLow && a.nFileSizeHigh == b.nFileSizeHigh;
}

int CopyTree(const std::wstring &from, const std::wstring &to, std::string &error)
{
    CreateDirectoryW(to.c_str(), nullptr);

    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW((from + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
    {
        error = "source folder missing";
        return 0;
    }

    int written = 0;
    do
    {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..")
        {
            continue;
        }

        const std::wstring source = from + L"\\" + name;
        const std::wstring target = to + L"\\" + name;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            written += CopyTree(source, target, error);
            continue;
        }

        if (SameSize(source, target))
        {
            continue;
        }

        if (CopyFileW(source.c_str(), target.c_str(), FALSE))
        {
            written += 1;
        }
        else if (error.empty())
        {
            error = "copy failed win32=" + std::to_string(GetLastError());
        }
    } while (FindNextFileW(search, &found));

    FindClose(search);
    return written;
}
}

int SyncFolder(const std::wstring &source, const std::wstring &target, std::string &error)
{
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        error = "not in package";
        return 0;
    }

    return CopyTree(source, target, error);
}
}
