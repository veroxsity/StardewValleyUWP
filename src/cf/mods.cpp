#include "mods.h"

#include "json.h"
#include "text.h"

#include <windows.h>

#include "third_party/miniz/miniz.h"

#include <algorithm>

namespace stardew_uwp
{
std::string ReadSmallFile(const std::wstring &path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return {};
    }

    std::string text;
    char buffer[4096] = {};
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read != 0 && text.size() < 256 * 1024)
    {
        text.append(buffer, read);
    }
    CloseHandle(file);
    return text;
}

namespace
{
std::string StripBom(const std::string &text)
{
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    {
        return text.substr(3);
    }
    return text;
}
}

std::vector<InstalledMod> ScanMods(const std::wstring &modsRoot)
{
    std::vector<InstalledMod> mods;

    WIN32_FIND_DATAW found = {};
    const HANDLE search = FindFirstFileW((modsRoot + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
    {
        return mods;
    }

    do
    {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            continue;
        }
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L".." || name.empty() || name[0] == L'.')
        {
            continue;
        }

        InstalledMod mod;
        mod.folder = Narrow(name);
        mod.name = mod.folder;

        JsonValue manifest;
        std::string error;
        // authors write manifests by hand, so a broken one is normal and the folder name still identifies the mod
        if (JsonParse(StripBom(ReadSmallFile(modsRoot + L"\\" + name + L"\\manifest.json")), manifest, error))
        {
            mod.name = manifest.StringField("Name", mod.folder);
            mod.version = manifest.StringField("Version");
            mod.uniqueId = manifest.StringField("UniqueID");
        }
        mods.push_back(mod);
    } while (FindNextFileW(search, &found));

    FindClose(search);
    std::sort(mods.begin(), mods.end(), [](const InstalledMod &left, const InstalledMod &right) { return left.name < right.name; });
    return mods;
}

namespace
{
bool UnsafeEntry(const std::string &name)
{
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos)
    {
        return true;
    }
    if (name.compare(0, 9, "__MACOSX/") == 0)
    {
        return true;
    }
    // a zip is allowed to name ..\ and windows will happily follow it out of the mods folder
    return name.find("..") != std::string::npos;
}

std::string LowerCopy(const std::string &value)
{
    std::string result = value;
    for (char &ch : result)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return result;
}

std::string LastComponent(const std::string &prefix)
{
    std::string trimmed = prefix;
    while (!trimmed.empty() && trimmed.back() == '/')
    {
        trimmed.pop_back();
    }
    const std::size_t slash = trimmed.find_last_of('/');
    return slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
}

bool EnsureDirectories(const std::wstring &path)
{
    for (std::size_t at = 0; at < path.size(); at++)
    {
        if (path[at] != L'\\')
        {
            continue;
        }
        const std::wstring part = path.substr(0, at);
        if (part.size() > 2)
        {
            CreateDirectoryW(part.c_str(), nullptr);
        }
    }
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
}
}

bool InstallModZip(const std::wstring &zipPath, const std::wstring &modsRoot, std::vector<std::string> &folders, std::string &error)
{
    folders.clear();

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    // miniz opens paths with ANSI fopen, so files go through wide handles and non-ascii names survive
    FILE *archive = nullptr;
    if (_wfopen_s(&archive, zipPath.c_str(), L"rb") != 0 || archive == nullptr || !mz_zip_reader_init_cfile(&zip, archive, 0, 0))
    {
        if (archive != nullptr)
        {
            fclose(archive);
        }
        error = "not a readable zip";
        return false;
    }

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    std::vector<std::string> names(count);
    for (mz_uint index = 0; index < count; index++)
    {
        char name[512] = {};
        mz_zip_reader_get_filename(&zip, index, name, sizeof(name));
        std::string entry = name;
        std::replace(entry.begin(), entry.end(), '\\', '/');
        names[index] = entry;
    }

    std::vector<std::string> roots;
    for (const std::string &entry : names)
    {
        if (UnsafeEntry(entry))
        {
            continue;
        }
        const std::string lower = LowerCopy(entry);
        if (lower.size() < 13 || lower.compare(lower.size() - 13, 13, "manifest.json") != 0)
        {
            continue;
        }
        roots.push_back(entry.substr(0, entry.size() - 13));
    }

    if (roots.empty())
    {
        mz_zip_reader_end(&zip);
        error = "no manifest.json in the zip";
        return false;
    }

    // a bundle can nest a mod inside another, keeping only the outermost roots extracts each one once
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    std::vector<std::string> outer;
    for (const std::string &root : roots)
    {
        bool nested = false;
        for (const std::string &other : roots)
        {
            if (other != root && !other.empty() && root.compare(0, other.size(), other) == 0)
            {
                nested = true;
                break;
            }
        }
        if (!nested)
        {
            outer.push_back(root);
        }
    }

    // a manifest at the zip root means the whole zip is one mod and the folder has to be named here
    std::wstring stem = zipPath.substr(zipPath.find_last_of(L'\\') + 1);
    const std::size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos)
    {
        stem = stem.substr(0, dot);
    }

    bool ok = true;
    for (mz_uint index = 0; index < count && ok; index++)
    {
        const std::string &entry = names[index];
        if (UnsafeEntry(entry))
        {
            continue;
        }

        const std::string *root = nullptr;
        for (const std::string &candidate : outer)
        {
            if (entry.compare(0, candidate.size(), candidate) == 0)
            {
                root = &candidate;
                break;
            }
        }
        if (root == nullptr)
        {
            continue;
        }

        const std::string folder = root->empty() ? Narrow(stem) : LastComponent(*root);
        if (folder.empty())
        {
            continue;
        }
        if (std::find(folders.begin(), folders.end(), folder) == folders.end())
        {
            folders.push_back(folder);
        }

        std::string relative = entry.substr(root->size());
        std::replace(relative.begin(), relative.end(), '/', '\\');
        const std::wstring target = modsRoot + L"\\" + Widen(folder) + (relative.empty() ? L"" : L"\\" + Widen(relative));

        if (mz_zip_reader_is_file_a_directory(&zip, index) || entry.back() == '/')
        {
            EnsureDirectories(target);
            continue;
        }

        const std::size_t slash = target.find_last_of(L'\\');
        if (slash != std::wstring::npos)
        {
            EnsureDirectories(target.substr(0, slash));
        }

        if (!mz_zip_reader_extract_to_file(&zip, index, Narrow(target).c_str(), 0))
        {
            error = "could not write " + Narrow(target);
            ok = false;
        }
    }

    mz_zip_reader_end(&zip);
    // init_cfile leaves the FILE* open, which locked cf-download.zip and broke the next download
    fclose(archive);
    if (!ok)
    {
        return false;
    }
    if (folders.empty())
    {
        error = "nothing was extracted";
        return false;
    }
    return true;
}

namespace
{
bool RemoveTree(const std::wstring &path)
{
    WIN32_FIND_DATAW found = {};
    const HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }

            const std::wstring child = path + L"\\" + name;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                RemoveTree(child);
            }
            else
            {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    return RemoveDirectoryW(path.c_str()) != FALSE;
}
}

bool RemoveMod(const std::wstring &modsRoot, const std::string &folder, std::string &error)
{
    if (folder.empty() || folder.find('\\') != std::string::npos || folder.find('/') != std::string::npos ||
        folder.find("..") != std::string::npos || folder.find(':') != std::string::npos)
    {
        error = "That is not a mod folder name";
        return false;
    }

    const std::wstring path = modsRoot + L"\\" + Widen(folder);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        error = "There is no folder called " + folder;
        return false;
    }

    if (!RemoveTree(path))
    {
        error = "Could not delete " + folder + ", error " + std::to_string(GetLastError());
        return false;
    }
    return true;
}
}
