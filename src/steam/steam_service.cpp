#include "steam_service.h"
#include "steam_cm.h"
#include "steam_full_install.h"
#include "json_value.h"
#include <windows.h>
#include <winhttp.h>
#include <dpapi.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct InternetHandle {
	HINTERNET value = nullptr;
	~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};

SRWLOCK g_lock = SRWLOCK_INIT;
SteamStatus g_status;
std::wstring g_localState;
void (*g_log)(const char*) = nullptr;
void (*g_changed)(void) = nullptr;
std::string g_refreshToken;
std::string g_accessToken;
volatile LONG g_workerActive = 0;
volatile LONG g_cancel = 0;

void logLine(const char* text) {
	if (g_log && text) g_log(text);
}

bool httpFailure(const char* stage) {
	char line[192];
	sprintf_s(line, "steam: HTTPS %s failed err=%lu\n", stage, GetLastError());
	logLine(line);
	return false;
}

void clearSecret(std::string& value) {
	if (!value.empty()) SecureZeroMemory(&value[0], value.size());
	value.clear();
}

void clearWideSecret(std::wstring& value) {
	if (!value.empty()) SecureZeroMemory(&value[0], value.size() * sizeof(wchar_t));
	value.clear();
}

void clearJsonStrings(json::JsonValue& value) {
	if (value.type == json::JsonType::String) clearSecret(value.string);
	else if (value.type == json::JsonType::Array) for (auto& item : value.array) clearJsonStrings(item);
	else if (value.type == json::JsonType::Object) for (auto& member : value.object) clearJsonStrings(member.second);
}

struct SecretGuard {
	std::string* value;
	~SecretGuard() { if (value) clearSecret(*value); }
};

struct JsonSecretGuard {
	json::JsonValue* value;
	~JsonSecretGuard() { if (value) clearJsonStrings(*value); }
};

struct WideSecretGuard {
	std::wstring* value;
	~WideSecretGuard() { if (value) clearWideSecret(*value); }
};

struct MemoryGuard {
	void* data;
	size_t size;
	~MemoryGuard() { if (data && size) SecureZeroMemory(data, size); }
};

void setStatus(SteamState state, const std::string& message) {
	AcquireSRWLockExclusive(&g_lock);
	g_status.state = state;
	g_status.message = message;
	if (state != SteamState::Starting && state != SteamState::Linking) {
		g_status.challengeUrl.clear();
	}
	ReleaseSRWLockExclusive(&g_lock);
}

void setChallenge(const std::string& challenge, bool remoteInteraction) {
	AcquireSRWLockExclusive(&g_lock);
	g_status.state = SteamState::Linking;
	g_status.message = remoteInteraction ? "Approval opened in Steam Mobile" : "Scan with Steam Mobile and approve this device";
	g_status.challengeUrl = challenge;
	ReleaseSRWLockExclusive(&g_lock);
}

std::wstring steamDir() { return g_localState + L"\\steam"; }
std::wstring sessionPath() { return steamDir() + L"\\session.dat"; }
std::wstring catalogPath() { return steamDir() + L"\\catalog.json"; }

bool dirExists(const std::wstring& path) {
	DWORD attrs = GetFileAttributesW(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool ensureDir(const std::wstring& path) {
	if (path.empty()) return false;
	if (dirExists(path)) return true;
	size_t slash = path.find_last_of(L"\\/");
	if (slash != std::wstring::npos) {
		std::wstring parent = path.substr(0, slash);
		if (!parent.empty() && !dirExists(parent) && !ensureDir(parent)) return false;
	}
	return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool readFile(const std::wstring& path, std::vector<uint8_t>& data, size_t maxBytes) {
	data.clear();
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size = {};
	bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && (uint64_t)size.QuadPart <= maxBytes;
	if (ok) {
		data.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = data.empty() || (ReadFile(file, data.data(), (DWORD)data.size(), &read, nullptr) && read == data.size());
	}
	CloseHandle(file);
	if (!ok) data.clear();
	return ok;
}

bool readText(const std::wstring& path, std::string& text, size_t maxBytes) {
	std::vector<uint8_t> data;
	if (!readFile(path, data, maxBytes)) return false;
	text.assign(data.begin(), data.end());
	return true;
}

bool writeFile(const std::wstring& path, const void* data, size_t size) {
	HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	bool ok = size == 0 || (WriteFile(file, data, (DWORD)size, &written, nullptr) && written == size);
	if (ok) ok = FlushFileBuffers(file) != FALSE;
	CloseHandle(file);
	return ok;
}

bool writeAtomic(const std::wstring& path, const void* data, size_t size) {
	std::wstring temp = path + L".tmp";
	if (!writeFile(temp, data, size)) return false;
	if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(temp.c_str());
		return false;
	}
	return true;
}

bool protectData(const std::string& plaintext, std::vector<uint8_t>& protectedData) {
	protectedData.clear();
	DATA_BLOB input = {};
	input.pbData = (BYTE*)(plaintext.empty() ? nullptr : plaintext.data());
	input.cbData = (DWORD)plaintext.size();
	DATA_BLOB output = {};
	if (!CryptProtectData(&input, L"Stardew Valley Xbox Steam session", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
	protectedData.assign(output.pbData, output.pbData + output.cbData);
	if (output.pbData) {
		SecureZeroMemory(output.pbData, output.cbData);
		LocalFree(output.pbData);
	}
	return true;
}

bool unprotectData(const std::vector<uint8_t>& protectedData, std::string& plaintext) {
	plaintext.clear();
	DATA_BLOB input = {};
	input.pbData = (BYTE*)(protectedData.empty() ? nullptr : protectedData.data());
	input.cbData = (DWORD)protectedData.size();
	DATA_BLOB output = {};
	if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
	plaintext.assign((const char*)output.pbData, (size_t)output.cbData);
	if (output.pbData) {
		SecureZeroMemory(output.pbData, output.cbData);
		LocalFree(output.pbData);
	}
	return true;
}

std::string urlEncode(const std::string& value) {
	static const char* hex = "0123456789ABCDEF";
	std::string output;
	for (unsigned char ch : value) {
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') output += (char)ch;
		else {
			output += '%';
			output += hex[ch >> 4];
			output += hex[ch & 15];
		}
	}
	return output;
}

std::wstring widenUtf8(const std::string& value) {
	if (value.empty()) return L"";
	int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), nullptr, 0);
	if (length <= 0) return L"";
	std::wstring output((size_t)length, L'\0');
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), &output[0], length)) return L"";
	return output;
}

bool httpRequest(const wchar_t* method, const std::wstring& url, const std::string& contentType, const std::string& body, std::string& response, DWORD& status, DWORD& steamResult) {
	response.clear();
	status = 0;
	steamResult = 0;
	URL_COMPONENTS parts = {};
	parts.dwStructSize = sizeof(parts);
	wchar_t host[256] = {};
	wchar_t path[4096] = {};
	wchar_t extra[4096] = {};
	MemoryGuard pathGuard{ path, sizeof(path) };
	MemoryGuard extraGuard{ extra, sizeof(extra) };
	parts.lpszHostName = host;
	parts.dwHostNameLength = 255;
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = 4095;
	parts.lpszExtraInfo = extra;
	parts.dwExtraInfoLength = 4095;
	if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &parts)) return httpFailure("URL parse");
	if (parts.nScheme != INTERNET_SCHEME_HTTPS) { SetLastError(ERROR_INVALID_PARAMETER); return httpFailure("TLS requirement"); }
	std::wstring target(path, parts.dwUrlPathLength);
	WideSecretGuard targetGuard{ &target };
	if (parts.dwExtraInfoLength) target.append(extra, parts.dwExtraInfoLength);

	InternetHandle session, connect, request;
	session.value = WinHttpOpen(L"StardewValleyXbox/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session.value) return httpFailure("session open");
	WinHttpSetTimeouts(session.value, 10000, 10000, 15000, 15000);
	connect.value = WinHttpConnect(session.value, host, parts.nPort, 0);
	if (!connect.value) return httpFailure("connect");
	request.value = WinHttpOpenRequest(connect.value, method, target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!request.value) return httpFailure("request open");
	DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
	WinHttpSetOption(request.value, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
	std::wstring headers;
	if (!contentType.empty()) headers = L"Content-Type: " + widenUtf8(contentType) + L"\r\n";
	LPVOID bodyData = body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data();
	DWORD bodySize = (DWORD)body.size();
	if (!WinHttpSendRequest(request.value, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : (DWORD)-1L, bodyData, bodySize, bodySize, 0)) return httpFailure("send");
	if (!WinHttpReceiveResponse(request.value, nullptr)) return httpFailure("receive");
	DWORD length = sizeof(status);
	if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) return httpFailure("status query");
	wchar_t resultBuffer[32] = {};
	length = sizeof(resultBuffer);
	if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CUSTOM, L"X-EResult", resultBuffer, &length, WINHTTP_NO_HEADER_INDEX)) steamResult = (DWORD)wcstoul(resultBuffer, nullptr, 10);
	for (;;) {
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request.value, &available)) return httpFailure("response size");
		if (!available) break;
		if (response.size() + available > 32 * 1024 * 1024) { SetLastError(ERROR_FILE_TOO_LARGE); return httpFailure("response limit"); }
		size_t offset = response.size();
		response.resize(offset + available);
		DWORD read = 0;
		if (!WinHttpReadData(request.value, &response[offset], available, &read)) return httpFailure("response read");
		response.resize(offset + read);
		if (!read) break;
	}
	if (status < 200 || status >= 300 || (steamResult && steamResult != 1)) {
		char line[160];
		sprintf_s(line, "steam: HTTPS response rejected status=%lu eresult=%lu\n", status, steamResult);
		logLine(line);
		return false;
	}
	return true;
}

bool postInputJson(const std::wstring& endpoint, const json::JsonValue& input, std::string& response) {
	std::string body = "input_json=" + urlEncode(json::serialize(input));
	DWORD status = 0, result = 0;
	bool ok = httpRequest(L"POST", endpoint, "application/x-www-form-urlencoded", body, response, status, result);
	clearSecret(body);
	return ok;
}

bool getJson(const std::wstring& url, std::string& response) {
	DWORD status = 0, result = 0;
	return httpRequest(L"GET", url, "", "", response, status, result);
}

const json::JsonValue* responseObject(const json::JsonValue& root) {
	const json::JsonValue* response = json::get(root, "response");
	return response && response->type == json::JsonType::Object ? response : nullptr;
}

std::string scalarString(const json::JsonValue& object, const char* key) {
	const json::JsonValue* value = json::get(object, key);
	if (!value) return "";
	if (value->type == json::JsonType::String) return value->string;
	if (value->type == json::JsonType::Number) return value->number;
	return "";
}

bool booleanValue(const json::JsonValue& object, const char* key) {
	const json::JsonValue* value = json::get(object, key);
	return value && value->type == json::JsonType::Boolean && value->boolean;
}

bool decodeBase64Url(const std::string& input, std::string& output) {
	output.clear();
	uint32_t value = 0;
	int bits = -8;
	for (unsigned char ch : input) {
		if (ch == '=') break;
		int digit = -1;
		if (ch >= 'A' && ch <= 'Z') digit = ch - 'A';
		else if (ch >= 'a' && ch <= 'z') digit = ch - 'a' + 26;
		else if (ch >= '0' && ch <= '9') digit = ch - '0' + 52;
		else if (ch == '-' || ch == '+') digit = 62;
		else if (ch == '_' || ch == '/') digit = 63;
		if (digit < 0) return false;
		value = (value << 6) | (uint32_t)digit;
		bits += 6;
		if (bits >= 0) {
			output += (char)((value >> bits) & 0xff);
			bits -= 8;
		}
	}
	return !output.empty();
}

bool jwtPayload(const std::string& token, json::JsonValue& payload) {
	size_t first = token.find('.');
	if (first == std::string::npos) return false;
	size_t second = token.find('.', first + 1);
	if (second == std::string::npos) return false;
	std::string decoded;
	if (!decodeBase64Url(token.substr(first + 1, second - first - 1), decoded)) return false;
	json::JsonParser parser(decoded);
	return parser.parse(payload) && payload.type == json::JsonType::Object;
}

std::string jwtStringClaim(const std::string& token, const char* name) {
	json::JsonValue payload;
	return jwtPayload(token, payload) ? json::stringValue(payload, name) : "";
}

uint64_t jwtNumberClaim(const std::string& token, const char* name) {
	json::JsonValue payload;
	if (!jwtPayload(token, payload)) return 0;
	const json::JsonValue* value = json::get(payload, name);
	if (!value) return 0;
	std::string text = value->type == json::JsonType::Number ? value->number : value->type == json::JsonType::String ? value->string : "";
	return text.empty() ? 0 : _strtoui64(text.c_str(), nullptr, 10);
}

bool jwtAudienceHas(const std::string& token, const char* audience) {
	json::JsonValue payload;
	if (!jwtPayload(token, payload)) return false;
	const json::JsonValue* value = json::get(payload, "aud");
	if (!value) return false;
	if (value->type == json::JsonType::String) return value->string == audience;
	if (value->type != json::JsonType::Array) return false;
	for (const auto& item : value->array) if (item.type == json::JsonType::String && item.string == audience) return true;
	return false;
}

bool accessTokenUsable(const std::string& token) {
	if (token.empty() || !jwtAudienceHas(token, "web")) return false;
	uint64_t expires = jwtNumberClaim(token, "exp");
	return !expires || expires > (uint64_t)time(nullptr) + 60;
}

bool saveSession(const std::string& steamId, const std::string& accountName, const std::string& refreshToken, const std::string& accessToken) {
	json::JsonValue root = json::JsonValue::makeObject();
	json::set(root, "version", json::JsonValue::makeNumber(1));
	json::set(root, "steamId", json::JsonValue::makeString(steamId));
	json::set(root, "accountName", json::JsonValue::makeString(accountName));
	json::set(root, "refreshToken", json::JsonValue::makeString(refreshToken));
	json::set(root, "accessToken", json::JsonValue::makeString(accessToken));
	json::set(root, "savedAt", json::JsonValue::makeNumber((long long)time(nullptr)));
	std::string plaintext = json::serialize(root);
	std::vector<uint8_t> protectedData;
	bool ok = protectData(plaintext, protectedData) && ensureDir(steamDir()) && writeAtomic(sessionPath(), protectedData.data(), protectedData.size());
	clearJsonStrings(root);
	clearSecret(plaintext);
	if (!protectedData.empty()) SecureZeroMemory(protectedData.data(), protectedData.size());
	return ok;
}

bool loadSession(std::string& steamId, std::string& accountName, std::string& refreshToken, std::string& accessToken) {
	std::vector<uint8_t> protectedData;
	if (!readFile(sessionPath(), protectedData, 1024 * 1024)) return false;
	std::string plaintext;
	if (!unprotectData(protectedData, plaintext)) {
		if (!protectedData.empty()) SecureZeroMemory(protectedData.data(), protectedData.size());
		return false;
	}
	if (!protectedData.empty()) SecureZeroMemory(protectedData.data(), protectedData.size());
	json::JsonValue root;
	json::JsonParser parser(plaintext);
	bool ok = parser.parse(root) && root.type == json::JsonType::Object;
	if (ok) {
		steamId = json::stringValue(root, "steamId");
		accountName = json::stringValue(root, "accountName");
		refreshToken = json::stringValue(root, "refreshToken");
		accessToken = json::stringValue(root, "accessToken");
		ok = !steamId.empty() && !refreshToken.empty() && jwtStringClaim(refreshToken, "sub") == steamId;
	}
	clearJsonStrings(root);
	clearSecret(plaintext);
	return ok;
}

bool generateAccessToken(std::string& refreshToken, const std::string& steamId, std::string& accessToken) {
	json::JsonValue input = json::JsonValue::makeObject();
	JsonSecretGuard inputGuard{ &input };
	json::set(input, "refresh_token", json::JsonValue::makeString(refreshToken));
	json::set(input, "steamid", json::JsonValue::makeString(steamId));
	json::set(input, "renewal_type", json::JsonValue::makeNumber(0));
	std::string responseText;
	SecretGuard responseGuard{ &responseText };
	if (!postInputJson(L"https://api.steampowered.com/IAuthenticationService/GenerateAccessTokenForApp/v1/", input, responseText)) return false;
	json::JsonValue root;
	JsonSecretGuard rootGuard{ &root };
	json::JsonParser parser(responseText);
	if (!parser.parse(root)) return false;
	const json::JsonValue* response = responseObject(root);
	if (!response) return false;
	std::string replacementAccess = json::stringValue(*response, "access_token");
	std::string replacementRefresh = json::stringValue(*response, "refresh_token");
	SecretGuard accessGuard{ &replacementAccess };
	SecretGuard refreshGuard{ &replacementRefresh };
	if (!accessTokenUsable(replacementAccess)) return false;
	accessToken = std::move(replacementAccess);
	if (!replacementRefresh.empty()) {
		clearSecret(refreshToken);
		refreshToken = std::move(replacementRefresh);
	}
	return true;
}

bool saveCatalog(const std::string& steamId, const json::JsonValue& games) {
	json::JsonValue root = json::JsonValue::makeObject();
	json::set(root, "version", json::JsonValue::makeNumber(1));
	json::set(root, "steamId", json::JsonValue::makeString(steamId));
	json::set(root, "syncedAt", json::JsonValue::makeNumber((long long)time(nullptr)));
	json::set(root, "games", games);
	std::string text = json::serialize(root);
	return ensureDir(steamDir()) && writeAtomic(catalogPath(), text.data(), text.size());
}

bool fetchOwnedGames(const std::string& accessToken, const std::string& steamId, json::JsonValue& games) {
	json::JsonValue input = json::JsonValue::makeObject();
	JsonSecretGuard inputGuard{ &input };
	json::set(input, "steamid", json::JsonValue::makeString(steamId));
	json::set(input, "include_appinfo", json::JsonValue::makeBoolean(true));
	json::set(input, "include_played_free_games", json::JsonValue::makeBoolean(true));
	std::string query = "access_token=" + urlEncode(accessToken) + "&input_json=" + urlEncode(json::serialize(input));
	SecretGuard queryGuard{ &query };
	std::wstring url = L"https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/?" + widenUtf8(query);
	WideSecretGuard urlGuard{ &url };
	std::string responseText;
	SecretGuard responseGuard{ &responseText };
	if (!getJson(url, responseText)) return false;
	json::JsonValue root;
	json::JsonParser parser(responseText);
	if (!parser.parse(root)) return false;
	const json::JsonValue* response = responseObject(root);
	if (!response) return false;
	const json::JsonValue* remoteGames = json::get(*response, "games");
	if (!remoteGames) {
		const json::JsonValue* count = json::get(*response, "game_count");
		if (count && scalarString(*response, "game_count") == "0") {
			games = json::JsonValue::makeArray();
			return true;
		}
		return false;
	}
	if (remoteGames->type != json::JsonType::Array) return false;
	games = *remoteGames;
	return true;
}

bool syncCatalogInternal() {
	std::string steamId, accountName, refreshToken, accessToken;
	SecretGuard refreshGuard{ &refreshToken };
	SecretGuard accessGuard{ &accessToken };
	AcquireSRWLockShared(&g_lock);
	steamId = g_status.steamId;
	accountName = g_status.accountName;
	refreshToken = g_refreshToken;
	accessToken = g_accessToken;
	ReleaseSRWLockShared(&g_lock);
	if (steamId.empty() || refreshToken.empty()) return false;
	setStatus(SteamState::Syncing, "Refreshing the owned-games catalog");
	if (!accessTokenUsable(accessToken)) {
		if (!generateAccessToken(refreshToken, steamId, accessToken)) {
			clearSecret(refreshToken);
			clearSecret(accessToken);
			setStatus(SteamState::RelinkRequired, "Steam requires a new QR approval");
			return false;
		}
	}
	json::JsonValue games;
	if (!fetchOwnedGames(accessToken, steamId, games)) {
		clearSecret(refreshToken);
		clearSecret(accessToken);
		setStatus(SteamState::Linked, "Linked; owned-games sync was unavailable");
		return false;
	}
	if (!saveCatalog(steamId, games)) logLine("steam: owned-games catalog could not be saved\n");
	AcquireSRWLockExclusive(&g_lock);
	clearSecret(g_refreshToken);
	clearSecret(g_accessToken);
	g_refreshToken = refreshToken;
	g_accessToken = accessToken;
	g_status.state = SteamState::Linked;
	g_status.message = "Steam library synchronized";
	ReleaseSRWLockExclusive(&g_lock);
	if (!saveSession(steamId, accountName, refreshToken, accessToken)) logLine("steam: updated session could not be persisted\n");
	if (g_changed) g_changed();
	clearSecret(refreshToken);
	clearSecret(accessToken);
	return true;
}

bool beginQrSession(std::string& clientId, std::string& requestId, std::string& challengeUrl, double& interval) {
	json::JsonValue details = json::JsonValue::makeObject();
	json::set(details, "device_friendly_name", json::JsonValue::makeString("Stardew Valley Xbox"));
	json::set(details, "platform_type", json::JsonValue::makeNumber(1));
	json::set(details, "os_type", json::JsonValue::makeNumber(-1));
	json::JsonValue input = json::JsonValue::makeObject();
	json::set(input, "device_details", std::move(details));
	json::set(input, "website_id", json::JsonValue::makeString("Client"));
	std::string responseText;
	SecretGuard responseGuard{ &responseText };
	if (!postInputJson(L"https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaQR/v1/", input, responseText)) return false;
	json::JsonValue root;
	JsonSecretGuard rootGuard{ &root };
	json::JsonParser parser(responseText);
	if (!parser.parse(root)) return false;
	const json::JsonValue* response = responseObject(root);
	if (!response) return false;
	clientId = scalarString(*response, "client_id");
	requestId = json::stringValue(*response, "request_id");
	challengeUrl = json::stringValue(*response, "challenge_url");
	std::string intervalText = scalarString(*response, "interval");
	interval = intervalText.empty() ? 5.0 : strtod(intervalText.c_str(), nullptr);
	if (interval < 2.0) interval = 2.0;
	if (interval > 15.0) interval = 15.0;
	return !clientId.empty() && !requestId.empty() && !challengeUrl.empty();
}

bool pollQrSession(const std::string& clientId, const std::string& requestId, json::JsonValue& response) {
	json::JsonValue input = json::JsonValue::makeObject();
	JsonSecretGuard inputGuard{ &input };
	json::set(input, "client_id", json::JsonValue::makeString(clientId));
	json::set(input, "request_id", json::JsonValue::makeString(requestId));
	std::string responseText;
	SecretGuard responseGuard{ &responseText };
	if (!postInputJson(L"https://api.steampowered.com/IAuthenticationService/PollAuthSessionStatus/v1/", input, responseText)) return false;
	json::JsonValue root;
	JsonSecretGuard rootGuard{ &root };
	json::JsonParser parser(responseText);
	if (!parser.parse(root)) return false;
	const json::JsonValue* object = responseObject(root);
	if (!object) return false;
	response = *object;
	return true;
}

DWORD WINAPI linkWorker(LPVOID) {
	std::string clientId, requestId, challengeUrl;
	SecretGuard requestGuard{ &requestId };
	double interval = 5.0;
	if (!beginQrSession(clientId, requestId, challengeUrl, interval)) {
		setStatus(SteamState::Error, "Steam QR session could not be started");
		InterlockedExchange(&g_workerActive, 0);
		return 1;
	}
	setChallenge(challengeUrl, false);
	ULONGLONG deadline = GetTickCount64() + 5 * 60 * 1000;
	while (!InterlockedCompareExchange(&g_cancel, 0, 0) && GetTickCount64() < deadline) {
		Sleep((DWORD)(interval * 1000.0));
		if (InterlockedCompareExchange(&g_cancel, 0, 0)) break;
		json::JsonValue response;
		JsonSecretGuard responseGuard{ &response };
		if (!pollQrSession(clientId, requestId, response)) {
			setStatus(SteamState::Error, "Steam QR polling failed");
			InterlockedExchange(&g_workerActive, 0);
			return 1;
		}
		if (InterlockedCompareExchange(&g_cancel, 0, 0)) break;
		std::string replacementClient = scalarString(response, "new_client_id");
		std::string replacementChallenge = json::stringValue(response, "new_challenge_url");
		if (!replacementClient.empty()) clientId = replacementClient;
		if (!replacementChallenge.empty()) challengeUrl = replacementChallenge;
		bool remoteInteraction = booleanValue(response, "had_remote_interaction");
		setChallenge(challengeUrl, remoteInteraction);
		std::string refreshToken = json::stringValue(response, "refresh_token");
		std::string accessToken = json::stringValue(response, "access_token");
		if (refreshToken.empty()) continue;
		std::string steamId = jwtStringClaim(refreshToken, "sub");
		std::string accountName = json::stringValue(response, "account_name");
		if (steamId.empty() || accountName.empty() || !jwtAudienceHas(refreshToken, "derive") || !jwtAudienceHas(refreshToken, "client")) {
			clearSecret(refreshToken);
			clearSecret(accessToken);
			setStatus(SteamState::Error, "Steam returned an invalid session");
			InterlockedExchange(&g_workerActive, 0);
			return 1;
		}
		if (!saveSession(steamId, accountName, refreshToken, accessToken)) {
			clearSecret(refreshToken);
			clearSecret(accessToken);
			setStatus(SteamState::Error, "Protected Steam session storage failed");
			InterlockedExchange(&g_workerActive, 0);
			return 1;
		}
		AcquireSRWLockExclusive(&g_lock);
		clearSecret(g_refreshToken);
		clearSecret(g_accessToken);
		g_refreshToken = refreshToken;
		g_accessToken = accessToken;
		g_status.state = SteamState::Linked;
		g_status.message = "Steam account linked";
		g_status.challengeUrl.clear();
		g_status.steamId = steamId;
		g_status.accountName = accountName;
		ReleaseSRWLockExclusive(&g_lock);
		logLine("steam: account linked; session stored with user-scoped DPAPI\n");
		syncCatalogInternal();
		std::string cmToken;
		SecretGuard cmGuard{ &cmToken };
		AcquireSRWLockShared(&g_lock);
		cmToken = g_refreshToken;
		ReleaseSRWLockShared(&g_lock);
		if (!cmToken.empty()) steamCmStart(cmToken, steamId, accountName);
		clearSecret(refreshToken);
		clearSecret(accessToken);
		InterlockedExchange(&g_workerActive, 0);
		return 0;
	}
	if (InterlockedCompareExchange(&g_cancel, 0, 0)) {
		AcquireSRWLockShared(&g_lock);
		bool hadSession = !g_refreshToken.empty();
		ReleaseSRWLockShared(&g_lock);
		setStatus(hadSession ? SteamState::RelinkRequired : SteamState::SignedOut, hadSession ? "Steam relink cancelled" : "Steam linking cancelled");
	} else setStatus(SteamState::Error, "Steam QR session expired");
	AcquireSRWLockExclusive(&g_lock);
	g_status.challengeUrl.clear();
	ReleaseSRWLockExclusive(&g_lock);
	InterlockedExchange(&g_workerActive, 0);
	return 0;
}

}

void steamInit(const wchar_t* localState, void (*log)(const char*), void (*onChanged)(void)) {
	g_localState = localState ? localState : L"";
	g_log = log;
	g_changed = onChanged;
	steamCmInit(g_localState.c_str(), log, onChanged);
	std::vector<uint8_t> probe;
	std::string probeText = "stardew-steam-storage-probe";
	std::string unprotected;
	if (!protectData(probeText, probe) || !unprotectData(probe, unprotected) || unprotected != probeText) {
		setStatus(SteamState::Error, "User-scoped protected storage is unavailable");
		return;
	}
	if (!probe.empty()) SecureZeroMemory(probe.data(), probe.size());
	clearSecret(unprotected);
	std::string steamId, accountName, refreshToken, accessToken;
	SecretGuard refreshGuard{ &refreshToken };
	SecretGuard accessGuard{ &accessToken };
	if (!loadSession(steamId, accountName, refreshToken, accessToken)) {
		setStatus(SteamState::SignedOut, "No Steam account linked");
		logLine("steam: protected storage ready; no linked account\n");
		return;
	}
	bool clientScoped = !accountName.empty() && jwtAudienceHas(refreshToken, "client");
	AcquireSRWLockExclusive(&g_lock);
	g_refreshToken = refreshToken;
	g_accessToken = accessToken;
	g_status.state = clientScoped ? SteamState::Linked : SteamState::RelinkRequired;
	g_status.message = clientScoped ? (accessTokenUsable(accessToken) ? "Steam account linked" : "Steam CM session restored; web catalog cached") : "Relink Steam once to enable package licenses";
	g_status.steamId = steamId;
	g_status.accountName = accountName;
	ReleaseSRWLockExclusive(&g_lock);
	if (clientScoped) steamCmStart(refreshToken, steamId, accountName);
	else steamCmRequireRelink();
	clearSecret(refreshToken);
	clearSecret(accessToken);
	logLine("steam: protected session restored\n");
}

SteamStatus steamStatus() {
	AcquireSRWLockShared(&g_lock);
	SteamStatus copy = g_status;
	ReleaseSRWLockShared(&g_lock);
	return copy;
}

bool steamBeginLink() {
	if (InterlockedCompareExchange(&g_workerActive, 1, 0) != 0) return false;
	InterlockedExchange(&g_cancel, 0);
	AcquireSRWLockExclusive(&g_lock);
	g_status.state = SteamState::Starting;
	g_status.message = "Starting Steam QR session";
	g_status.challengeUrl.clear();
	ReleaseSRWLockExclusive(&g_lock);
	HANDLE thread = CreateThread(nullptr, 0, linkWorker, nullptr, 0, nullptr);
	if (!thread) {
		InterlockedExchange(&g_workerActive, 0);
		setStatus(SteamState::Error, "Steam link worker could not start");
		return false;
	}
	CloseHandle(thread);
	return true;
}

void steamCancelLink() {
	InterlockedExchange(&g_cancel, 1);
}

void steamSignOut() {
	steamCancelLink();
	for (int i = 0; i < 1200 && InterlockedCompareExchange(&g_workerActive, 0, 0); i++) Sleep(50);
	steamCmForget();
	AcquireSRWLockExclusive(&g_lock);
	clearSecret(g_refreshToken);
	clearSecret(g_accessToken);
	g_status = SteamStatus();
	g_status.state = SteamState::SignedOut;
	g_status.message = "Steam account signed out locally";
	ReleaseSRWLockExclusive(&g_lock);
	DeleteFileW(sessionPath().c_str());
	DeleteFileW(catalogPath().c_str());
	logLine("steam: local session and catalog removed\n");
}
