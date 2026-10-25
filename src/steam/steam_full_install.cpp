#include "steam_full_install.h"
#include "steam_chunk.h"
#include "json_value.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace {

constexpr uint32_t kDirectoryFlag = 64u;
constexpr uint32_t kSymlinkFlag = 512u;

std::string shaHex(const uint8_t* data, size_t size) {
	static const char digits[] = "0123456789abcdef";
	std::string value(size * 2, '0');
	for (size_t i = 0; i < size; i++) {
		value[i * 2] = digits[data[i] >> 4];
		value[i * 2 + 1] = digits[data[i] & 15];
	}
	return value;
}

std::string chunkShaHex(const std::array<uint8_t, 20>& sha) {
	return shaHex(sha.data(), sha.size());
}

bool hashBytes(const uint8_t* data, size_t size, std::string& output) {
	output.clear();
	if ((!data && size) || size > (std::numeric_limits<ULONG>::max)()) return false;
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0))) return false;
	uint8_t digest[20] = {};
	NTSTATUS status = BCryptHash(algorithm, nullptr, 0, (PUCHAR)data, (ULONG)size, digest, sizeof(digest));
	BCryptCloseAlgorithmProvider(algorithm, 0);
	if (BCRYPT_SUCCESS(status)) output = shaHex(digest, sizeof(digest));
	SecureZeroMemory(digest, sizeof(digest));
	return BCRYPT_SUCCESS(status);
}

bool hashFile(const std::wstring& path, uint64_t expectedSize, std::string& output) {
	output.clear();
	DWORD attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || (uint64_t)size.QuadPart != expectedSize) { CloseHandle(file); return false; }
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	std::vector<uint8_t> object;
	std::vector<uint8_t> buffer(1024 * 1024);
	uint8_t digest[20] = {};
	bool ok = false;
	do {
		if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0))) break;
		DWORD objectSize = 0;
		ULONG resultSize = 0;
		if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objectSize, sizeof(objectSize), &resultSize, 0)) || resultSize != sizeof(objectSize) || !objectSize) break;
		object.resize(objectSize);
		if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0))) break;
		uint64_t total = 0;
		for (;;) {
			DWORD read = 0;
			if (!ReadFile(file, buffer.data(), (DWORD)buffer.size(), &read, nullptr)) break;
			if (!read) {
				if (total != expectedSize || !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) break;
				output = shaHex(digest, sizeof(digest));
				ok = true;
				break;
			}
			if (total > expectedSize || read > expectedSize - total || !BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0))) break;
			total += read;
		}
	} while (false);
	if (hash) BCryptDestroyHash(hash);
	if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
	if (!object.empty()) SecureZeroMemory(object.data(), object.size());
	if (!buffer.empty()) SecureZeroMemory(buffer.data(), buffer.size());
	SecureZeroMemory(digest, sizeof(digest));
	CloseHandle(file);
	return ok;
}

bool verifyManifestFile(const std::wstring& path, const SteamManifestFile& file) {
	std::string hash;
	return hashFile(path, file.size, hash) && (!file.size || hash == chunkShaHex(file.contentSha));
}

bool reservedComponent(const std::wstring& component) {
	size_t dot = component.find(L'.');
	std::wstring stem = component.substr(0, dot);
	for (wchar_t& ch : stem) if (ch >= L'a' && ch <= L'z') ch = (wchar_t)(ch - L'a' + L'A');
	if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL") return true;
	return stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) && stem[3] >= L'1' && stem[3] <= L'9';
}

bool normalizePath(const std::string& raw, size_t limit, std::string& normalized, std::wstring& wide) {
	normalized.clear();
	wide.clear();
	if (raw.empty() || raw.size() > limit || raw.find('\0') != std::string::npos) return false;
	std::string value = raw;
	for (char& ch : value) if (ch == '/') ch = '\\';
	if (value[0] == '\\' || value.find(':') != std::string::npos) return false;
	size_t offset = 0;
	while (offset <= value.size()) {
		size_t slash = value.find('\\', offset);
		size_t end = slash == std::string::npos ? value.size() : slash;
		std::string component = value.substr(offset, end - offset);
		if (component.empty() || component == "." || component == ".." || component.back() == '.' || component.back() == ' ') return false;
		for (unsigned char ch : component) if (ch < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '|' || ch == '?' || ch == '*') return false;
		if (component.size() > (size_t)(std::numeric_limits<int>::max)()) return false;
		int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, component.data(), (int)component.size(), nullptr, 0);
		if (count <= 0 || count > 255) return false;
		std::wstring part((size_t)count, L'\0');
		if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, component.data(), (int)component.size(), &part[0], count) || reservedComponent(part)) return false;
		if (!normalized.empty()) { normalized.push_back('\\'); wide.push_back(L'\\'); }
		normalized += component;
		wide += part;
		if (slash == std::string::npos) break;
		offset = slash + 1;
	}
	return !normalized.empty() && normalized.size() <= limit;
}

bool dirExists(const std::wstring& path) {
	DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool ensureDir(const std::wstring& path) {
	if (path.empty()) return false;
	if (dirExists(path)) return true;
	DWORD attributes = GetFileAttributesW(path.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES) return false;
	size_t slash = path.find_last_of(L"\\/");
	if (slash != std::wstring::npos) {
		std::wstring parent = path.substr(0, slash);
		if (!parent.empty() && !dirExists(parent) && !ensureDir(parent)) return false;
	}
	if (CreateDirectoryW(path.c_str(), nullptr)) return true;
	return GetLastError() == ERROR_ALREADY_EXISTS && dirExists(path);
}

bool clearTree(const std::wstring& path) {
	DWORD attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
	if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
	WIN32_FIND_DATAW data = {};
	HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND && RemoveDirectoryW(path.c_str()) != FALSE;
	bool ok = true;
	do {
		if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
		if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ok = false; break; }
		std::wstring child = path + L"\\" + data.cFileName;
		ok = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? clearTree(child) : DeleteFileW(child.c_str()) != FALSE;
	} while (ok && FindNextFileW(find, &data));
	DWORD findError = GetLastError();
	FindClose(find);
	if (ok && findError != ERROR_NO_MORE_FILES) ok = false;
	return ok && RemoveDirectoryW(path.c_str()) != FALSE;
}

std::wstring steamDir(const std::wstring& localState) { return localState + L"\\steam"; }
std::wstring stagingRoot(const std::wstring& localState, uint32_t appId) { return steamDir(localState) + L"\\staging\\install-" + std::to_wstring(appId); }
std::wstring finalRoot(const std::wstring& localState, uint32_t appId) { return localState + L"\\apps\\steam-" + std::to_wstring(appId); }
std::wstring backupRoot(const std::wstring& localState, uint32_t appId) { return finalRoot(localState, appId) + L".old"; }
std::wstring progressPath(const std::wstring& localState) { return steamDir(localState) + L"\\install-progress.json"; }
std::wstring recordPath(const std::wstring& localState, uint32_t appId) { return steamDir(localState) + L"\\installed-" + std::to_wstring(appId) + L".json"; }

bool contentPath(const std::wstring& root, const std::string& raw, const SteamFullInstallLimits& limits, std::wstring& path) {
	std::string normalized;
	std::wstring relative;
	if (!normalizePath(raw, limits.pathBytes, normalized, relative) || normalized != raw || root.empty()) return false;
	path = root + L"\\" + relative;
	return path.size() <= limits.fullPathChars && path.compare(0, root.size() + 1, root + L"\\") == 0;
}

bool readText(const std::wstring& path, size_t limit, std::string& text) {
	text.clear();
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size = {};
	bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && (uint64_t)size.QuadPart <= limit;
	if (ok) {
		text.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = text.empty() || (ReadFile(file, &text[0], (DWORD)text.size(), &read, nullptr) && read == text.size());
	}
	CloseHandle(file);
	if (!ok) text.clear();
	return ok;
}

bool writeAtomic(const std::wstring& path, const std::string& text) {
	if (text.size() > (std::numeric_limits<DWORD>::max)()) return false;
	std::wstring temp = path + L".tmp";
	DeleteFileW(temp.c_str());
	HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	bool ok = text.empty() || (WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size());
	if (ok) ok = FlushFileBuffers(file) != FALSE;
	CloseHandle(file);
	if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(temp.c_str()); return false; }
	return true;
}

bool preallocate(const std::wstring& path, uint64_t size) {
	if (size > (uint64_t)(std::numeric_limits<LONGLONG>::max)()) return false;
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER end = {};
	end.QuadPart = (LONGLONG)size;
	bool ok = SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
	CloseHandle(file);
	return ok;
}

bool flushFile(const std::wstring& path, uint64_t expectedSize) {
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size = {};
	bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && (uint64_t)size.QuadPart == expectedSize && FlushFileBuffers(file);
	CloseHandle(file);
	return ok;
}

bool verifyChunkRegion(const std::wstring& path, const SteamManifestChunk& chunk, uint64_t fileSize) {
	if (!chunk.originalBytes || chunk.offset > fileSize || chunk.originalBytes > fileSize - chunk.offset) return false;
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER position = {};
	position.QuadPart = (LONGLONG)chunk.offset;
	std::vector<uint8_t> data(chunk.originalBytes);
	DWORD read = 0;
	bool ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && ReadFile(file, data.data(), chunk.originalBytes, &read, nullptr) && read == chunk.originalBytes;
	CloseHandle(file);
	if (ok) {
		std::string sha;
		ok = hashBytes(data.data(), data.size(), sha) && sha == chunkShaHex(chunk.sha) && steamChunkAdler32(data.data(), data.size()) == chunk.checksum;
	}
	if (!data.empty()) SecureZeroMemory(data.data(), data.size());
	return ok;
}

bool validProgress(const SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits) {
	return !progress.steamId.empty() && progress.steamId.size() <= 32 && progress.appId == limits.appId && progress.depotId == limits.depotId && progress.manifestId == limits.manifestId && progress.buildId == limits.buildId && progress.nextFile <= limits.fileCount && progress.filesVerified <= limits.fileCount && progress.chunksVerified <= limits.chunkCount && progress.transferredBytes <= limits.maxTransferBytes && progress.updatedAt;
}

bool matchesManifest(const SteamFullInstallProgress& progress, const SteamManifestMetadata& metadata) {
	return progress.steamId == metadata.steamId && progress.appId == metadata.appId && progress.depotId == metadata.depotId && progress.manifestId == metadata.manifestId && progress.buildId == metadata.buildId;
}

bool saveProgress(const std::wstring& localState, const SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits) {
	std::string text = steamFullInstallSerializeProgress(progress, limits);
	return !text.empty() && ensureDir(steamDir(localState)) && writeAtomic(progressPath(localState), text);
}

bool validateResume(const std::wstring& localState, const SteamManifestMetadata& metadata, SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits) {
	if (!matchesManifest(progress, metadata) || progress.nextFile >= metadata.files.size() && progress.currentChunk != 0 || progress.nextFile < metadata.files.size() && progress.currentChunk > metadata.files[progress.nextFile].chunks.size()) return false;
	std::wstring root = stagingRoot(localState, metadata.appId);
	if (!dirExists(root)) return false;
	for (size_t i = 0; i < progress.nextFile; i++) {
		const auto& file = metadata.files[i];
		std::wstring path;
		if (!contentPath(root, file.path, limits, path)) return false;
		if (file.flags & kDirectoryFlag) { if (!dirExists(path)) return false; }
		else if (!verifyManifestFile(path, file)) return false;
	}
	if (progress.nextFile < metadata.files.size() && progress.currentChunk) {
		const auto& file = metadata.files[progress.nextFile];
		std::wstring path;
		if (!contentPath(root, file.path, limits, path)) return false;
		path += L".part";
		for (size_t i = 0; i < progress.currentChunk; i++) if (!verifyChunkRegion(path, file.chunks[i], file.size)) return false;
	}
	return true;
}

bool inspectMachine(const std::wstring& path, std::string& architecture) {
	architecture.clear();
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	uint8_t data[4096] = {};
	DWORD read = 0;
	bool ok = ReadFile(file, data, sizeof(data), &read, nullptr) && read >= 0x40 && data[0] == 'M' && data[1] == 'Z';
	CloseHandle(file);
	if (!ok) return false;
	uint32_t offset = 0;
	std::memcpy(&offset, data + 0x3c, sizeof(offset));
	if ((uint64_t)offset + 6 > read || std::memcmp(data + offset, "PE\0\0", 4) != 0) return false;
	uint16_t machine = 0;
	std::memcpy(&machine, data + offset + 4, sizeof(machine));
	if (machine == 0x8664) architecture = "x64";
	else if (machine == 0x014c) architecture = "x86";
	else architecture = "unsupported";
	return true;
}

bool countTree(const std::wstring& path, uint64_t& entries, uint64_t& bytes) {
	DWORD attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
	WIN32_FIND_DATAW data = {};
	HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return false;
	bool ok = true;
	do {
		if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
		if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT || entries == (std::numeric_limits<uint64_t>::max)()) { ok = false; break; }
		entries++;
		std::wstring child = path + L"\\" + data.cFileName;
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = countTree(child, entries, bytes);
		else {
			uint64_t size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
			if (bytes > (std::numeric_limits<uint64_t>::max)() - size) ok = false;
			else bytes += size;
		}
	} while (ok && FindNextFileW(find, &data));
	DWORD findError = GetLastError();
	FindClose(find);
	return ok && findError == ERROR_NO_MORE_FILES;
}

bool verifyTree(const std::wstring& root, const SteamManifestMetadata& metadata, const SteamFullInstallLimits& limits) {
	for (const auto& file : metadata.files) {
		std::wstring path;
		if (!contentPath(root, file.path, limits, path)) return false;
		if (file.flags & kDirectoryFlag) { if (!dirExists(path)) return false; }
		else if (!verifyManifestFile(path, file)) return false;
	}
	uint64_t entries = 0;
	uint64_t bytes = 0;
	return countTree(root, entries, bytes) && entries == metadata.files.size() && bytes == metadata.originalBytes;
}

bool quickVerifyRecordRoot(const std::wstring& root, const SteamInstalledRecord& record) {
	uint64_t entries = 0;
	uint64_t bytes = 0;
	if (!countTree(root, entries, bytes) || entries != record.fileCount || bytes != record.installedBytes) return false;
	std::wstring executable = root + L"\\" + std::wstring(record.executable.begin(), record.executable.end());
	std::string architecture;
	return inspectMachine(executable, architecture) && architecture == record.architecture;
}

bool jsonUInt64(const json::JsonValue& value, uint64_t& result) {
	if (value.type != json::JsonType::Number || value.number.empty() || value.number[0] == '-') return false;
	char* end = nullptr;
	errno = 0;
	unsigned long long parsed = _strtoui64(value.number.c_str(), &end, 10);
	if (errno == ERANGE || !end || *end) return false;
	result = parsed;
	return true;
}

bool jsonUInt32(const json::JsonValue& value, uint32_t& result) {
	uint64_t value64 = 0;
	if (!jsonUInt64(value, value64) || value64 > 0xffffffffull) return false;
	result = (uint32_t)value64;
	return true;
}

json::JsonValue number(uint64_t value) { return json::JsonValue::makeNumber(std::to_string(value)); }
const json::JsonValue* member(const json::JsonValue& value, const char* name) { return json::get(value, name); }

bool validRecord(const SteamInstalledRecord& record, const SteamFullInstallLimits& limits) {
	return !record.steamId.empty() && record.steamId.size() <= 32 && record.appId && record.depotId && record.manifestId && !record.buildId.empty() && !record.executable.empty() && record.appId == limits.appId && record.depotId == limits.depotId && record.manifestId == limits.manifestId && record.buildId == limits.buildId && record.installDir == "apps/steam-" + std::to_string(limits.appId) && record.executable == limits.executablePath && (record.architecture == "x64" || record.architecture == "x86" || record.architecture == "unsupported") && record.installedBytes == limits.installBytes && record.fileCount == limits.fileCount && record.chunkCount == limits.chunkCount && record.installedAt;
}

bool loadRecordAt(const std::wstring& localState, uint32_t appId, const std::wstring& source, SteamInstalledRecord& record) {
	record = SteamInstalledRecord();
	std::string text;
	if (!readText(source, 64u * 1024u, text)) return false;
	json::JsonValue root;
	json::JsonParser parser(text);
	if (!parser.parse(root) || root.type != json::JsonType::Object || json::stringValue(root, "state") != "installed") return false;
	uint64_t version = 0;
	const auto* versionValue = member(root, "version");
	const auto* appIdValue = member(root, "appId");
	const auto* depotIdValue = member(root, "depotId");
	const auto* manifestIdValue = member(root, "manifestId");
	const auto* installedBytesValue = member(root, "installedBytes");
	const auto* fileCountValue = member(root, "fileCount");
	const auto* chunkCountValue = member(root, "chunkCount");
	const auto* installedAtValue = member(root, "installedAt");
	if (!versionValue || !jsonUInt64(*versionValue, version) || version != 1 || !appIdValue || !jsonUInt32(*appIdValue, record.appId) || record.appId != appId || !depotIdValue || !jsonUInt32(*depotIdValue, record.depotId) || !manifestIdValue || !jsonUInt64(*manifestIdValue, record.manifestId) || !installedBytesValue || !jsonUInt64(*installedBytesValue, record.installedBytes) || !fileCountValue || !jsonUInt64(*fileCountValue, record.fileCount) || !chunkCountValue || !jsonUInt64(*chunkCountValue, record.chunkCount) || !installedAtValue || !jsonUInt64(*installedAtValue, record.installedAt)) return false;
	record.steamId = json::stringValue(root, "steamId");
	record.buildId = json::stringValue(root, "buildId");
	record.installDir = json::stringValue(root, "installDir");
	record.executable = json::stringValue(root, "executable");
	record.architecture = json::stringValue(root, "architecture");
	SteamFullInstallLimits limits = steamFullInstallDefaultLimits();
	limits.appId = record.appId;
	limits.depotId = record.depotId;
	limits.manifestId = record.manifestId;
	limits.buildId = record.buildId;
	limits.executablePath = record.executable;
	limits.installBytes = record.installedBytes;
	limits.fileCount = (size_t)record.fileCount;
	limits.chunkCount = (size_t)record.chunkCount;
	if (record.fileCount > (std::numeric_limits<size_t>::max)() || record.chunkCount > (std::numeric_limits<size_t>::max)() || !validRecord(record, limits)) return false;
	// the host copies SMAPI and the patched FNA over the install on every launch, so counting files would fail after the first Play
	std::wstring executable = finalRoot(localState, appId) + L"\\" + std::wstring(record.executable.begin(), record.executable.end());
	std::string architecture;
	return inspectMachine(executable, architecture) && architecture == record.architecture;
}

}

SteamFullInstallLimits steamFullInstallDefaultLimits() { return SteamFullInstallLimits(); }

std::string steamFullInstallUnrealShippingCandidate(const std::string& executablePath) {
	std::string path = executablePath;
	for (char& ch : path) if (ch == '/') ch = '\\';
	size_t slash = path.find_last_of('\\');
	size_t name = slash == std::string::npos ? 0 : slash + 1;
	if (path.size() <= name + 4 || _stricmp(path.c_str() + path.size() - 4, ".exe") != 0) return "";
	std::string lower = path;
	for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
	if (lower.find("\\binaries\\win64\\") != std::string::npos) return "";
	std::string stem = path.substr(name, path.size() - name - 4);
	return path.substr(0, name) + stem + "\\Binaries\\Win64\\" + stem + "-Win64-Shipping.exe";
}

bool steamFullInstallMergeManifests(const std::vector<SteamManifestMetadata>& manifests, SteamManifestMetadata& merged, std::string& error) {
	merged = SteamManifestMetadata();
	error.clear();
	if (manifests.empty()) { error = "Full install has no manifests"; return false; }
	merged = manifests.front();
	merged.files.clear();
	merged.originalBytes = 0;
	merged.compressedBytes = 0;
	merged.uniqueChunks = 0;
	std::map<std::string, size_t> paths;
	std::map<std::array<uint8_t, 20>, std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> chunks;
	std::set<uint32_t> depots;
	for (const auto& manifest : manifests) {
		std::string manifestError;
		if (!steamManifestValidate(manifest, steamManifestDefaultLimits(), manifestError) || manifest.steamId != merged.steamId || manifest.appId != merged.appId || manifest.buildId != merged.buildId || !depots.insert(manifest.depotId).second) { error = manifestError.empty() ? "Full install manifest set identity is invalid" : manifestError; return false; }
		for (auto file : manifest.files) {
			file.sourceDepotId = manifest.depotId;
			std::string key = file.path;
			for (char& ch : key) {
				if (ch == '/') ch = '\\';
				else ch = (char)std::tolower((unsigned char)ch);
			}
			auto found = paths.find(key);
			if (found == paths.end()) {
				paths[key] = merged.files.size();
				merged.files.push_back(std::move(file));
			} else merged.files[found->second] = std::move(file);
		}
	}
	for (const auto& file : merged.files) {
		if (merged.originalBytes > (std::numeric_limits<uint64_t>::max)() - file.size) { error = "Full install merged size overflows"; return false; }
		merged.originalBytes += file.size;
		for (const auto& chunk : file.chunks) {
			auto value = std::make_pair(chunk.checksum, std::make_pair(chunk.originalBytes, chunk.compressedBytes));
			auto inserted = chunks.emplace(chunk.sha, value);
			if (!inserted.second && inserted.first->second != value) { error = "Full install merged chunk identity is inconsistent"; return false; }
		}
	}
	for (const auto& chunk : chunks) {
		if (merged.compressedBytes > (std::numeric_limits<uint64_t>::max)() - chunk.second.second.second) { error = "Full install merged transfer size overflows"; return false; }
		merged.compressedBytes += chunk.second.second.second;
	}
	if (chunks.size() > 0xffffffffu) { error = "Full install merged chunk count exceeds policy"; return false; }
	merged.uniqueChunks = (uint32_t)chunks.size();
	std::string manifestError;
	if (!steamManifestValidate(merged, steamManifestDefaultLimits(), manifestError)) { error = manifestError; return false; }
	return true;
}

bool steamFullInstallConfigure(const SteamManifestMetadata& metadata, const std::string& executablePath, SteamFullInstallLimits& limits, std::string& error) {
	limits = steamFullInstallDefaultLimits();
	error.clear();
	std::string manifestError;
	if (!steamManifestValidate(metadata, steamManifestDefaultLimits(), manifestError) || metadata.steamId.empty() || !metadata.appId || !metadata.depotId || !metadata.manifestId || metadata.buildId.empty() || metadata.originalBytes > limits.maxInstallBytes || executablePath.empty()) { error = manifestError.empty() ? "Full install manifest identity is invalid" : manifestError; return false; }
	std::string normalized;
	std::wstring wide;
	if (!normalizePath(executablePath, limits.pathBytes, normalized, wide)) { error = "Full install executable path is invalid"; return false; }
	const SteamManifestFile* executable = nullptr;
	const SteamManifestFile* shipping = nullptr;
	std::string shippingPath = steamFullInstallUnrealShippingCandidate(normalized);
	size_t chunkCount = 0;
	for (const auto& file : metadata.files) {
		if (chunkCount > (std::numeric_limits<size_t>::max)() - file.chunks.size()) { error = "Full install chunk count overflows"; return false; }
		chunkCount += file.chunks.size();
		if (_stricmp(file.path.c_str(), normalized.c_str()) == 0) executable = &file;
		if (!shippingPath.empty() && _stricmp(file.path.c_str(), shippingPath.c_str()) == 0) shipping = &file;
	}
	if (shipping) executable = shipping;
	if (!executable || (executable->flags & (kDirectoryFlag | kSymlinkFlag)) || !executable->linkTarget.empty() || !executable->hasContentSha || !executable->size || executable->chunks.empty()) { error = "Full install launch executable is not a regular manifest file"; return false; }
	limits.appId = metadata.appId;
	limits.depotId = metadata.depotId;
	limits.manifestId = metadata.manifestId;
	limits.buildId = metadata.buildId;
	limits.executablePath = executable->path;
	limits.executableSha = chunkShaHex(executable->contentSha);
	limits.executableBytes = executable->size;
	limits.installBytes = metadata.originalBytes;
	limits.uniqueCompressedBytes = metadata.compressedBytes;
	limits.fileCount = metadata.files.size();
	limits.chunkCount = chunkCount;
	limits.uniqueChunkCount = metadata.uniqueChunks;
	return true;
}
bool steamFullInstallValidateManifest(const SteamManifestMetadata& metadata, const SteamFullInstallLimits& limits, uint64_t& referenceTransferBytes, std::string& error) {
	referenceTransferBytes = 0;
	error.clear();
	std::string manifestError;
	if (!steamManifestValidate(metadata, steamManifestDefaultLimits(), manifestError) || metadata.steamId.empty() || metadata.steamId.size() > 32 || metadata.appId != limits.appId || metadata.depotId != limits.depotId || metadata.manifestId != limits.manifestId || metadata.buildId != limits.buildId || metadata.files.size() != limits.fileCount || metadata.originalBytes != limits.installBytes || metadata.compressedBytes != limits.uniqueCompressedBytes || metadata.uniqueChunks != limits.uniqueChunkCount || metadata.originalBytes > limits.maxInstallBytes) { error = manifestError.empty() ? "Full install manifest identity is invalid" : manifestError; return false; }
	size_t chunks = 0;
	bool executableFound = false;
	for (const auto& file : metadata.files) {
		if (chunks > limits.chunkCount - file.chunks.size()) { error = "Full install chunk count overflows"; return false; }
		chunks += file.chunks.size();
		for (const auto& chunk : file.chunks) {
			if (referenceTransferBytes > limits.maxTransferBytes - chunk.compressedBytes) { error = "Full install transfer size exceeds policy"; return false; }
			referenceTransferBytes += chunk.compressedBytes;
		}
		if (file.path == limits.executablePath) executableFound = !(file.flags & (kDirectoryFlag | kSymlinkFlag)) && file.size == limits.executableBytes && file.hasContentSha && chunkShaHex(file.contentSha) == limits.executableSha;
	}
	if (chunks != limits.chunkCount || !executableFound) { error = "Full install manifest content boundary is invalid"; return false; }
	return true;
}

bool steamFullInstallBegin(const std::wstring& localState, const SteamManifestMetadata& metadata, const SteamFullInstallLimits& limits, SteamFullInstallProgress& progress, uint64_t& referenceTransferBytes, bool& resumed, std::string& error) {
	progress = SteamFullInstallProgress();
	resumed = false;
	if (localState.empty() || !steamFullInstallValidateManifest(metadata, limits, referenceTransferBytes, error)) return false;
	std::wstring final = finalRoot(localState, metadata.appId);
	DWORD finalAttributes = GetFileAttributesW(final.c_str());
	if (finalAttributes != INVALID_FILE_ATTRIBUTES) {
		if (!(finalAttributes & FILE_ATTRIBUTE_DIRECTORY)) { error = "Full install destination is not a folder"; return false; }
		// a folder with no valid record is downloaded over, the finish step moves it aside and swaps the new one in
		SteamInstalledRecord current;
		if (loadRecordAt(localState, metadata.appId, recordPath(localState, metadata.appId), current) && current.buildId == metadata.buildId && current.manifestId == metadata.manifestId) { error = "Requested Steam build is already installed"; return false; }
	}
	std::string cached;
	SteamFullInstallProgress loaded;
	bool cachedProgress = readText(progressPath(localState), limits.recordBytes, cached) && steamFullInstallDeserializeProgress(cached, limits, loaded);
	if (cachedProgress && validateResume(localState, metadata, loaded, limits)) {
		progress = loaded;
		resumed = true;
		return true;
	}
	if (cachedProgress && loaded.appId && loaded.appId != metadata.appId) steamFullInstallClearStaging(localState, loaded.appId, limits);
	steamFullInstallClearStaging(localState, metadata.appId, limits);
	std::wstring parent = steamDir(localState) + L"\\staging";
	if (!ensureDir(parent)) { error = "Full install staging parent could not be created"; return false; }
	ULARGE_INTEGER available = {};
	if (!GetDiskFreeSpaceExW(parent.c_str(), &available, nullptr, nullptr) || metadata.originalBytes > (std::numeric_limits<uint64_t>::max)() - limits.freeSpaceMarginBytes || available.QuadPart < metadata.originalBytes + limits.freeSpaceMarginBytes) { error = "Full install requires more free space"; return false; }
	if (!ensureDir(stagingRoot(localState, metadata.appId))) { error = "Full install staging root could not be created"; return false; }
	progress.steamId = metadata.steamId;
	progress.appId = metadata.appId;
	progress.depotId = metadata.depotId;
	progress.manifestId = metadata.manifestId;
	progress.buildId = metadata.buildId;
	progress.updatedAt = (uint64_t)time(nullptr);
	if (!saveProgress(localState, progress, limits)) { steamFullInstallClearStaging(localState, metadata.appId, limits); error = "Full install progress could not be saved"; return false; }
	return true;
}

bool steamFullInstallPrepareFile(const std::wstring& localState, const SteamManifestMetadata& metadata, SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits, std::string& error) {
	error.clear();
	if (!validProgress(progress, limits) || !matchesManifest(progress, metadata) || progress.nextFile >= metadata.files.size()) { error = "Full install file progress is invalid"; return false; }
	const auto& file = metadata.files[progress.nextFile];
	std::wstring path;
	if (!contentPath(stagingRoot(localState, metadata.appId), file.path, limits, path)) { error = "Full install file path is invalid"; return false; }
	if (file.flags & kDirectoryFlag) {
		if (progress.currentChunk || !ensureDir(path)) { error = "Full install directory could not be created"; return false; }
		progress.nextFile++;
		progress.filesVerified++;
		progress.updatedAt = (uint64_t)time(nullptr);
		return saveProgress(localState, progress, limits);
	}
	size_t slash = path.find_last_of(L'\\');
	if (slash == std::wstring::npos || !ensureDir(path.substr(0, slash))) { error = "Full install parent directory could not be created"; return false; }
	std::wstring part = path + L".part";
	if (!file.size) {
		if (progress.currentChunk || !preallocate(part, 0)) { error = "Full install empty file could not be created"; return false; }
		if (!verifyManifestFile(part, file) || !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(part.c_str()); error = "Full install empty file verification failed"; return false; }
		progress.nextFile++;
		progress.filesVerified++;
		progress.updatedAt = (uint64_t)time(nullptr);
		return saveProgress(localState, progress, limits);
	}
	if (!progress.currentChunk) {
		DeleteFileW(part.c_str());
		DeleteFileW(path.c_str());
		if (!preallocate(part, file.size)) { error = "Full install file could not be preallocated"; return false; }
	} else {
		for (size_t i = 0; i < progress.currentChunk; i++) if (!verifyChunkRegion(part, file.chunks[i], file.size)) { error = "Full install resumable chunk failed verification"; return false; }
	}
	return true;
}

bool steamFullInstallWriteChunks(const std::wstring& localState, const SteamManifestMetadata& metadata, const SteamFullInstallProgress& progress, size_t fileIndex, const std::vector<std::vector<uint8_t>>& chunks, const SteamFullInstallLimits& limits, std::string& error) {
	error.clear();
	if (chunks.empty() || fileIndex != progress.nextFile || fileIndex >= metadata.files.size()) { error = "Full install chunk batch position is invalid"; return false; }
	const auto& file = metadata.files[fileIndex];
	if (progress.currentChunk > file.chunks.size() || chunks.size() > file.chunks.size() - progress.currentChunk) { error = "Full install chunk batch length is invalid"; return false; }
	std::wstring path;
	if (!contentPath(stagingRoot(localState, metadata.appId), file.path, limits, path)) { error = "Full install chunk batch path is invalid"; return false; }
	path += L".part";
	DWORD attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) { error = "Full install chunk batch file is invalid"; return false; }
	HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) { error = "Full install chunk batch file could not be opened"; return false; }
	LARGE_INTEGER actual = {};
	bool ok = GetFileSizeEx(handle, &actual) && actual.QuadPart >= 0 && (uint64_t)actual.QuadPart == file.size;
	for (size_t i = 0; ok && i < chunks.size(); i++) {
		const auto& chunk = file.chunks[progress.currentChunk + i];
		const auto& data = chunks[i];
		std::string sha;
		if (data.size() != chunk.originalBytes || !hashBytes(data.data(), data.size(), sha) || sha != chunkShaHex(chunk.sha) || steamChunkAdler32(data.data(), data.size()) != chunk.checksum || chunk.offset > (uint64_t)(std::numeric_limits<LONGLONG>::max)()) { ok = false; break; }
		LARGE_INTEGER position = {};
		position.QuadPart = (LONGLONG)chunk.offset;
		DWORD written = 0;
		ok = SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) && WriteFile(handle, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
	}
	CloseHandle(handle);
	if (!ok) error = "Full install chunk batch write failed";
	return ok;
}

bool steamFullInstallSaveProgress(const std::wstring& localState, const SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits) {
	return validProgress(progress, limits) && saveProgress(localState, progress, limits);
}

bool steamFullInstallFinalizeFile(const std::wstring& localState, const SteamManifestMetadata& metadata, SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits, std::string& error) {
	error.clear();
	if (!validProgress(progress, limits) || progress.nextFile >= metadata.files.size()) { error = "Full install file finalization state is invalid"; return false; }
	const auto& file = metadata.files[progress.nextFile];
	if (file.flags & kDirectoryFlag || progress.currentChunk != file.chunks.size()) { error = "Full install file has missing chunks"; return false; }
	std::wstring path;
	if (!contentPath(stagingRoot(localState, metadata.appId), file.path, limits, path)) { error = "Full install file path is invalid"; return false; }
	std::wstring part = path + L".part";
	if (!flushFile(part, file.size)) { error = "Full install file flush failed"; return false; }
	if (!verifyManifestFile(part, file)) { error = "Full install complete file hash is invalid"; return false; }
	if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { error = "Full install file finalization failed"; return false; }
	if (!verifyManifestFile(path, file)) { DeleteFileW(path.c_str()); error = "Full install finalized file hash is invalid"; return false; }
	progress.nextFile++;
	progress.currentChunk = 0;
	progress.filesVerified++;
	progress.updatedAt = (uint64_t)time(nullptr);
	return saveProgress(localState, progress, limits);
}

bool steamFullInstallComplete(const std::wstring& localState, const SteamManifestMetadata& metadata, const SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits, SteamInstalledRecord& record, std::string& error) {
	record = SteamInstalledRecord();
	error.clear();
	uint64_t referenceTransferBytes = 0;
	if (!steamFullInstallValidateManifest(metadata, limits, referenceTransferBytes, error) || !validProgress(progress, limits) || !matchesManifest(progress, metadata) || progress.nextFile != metadata.files.size() || progress.currentChunk || progress.filesVerified != metadata.files.size() || progress.chunksVerified != limits.chunkCount || progress.transferredBytes != referenceTransferBytes) { if (error.empty()) error = "Full install completion state is invalid"; return false; }
	std::wstring staging = stagingRoot(localState, metadata.appId);
	if (!verifyTree(staging, metadata, limits)) { error = "Full install staging verification failed"; return false; }
	std::wstring executable;
	if (!contentPath(staging, limits.executablePath, limits, executable)) { error = "Full install executable path is invalid"; return false; }
	std::string architecture;
	if (!inspectMachine(executable, architecture)) { error = "Full install executable PE inspection failed"; return false; }
	record.steamId = metadata.steamId;
	record.appId = metadata.appId;
	record.depotId = metadata.depotId;
	record.manifestId = metadata.manifestId;
	record.buildId = metadata.buildId;
	record.installDir = "apps/steam-" + std::to_string(metadata.appId);
	record.executable = limits.executablePath;
	record.architecture = architecture;
	record.installedBytes = metadata.originalBytes;
	record.fileCount = metadata.files.size();
	record.chunkCount = limits.chunkCount;
	record.installedAt = (uint64_t)time(nullptr);
	std::string recordText = steamFullInstallSerializeRecord(record, limits);
	if (recordText.empty() || !ensureDir(steamDir(localState))) { error = "Full install record is invalid"; return false; }
	std::wstring pendingRecord = recordPath(localState, metadata.appId) + L".pending";
	DeleteFileW(pendingRecord.c_str());
	if (!writeAtomic(pendingRecord, recordText)) { error = "Full install pending record could not be saved"; return false; }
	std::wstring final = finalRoot(localState, metadata.appId);
	std::wstring backup = backupRoot(localState, metadata.appId);
	bool updating = dirExists(final);
	if ((GetFileAttributesW(final.c_str()) != INVALID_FILE_ATTRIBUTES && !updating)
		|| (dirExists(backup) && !clearTree(backup))
		|| !ensureDir(localState + L"\\apps")
		|| (updating && !MoveFileExW(final.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH))) {
		DeleteFileW(pendingRecord.c_str());
		error = "Full install update preparation failed";
		return false;
	}
	auto rollback = [&] {
		if (dirExists(final)) clearTree(final);
		if (updating && dirExists(backup)) MoveFileExW(backup.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH);
		DeleteFileW(pendingRecord.c_str());
	};
	if (!MoveFileExW(staging.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH)) { rollback(); error = "Full install directory finalization failed"; return false; }
	if (!verifyTree(final, metadata, limits) || !quickVerifyRecordRoot(final, record)) { rollback(); error = "Full install finalized tree verification failed"; return false; }
	if (!MoveFileExW(pendingRecord.c_str(), recordPath(localState, metadata.appId).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { rollback(); error = "Full install record finalization failed"; return false; }
	if (updating) clearTree(backup);
	DeleteFileW(progressPath(localState).c_str());
	return true;
}

bool steamFullInstallLoadRecordAny(const std::wstring& localState, uint32_t appId, SteamInstalledRecord& record) {
	record = SteamInstalledRecord();
	if (!appId) return false;
	std::wstring path = recordPath(localState, appId);
	std::wstring pending = path + L".pending";
	std::wstring final = finalRoot(localState, appId);
	std::wstring backup = backupRoot(localState, appId);
	if (!dirExists(final) && dirExists(backup)) {
		MoveFileExW(backup.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH);
		DeleteFileW(pending.c_str());
	}
	bool valid = loadRecordAt(localState, appId, path, record);
	if (!valid) {
		record = SteamInstalledRecord();
		if (loadRecordAt(localState, appId, pending, record) && MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) valid = true;
		else if (dirExists(backup) && clearTree(final) && MoveFileExW(backup.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH)) {
			record = SteamInstalledRecord();
			valid = loadRecordAt(localState, appId, path, record);
		}
	}
	if (valid) { DeleteFileW(pending.c_str()); if (dirExists(backup)) clearTree(backup); }
	else {
		record = SteamInstalledRecord();
		if (!dirExists(backup)) DeleteFileW(path.c_str());
		DeleteFileW(pending.c_str());
	}
	return valid;
}
void steamFullInstallClearStaging(const std::wstring& localState, uint32_t appId, const SteamFullInstallLimits& limits) {
	std::wstring root = stagingRoot(localState, appId);
	if (!root.empty() && root.size() <= limits.fullPathChars) clearTree(root);
	DeleteFileW(progressPath(localState).c_str());
}

std::string steamFullInstallSerializeProgress(const SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits) {
	if (!validProgress(progress, limits)) return "";
	json::JsonValue root = json::JsonValue::makeObject();
	json::set(root, "version", number(1));
	json::set(root, "state", json::JsonValue::makeString("downloading"));
	json::set(root, "steamId", json::JsonValue::makeString(progress.steamId));
	json::set(root, "appId", number(progress.appId));
	json::set(root, "depotId", number(progress.depotId));
	json::set(root, "manifestId", number(progress.manifestId));
	json::set(root, "buildId", json::JsonValue::makeString(progress.buildId));
	json::set(root, "nextFile", number(progress.nextFile));
	json::set(root, "currentChunk", number(progress.currentChunk));
	json::set(root, "filesVerified", number(progress.filesVerified));
	json::set(root, "chunksVerified", number(progress.chunksVerified));
	json::set(root, "transferredBytes", number(progress.transferredBytes));
	json::set(root, "updatedAt", number(progress.updatedAt));
	std::string text = json::serialize(root);
	return text.size() <= limits.recordBytes ? text : "";
}

bool steamFullInstallDeserializeProgress(const std::string& text, const SteamFullInstallLimits& limits, SteamFullInstallProgress& progress) {
	progress = SteamFullInstallProgress();
	if (text.empty() || text.size() > limits.recordBytes) return false;
	json::JsonValue root;
	json::JsonParser parser(text);
	if (!parser.parse(root) || root.type != json::JsonType::Object || json::stringValue(root, "state") != "downloading") return false;
	uint64_t version = 0, nextFile = 0, currentChunk = 0;
	const auto* versionValue = member(root, "version");
	const auto* appId = member(root, "appId");
	const auto* depotId = member(root, "depotId");
	const auto* manifestId = member(root, "manifestId");
	const auto* nextFileValue = member(root, "nextFile");
	const auto* currentChunkValue = member(root, "currentChunk");
	const auto* filesVerified = member(root, "filesVerified");
	const auto* chunksVerified = member(root, "chunksVerified");
	const auto* transferredBytes = member(root, "transferredBytes");
	const auto* updatedAt = member(root, "updatedAt");
	if (!versionValue || !jsonUInt64(*versionValue, version) || version != 1 || !appId || !jsonUInt32(*appId, progress.appId) || !depotId || !jsonUInt32(*depotId, progress.depotId) || !manifestId || !jsonUInt64(*manifestId, progress.manifestId) || !nextFileValue || !jsonUInt64(*nextFileValue, nextFile) || nextFile > (std::numeric_limits<size_t>::max)() || !currentChunkValue || !jsonUInt64(*currentChunkValue, currentChunk) || currentChunk > (std::numeric_limits<size_t>::max)() || !filesVerified || !jsonUInt64(*filesVerified, progress.filesVerified) || !chunksVerified || !jsonUInt64(*chunksVerified, progress.chunksVerified) || !transferredBytes || !jsonUInt64(*transferredBytes, progress.transferredBytes) || !updatedAt || !jsonUInt64(*updatedAt, progress.updatedAt)) return false;
	progress.nextFile = (size_t)nextFile;
	progress.currentChunk = (size_t)currentChunk;
	progress.steamId = json::stringValue(root, "steamId");
	progress.buildId = json::stringValue(root, "buildId");
	return validProgress(progress, limits);
}

std::string steamFullInstallSerializeRecord(const SteamInstalledRecord& record, const SteamFullInstallLimits& limits) {
	if (!validRecord(record, limits)) return "";
	json::JsonValue root = json::JsonValue::makeObject();
	json::set(root, "version", number(1));
	json::set(root, "state", json::JsonValue::makeString("installed"));
	json::set(root, "steamId", json::JsonValue::makeString(record.steamId));
	json::set(root, "appId", number(record.appId));
	json::set(root, "depotId", number(record.depotId));
	json::set(root, "manifestId", number(record.manifestId));
	json::set(root, "buildId", json::JsonValue::makeString(record.buildId));
	json::set(root, "installDir", json::JsonValue::makeString(record.installDir));
	json::set(root, "executable", json::JsonValue::makeString(record.executable));
	json::set(root, "architecture", json::JsonValue::makeString(record.architecture));
	json::set(root, "installedBytes", number(record.installedBytes));
	json::set(root, "fileCount", number(record.fileCount));
	json::set(root, "chunkCount", number(record.chunkCount));
	json::set(root, "installedAt", number(record.installedAt));
	std::string text = json::serialize(root);
	return text.size() <= limits.recordBytes ? text : "";
}

