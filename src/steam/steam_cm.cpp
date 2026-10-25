#include "steam_cm.h"
#include "steam_pics.h"
#include "steam_appinfo.h"
#include "steam_content_auth.h"
#include "steam_install_queue.h"
#include "steam_manifest.h"
#include "steam_chunk.h"
#include "steam_full_install.h"
#include "steam_service.h"
#include "net/http.h"
#include "json_value.h"
#include "third_party/miniz/miniz_tinfl.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kProtoMask = 0x80000000u;
constexpr uint32_t kMsgMulti = 1;
constexpr uint32_t kMsgServiceMethodResponse = 147;
constexpr uint32_t kMsgServiceMethodCallFromClient = 151;
constexpr uint32_t kMsgHeartBeat = 703;
constexpr uint32_t kMsgLogOnResponse = 751;
constexpr uint32_t kMsgLoggedOff = 757;
constexpr uint32_t kMsgLicenseList = 780;
constexpr uint32_t kMsgDepotKeyRequest = 5438;
constexpr uint32_t kMsgDepotKeyResponse = 5439;
constexpr uint32_t kMsgLogon = 5514;
constexpr uint32_t kMsgPicsProductInfoRequest = 8903;
constexpr uint32_t kMsgPicsProductInfoResponse = 8904;
constexpr uint32_t kMsgPicsAccessTokenRequest = 8905;
constexpr uint32_t kMsgPicsAccessTokenResponse = 8906;
constexpr uint32_t kMsgHello = 9805;
constexpr uint32_t kProtocolVersion = 65581;
constexpr size_t kPicsBatchSize = 200;
constexpr size_t kAppPicsBatchSize = 25;
constexpr ULONGLONG kPicsRequestTimeoutMs = 45000;
constexpr uint64_t kClientSuppliedSteamId = 76561197960265728ull;

struct LicenseRecord {
	uint32_t packageId = 0;
	uint32_t flags = 0;
	uint32_t licenseType = 0;
	uint32_t ownerId = 0;
	uint64_t accessToken = 0;
	uint32_t masterPackageId = 0;
};

struct LicenseVectorGuard {
	std::vector<LicenseRecord>* value;
	~LicenseVectorGuard() {
		if (!value) return;
		for (auto& license : *value) SecureZeroMemory(&license.accessToken, sizeof(license.accessToken));
	}
};

struct WorkerArgs {
	std::string refreshToken;
	std::string steamId;
	std::string accountName;
};

struct WebSocket {
	HINTERNET session = nullptr;
	HINTERNET connect = nullptr;
	HINTERNET request = nullptr;
	HINTERNET socket = nullptr;
	~WebSocket() {
		if (socket) {
			WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
			WinHttpCloseHandle(socket);
		}
		if (request) WinHttpCloseHandle(request);
		if (connect) WinHttpCloseHandle(connect);
		if (session) WinHttpCloseHandle(session);
	}
};

struct Packet {
	uint32_t message = 0;
	uint64_t steamId = 0;
	int32_t sessionId = 0;
	uint64_t sourceJobId = 0;
	uint64_t targetJobId = 0;
	int32_t result = 2;
	std::string targetJobName;
	const uint8_t* body = nullptr;
	size_t bodySize = 0;
};

enum class PicsStage {
	Idle,
	NeedPackageTokenRequest,
	WaitingForPackageTokens,
	NeedPackageProductRequest,
	WaitingForPackageProducts,
	NeedAppTokenRequest,
	WaitingForAppTokens,
	NeedAppProductRequest,
	WaitingForAppProducts,
	Complete,
};

enum class PlanStage {
	Idle,
	NeedServers,
	WaitingForServers,
	NeedDepotKey,
	WaitingForDepotKey,
	NeedManifestCode,
	WaitingForManifestCode,
	RetrieveManifest,
	RetrieveInstall,
	Complete,
	Error,
};

struct ConnectionState {
	uint64_t steamId = 0;
	int32_t sessionId = 0;
	uint32_t cellId = 0;
	int heartbeatSeconds = 30;
	ULONGLONG nextHeartbeat = 0;
	ULONGLONG picsDeadline = 0;
	ULONGLONG planDeadline = 0;
	uint64_t nextJobId = 1;
	uint64_t tokenJobId = 0;
	uint64_t productJobId = 0;
	bool loggedOn = false;
	bool licensesReceived = false;
	PicsStage picsStage = PicsStage::Idle;
	size_t picsBatchOffset = 0;
	std::vector<LicenseRecord> licenses;
	std::vector<uint32_t> batchPackageIds;
	std::map<uint32_t, uint64_t> batchTokens;
	std::set<uint32_t> batchSeen;
	std::vector<SteamPicsPackageInfo> resolvedPackages;
	std::set<uint32_t> deniedPackages;
	std::set<uint32_t> unknownPackages;
	std::set<uint32_t> missingTokenPackages;
	std::set<uint32_t> invalidPackages;
	size_t appBatchOffset = 0;
	std::vector<uint32_t> catalogAppIds;
	std::vector<uint32_t> batchAppIds;
	std::map<uint32_t, uint64_t> batchAppTokens;
	std::set<uint32_t> batchAppSeen;
	std::vector<SteamAppMetadata> resolvedApps;
	std::set<uint32_t> deniedApps;
	std::set<uint32_t> unknownApps;
	std::set<uint32_t> missingTokenApps;
	std::set<uint32_t> invalidApps;
	std::string appHttpHost;
	PlanStage planStage = PlanStage::Idle;
	uint64_t planJobId = 0;
	size_t planDepotIndex = 0;
	SteamInstallPlan plan;
	std::vector<SteamContentServer> planServers;
	std::vector<uint8_t> manifestDepotKey;
	uint64_t manifestRequestCode = 0;
	bool installAttempted = false;
	bool installSucceeded = false;
	std::vector<SteamManifestMetadata> installManifests;
	std::map<uint32_t, std::vector<uint8_t>> installDepotKeys;
	SteamManifestMetadata installMetadata;
	SteamFullInstallLimits installLimits;
	~ConnectionState() {
		for (auto& license : licenses) SecureZeroMemory(&license.accessToken, sizeof(license.accessToken));
		for (auto& token : batchTokens) SecureZeroMemory(&token.second, sizeof(token.second));
		for (auto& token : batchAppTokens) SecureZeroMemory(&token.second, sizeof(token.second));
		if (!manifestDepotKey.empty()) SecureZeroMemory(manifestDepotKey.data(), manifestDepotKey.size());
		for (auto& key : installDepotKeys) if (!key.second.empty()) SecureZeroMemory(key.second.data(), key.second.size());
		SecureZeroMemory(&manifestRequestCode, sizeof(manifestRequestCode));
	}
};

SRWLOCK g_lock = SRWLOCK_INIT;
SteamCmStatus g_status;
std::wstring g_localState;
void (*g_log)(const char*) = nullptr;
void (*g_changed)(void) = nullptr;
std::string g_entitlementSteamId;
std::set<uint32_t> g_entitledApps;
std::set<uint32_t> g_entitledDepots;
std::string g_appMetadataSteamId;
std::map<uint32_t, SteamAppMetadata> g_appMetadata;
std::string g_installPlanSteamId;
SteamInstallPlan g_installPlan;
SteamManifestMetadata g_manifestMetadata;
SteamManifestStatus g_manifestStatus;
std::map<uint32_t, SteamInstalledRecord> g_installedRecords;
std::map<uint32_t, SteamInstallStatus> g_installStatuses;
uint32_t g_pendingPlanAppId = 0;
ULONGLONG g_pendingPlanQueuedAt = 0;
volatile LONG g_stop = 0;
volatile LONG g_workerActive = 0;
volatile LONG g_cancelInstallAppId = 0;

bool installCancelRequested(uint32_t appId) {
	return appId && (uint32_t)InterlockedCompareExchange(&g_cancelInstallAppId, 0, 0) == appId;
}

void clearInstallCancel(uint32_t appId) {
	if (appId) InterlockedCompareExchange(&g_cancelInstallAppId, 0, (LONG)appId);
}

constexpr bool installBatchNeedsCheckpoint(size_t completed, size_t total, bool cancelled) {
	return cancelled || completed < total;
}
static_assert(installBatchNeedsCheckpoint(4, 8, false) && installBatchNeedsCheckpoint(8, 8, true) && !installBatchNeedsCheckpoint(8, 8, false), "install checkpoint policy");

void logLine(const char* text) {
	if (g_log && text) g_log(text);
}

void clearSecret(std::string& value) {
	if (!value.empty()) SecureZeroMemory(&value[0], value.size());
	value.clear();
}

struct ByteVectorGuard {
	std::vector<uint8_t>* value;
	~ByteVectorGuard() { if (value && !value->empty()) SecureZeroMemory(value->data(), value->size()); }
};

void setStatus(SteamCmState state, const std::string& message, int licenseCount = -1, int resolvedPackageCount = -1, int entitlementAppCount = -1, int appMetadataCount = -1) {
	AcquireSRWLockExclusive(&g_lock);
	g_status.state = state;
	g_status.message = message;
	if (licenseCount >= 0) g_status.licenseCount = licenseCount;
	if (resolvedPackageCount >= 0) g_status.resolvedPackageCount = resolvedPackageCount;
	if (entitlementAppCount >= 0) g_status.entitlementAppCount = entitlementAppCount;
	if (appMetadataCount >= 0) g_status.appMetadataCount = appMetadataCount;
	ReleaseSRWLockExclusive(&g_lock);
}

SteamCmState currentState() {
	AcquireSRWLockShared(&g_lock);
	SteamCmState state = g_status.state;
	ReleaseSRWLockShared(&g_lock);
	return state;
}

std::wstring steamDir() { return g_localState + L"\\steam"; }
std::wstring licensesPath() { return steamDir() + L"\\licenses.json"; }
std::wstring entitlementsPath() { return steamDir() + L"\\entitlements.json"; }
std::wstring catalogPath() { return steamDir() + L"\\catalog.json"; }
std::wstring appInfoPath() { return steamDir() + L"\\appinfo.json"; }
std::wstring installPlanPath() { return steamDir() + L"\\install-plan.json"; }
std::wstring manifestMetadataPath() { return steamDir() + L"\\manifest-metadata.json"; }

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

std::string readText(const std::wstring& path, size_t limit) {
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return "";
	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || (uint64_t)size.QuadPart > limit) { CloseHandle(file); return ""; }
	std::string text((size_t)size.QuadPart, '\0');
	DWORD read = 0;
	bool ok = text.empty() || (ReadFile(file, &text[0], (DWORD)text.size(), &read, nullptr) && read == text.size());
	CloseHandle(file);
	if (!ok) return "";
	return text;
}

size_t manifestChunkCount(const SteamManifestMetadata& metadata) {
	size_t count = 0;
	for (const auto& file : metadata.files) count += file.chunks.size();
	return count;
}

SteamManifestStatus manifestStatusFor(const SteamManifestMetadata& metadata, const char* state, const char* message) {
	SteamManifestStatus status;
	status.state = state;
	status.message = message;
	status.appId = metadata.appId;
	status.depotId = metadata.depotId;
	status.manifestId = metadata.manifestId;
	status.buildId = metadata.buildId;
	status.retrievedAt = metadata.retrievedAt;
	status.originalBytes = metadata.originalBytes;
	status.compressedBytes = metadata.compressedBytes;
	status.fileCount = metadata.files.size();
	status.chunkCount = manifestChunkCount(metadata);
	return status;
}

bool manifestMatchesCatalog(const SteamManifestMetadata& manifest, const std::string& steamId, const std::map<uint32_t, SteamAppMetadata>& metadata) {
	if (manifest.steamId != steamId) return false;
	auto app = metadata.find(manifest.appId);
	if (app == metadata.end() || app->second.publicBuildId != manifest.buildId) return false;
	for (const auto& depot : app->second.publicDepots) {
		if (!depot.sharedInstall && !depot.depotFromApp && depot.depotId == manifest.depotId && depot.manifestId == manifest.manifestId) return true;
	}
	return false;
}

void loadCachedManifest(const std::string& steamId, const std::map<uint32_t, SteamAppMetadata>& appMetadata, SteamManifestMetadata& metadata, SteamManifestStatus& status) {
	metadata = SteamManifestMetadata();
	status = SteamManifestStatus();
	std::string text = readText(manifestMetadataPath(), steamManifestDefaultLimits().cacheBytes);
	if (text.empty()) return;
	SteamManifestMetadata checked;
	if (!steamManifestDeserialize(text, steamManifestDefaultLimits(), checked)) {
		status.state = "invalid";
		status.message = "Manifest invalid: cached metadata failed validation";
		return;
	}
	status = manifestStatusFor(checked, "expired", "Manifest expired: cached identity or age is stale");
	uint64_t now = (uint64_t)time(nullptr);
	if (!manifestMatchesCatalog(checked, steamId, appMetadata) || steamManifestClassify(checked, steamId, checked.appId, checked.depotId, checked.manifestId, checked.buildId, now, steamManifestFreshnessSeconds()) != SteamManifestCacheState::Ready) return;
	metadata = std::move(checked);
	status = manifestStatusFor(metadata, "ready", "Manifest ready from validated cache");
}

bool saveManifestMetadata(const SteamManifestMetadata& metadata) {
	std::string text = steamManifestSerialize(metadata, steamManifestDefaultLimits());
	if (text.empty()) return false;
	SteamManifestMetadata checked;
	if (!steamManifestDeserialize(text, steamManifestDefaultLimits(), checked)) return false;
	if (steamManifestClassify(checked, metadata.steamId, metadata.appId, metadata.depotId, metadata.manifestId, metadata.buildId, metadata.retrievedAt, steamManifestFreshnessSeconds()) != SteamManifestCacheState::Ready) return false;
	return ensureDir(steamDir()) && writeAtomic(manifestMetadataPath(), text.data(), text.size());
}

SteamInstallStatus installStatusFor(const SteamInstalledRecord& record, const char* state, const char* message) {
	SteamInstallStatus status;
	status.state = state;
	status.message = message;
	status.appId = record.appId;
	status.depotId = record.depotId;
	status.manifestId = record.manifestId;
	status.buildId = record.buildId;
	status.installedBytes = record.installedBytes;
	status.totalBytes = record.installedBytes;
	status.filesVerified = record.fileCount;
	status.totalFiles = record.fileCount;
	status.chunksVerified = record.chunkCount;
	status.totalChunks = record.chunkCount;
	status.executable = record.executable;
	status.architecture = record.architecture;
	return status;
}

void publishInstallStatus(const SteamInstallStatus& status, const SteamInstalledRecord* record) {
	AcquireSRWLockExclusive(&g_lock);
	g_installStatuses[status.appId] = status;
	if (record) g_installedRecords[record->appId] = *record;
	ReleaseSRWLockExclusive(&g_lock);
}
void clearManifestAuthorization(ConnectionState& state) {
	if (!state.manifestDepotKey.empty()) SecureZeroMemory(state.manifestDepotKey.data(), state.manifestDepotKey.size());
	state.manifestDepotKey.clear();
	SecureZeroMemory(&state.manifestRequestCode, sizeof(state.manifestRequestCode));
	state.manifestRequestCode = 0;

}

void clearInstallDepotKeys(ConnectionState& state) {
	for (auto& key : state.installDepotKeys) if (!key.second.empty()) SecureZeroMemory(key.second.data(), key.second.size());
	state.installDepotKeys.clear();
}

void publishManifestResult(const SteamManifestStatus& status, const SteamManifestMetadata* metadata) {
	AcquireSRWLockExclusive(&g_lock);
	g_manifestStatus = status;
	g_manifestMetadata = metadata ? *metadata : SteamManifestMetadata();
	ReleaseSRWLockExclusive(&g_lock);
	if (g_changed) g_changed();
}

int loadCachedLicenseCount() {
	std::string text = readText(licensesPath(), 16 * 1024 * 1024);
	if (text.empty()) return 0;
	json::JsonValue root;
	json::JsonParser parser(text);
	if (!parser.parse(root)) return 0;
	const auto* licenses = json::get(root, "licenses");
	return licenses && licenses->type == json::JsonType::Array ? (int)licenses->array.size() : 0;
}

bool jsonUInt(const json::JsonValue& value, uint32_t& result) {
	if (value.type != json::JsonType::Number || value.number.empty()) return false;
	char* end = nullptr;
	unsigned long long parsed = _strtoui64(value.number.c_str(), &end, 10);
	if (!end || *end || parsed > 0xffffffffull) return false;
	result = (uint32_t)parsed;
	return true;
}

bool jsonUInt64(const json::JsonValue& value, uint64_t& result) {
	if (value.type != json::JsonType::Number || value.number.empty()) return false;
	char* end = nullptr;
	unsigned long long parsed = _strtoui64(value.number.c_str(), &end, 10);
	if (!end || *end) return false;
	result = (uint64_t)parsed;
	return true;
}

bool jsonBool(const json::JsonValue& value, const char* key, bool fallback = false) {
	const auto* member = json::get(value, key);
	return member && member->type == json::JsonType::Boolean ? member->boolean : fallback;
}

void loadCachedEntitlements(std::string& steamId, std::set<uint32_t>& apps, std::set<uint32_t>& depots, int& packageCount) {
	steamId.clear();
	apps.clear();
	depots.clear();
	packageCount = 0;
	std::string text = readText(entitlementsPath(), 32 * 1024 * 1024);
	if (text.empty()) return;
	json::JsonValue root;
	json::JsonParser parser(text);
	if (!parser.parse(root) || root.type != json::JsonType::Object) return;
	std::string checkedSteamId = json::stringValue(root, "steamId");
	int checkedPackageCount = json::intValue(root, "resolvedPackageCount", 0);
	const auto* appList = json::get(root, "apps");
	const auto* packageList = json::get(root, "packages");
	if (checkedSteamId.empty() || checkedPackageCount <= 0 || !appList || appList->type != json::JsonType::Array || appList->array.size() > 100000 || !packageList || packageList->type != json::JsonType::Array || packageList->array.size() > 100000) return;
	std::set<uint32_t> checkedApps;
	std::set<uint32_t> checkedDepots;
	for (const auto& item : appList->array) {
		if (item.type != json::JsonType::Object) return;
		const auto* appId = json::get(item, "appId");
		uint32_t value = 0;
		if (!appId || !jsonUInt(*appId, value) || !value) return;
		checkedApps.insert(value);
	}
	for (const auto& item : packageList->array) {
		if (item.type != json::JsonType::Object) return;
		const auto* depotIds = json::get(item, "depotIds");
		if (!depotIds || depotIds->type != json::JsonType::Array) return;
		for (const auto& depotId : depotIds->array) {
			uint32_t value = 0;
			if (!jsonUInt(depotId, value) || !value) return;
			checkedDepots.insert(value);
			if (checkedDepots.size() > 100000) return;
		}
	}
	if (checkedApps.empty() || checkedDepots.empty()) return;
	steamId = std::move(checkedSteamId);
	apps = std::move(checkedApps);
	depots = std::move(checkedDepots);
	packageCount = checkedPackageCount;
}

bool readCatalogAppIds(const std::string& steamId, std::vector<uint32_t>& appIds) {
	appIds.clear();
	std::string text = readText(catalogPath(), 32 * 1024 * 1024);
	if (text.empty()) return false;
	json::JsonValue root;
	json::JsonParser parser(text);
	if (!parser.parse(root) || root.type != json::JsonType::Object) return false;
	if (json::stringValue(root, "steamId") != steamId) return false;
	const auto* games = json::get(root, "games");
	if (!games || games->type != json::JsonType::Array) return false;
	std::set<uint32_t> unique;
	for (const auto& game : games->array) {
		if (game.type != json::JsonType::Object) continue;
		const auto* appId = json::get(game, "appid");
		uint32_t value = 0;
		if (appId && jsonUInt(*appId, value) && value) unique.insert(value);
	}
	appIds.assign(unique.begin(), unique.end());
	return true;
}

bool loadAppMetadataItem(const json::JsonValue& item, SteamAppMetadata& metadata) {
	if (item.type != json::JsonType::Object) return false;
	const auto* appId = json::get(item, "appId");
	if (!appId || !jsonUInt(*appId, metadata.appId) || !metadata.appId) return false;
	const auto* changeNumber = json::get(item, "changeNumber");
	const auto* declaredSize = json::get(item, "declaredSize");
	if (changeNumber) jsonUInt(*changeNumber, metadata.changeNumber);
	if (declaredSize) jsonUInt(*declaredSize, metadata.declaredSize);
	metadata.missingToken = jsonBool(item, "missingToken");
	metadata.onlyPublic = jsonBool(item, "onlyPublic");
	metadata.hasBuffer = jsonBool(item, "hasBuffer");
	metadata.bufferValid = jsonBool(item, "bufferValid");
	metadata.windowsSupported = jsonBool(item, "windowsSupported");
	metadata.shaHex = json::stringValue(item, "sha");
	metadata.name = json::stringValue(item, "name");
	metadata.type = json::stringValue(item, "type");
	metadata.releaseState = json::stringValue(item, "releaseState");
	metadata.osList = json::stringValue(item, "osList");
	metadata.osArch = json::stringValue(item, "osArch");
	metadata.controllerSupport = json::stringValue(item, "controllerSupport");
	metadata.developer = json::stringValue(item, "developer");
	metadata.publisher = json::stringValue(item, "publisher");
	metadata.installDir = json::stringValue(item, "installDir");
	metadata.publicBuildId = json::stringValue(item, "publicBuildId");
	metadata.architecture = json::stringValue(item, "architecture");
	metadata.preflight = json::stringValue(item, "preflight");
	const auto* installBytes = json::get(item, "installBytes");
	const auto* downloadBytes = json::get(item, "downloadBytes");
	if (installBytes) jsonUInt64(*installBytes, metadata.installBytes);
	if (downloadBytes) jsonUInt64(*downloadBytes, metadata.downloadBytes);
	const auto* depots = json::get(item, "windowsDepotIds");
	if (depots && depots->type == json::JsonType::Array) {
		for (const auto& depot : depots->array) {
			uint32_t value = 0;
			if (jsonUInt(depot, value) && value) metadata.windowsDepotIds.push_back(value);
		}
	}
	const auto* publicDepots = json::get(item, "publicDepots");
	if (publicDepots && publicDepots->type == json::JsonType::Array) {
		for (const auto& depot : publicDepots->array) {
			if (depot.type != json::JsonType::Object) continue;
			SteamAppDepotManifest manifest;
			const auto* depotId = json::get(depot, "depotId");
			const auto* depotFromApp = json::get(depot, "depotFromApp");
			const auto* manifestId = json::get(depot, "manifestId");
			const auto* size = json::get(depot, "size");
			const auto* download = json::get(depot, "download");
			if (!depotId || !jsonUInt(*depotId, manifest.depotId) || !manifest.depotId) continue;
			if (depotFromApp) jsonUInt(*depotFromApp, manifest.depotFromApp);
			if (manifestId) jsonUInt64(*manifestId, manifest.manifestId);
			if (size) jsonUInt64(*size, manifest.size);
			if (download) jsonUInt64(*download, manifest.download);
			manifest.sharedInstall = jsonBool(depot, "sharedInstall");
			manifest.osList = json::stringValue(depot, "osList");
			manifest.language = json::stringValue(depot, "language");
			metadata.publicDepots.push_back(std::move(manifest));
		}
	}
	const auto* launches = json::get(item, "launchOptions");
	if (launches && launches->type == json::JsonType::Array) {
		for (const auto& launch : launches->array) {
			if (launch.type != json::JsonType::Object) continue;
			SteamAppLaunchOption option;
			option.executable = json::stringValue(launch, "executable");
			option.arguments = json::stringValue(launch, "arguments");
			option.workingDir = json::stringValue(launch, "workingDir");
			option.description = json::stringValue(launch, "description");
			option.osList = json::stringValue(launch, "osList");
			option.osArch = json::stringValue(launch, "osArch");
			if (!option.executable.empty()) metadata.launchOptions.push_back(std::move(option));
		}
	}
	return metadata.bufferValid;
}

void loadCachedAppInfo(std::string& steamId, std::map<uint32_t, SteamAppMetadata>& metadata) {
	steamId.clear();
	metadata.clear();
	std::string text = readText(appInfoPath(), 32 * 1024 * 1024);
	if (text.empty()) return;
	json::JsonValue root;
	json::JsonParser parser(text);
	if (!parser.parse(root) || root.type != json::JsonType::Object) return;
	steamId = json::stringValue(root, "steamId");
	const auto* apps = json::get(root, "apps");
	if (!apps || apps->type != json::JsonType::Array) return;
	for (const auto& item : apps->array) {
		SteamAppMetadata app;
		if (loadAppMetadataItem(item, app)) metadata[app.appId] = std::move(app);
	}
}

bool loadInstallPlanItem(const json::JsonValue& root, SteamInstallPlan& plan) {
	plan = SteamInstallPlan();
	if (json::intValue(root, "version", 0) != 2) return false;
	const auto* appId = json::get(root, "appId");
	if (!appId || !jsonUInt(*appId, plan.appId) || !plan.appId) return false;
	plan.name = json::stringValue(root, "name");
	plan.branch = json::stringValue(root, "branch", "public");
	plan.buildId = json::stringValue(root, "buildId");
	plan.state = json::stringValue(root, "state");
	plan.message = json::stringValue(root, "message");
	plan.serverHost = json::stringValue(root, "serverHost");
	const auto* createdAt = json::get(root, "createdAt");
	const auto* installBytes = json::get(root, "installBytes");
	const auto* downloadBytes = json::get(root, "downloadBytes");
	if (createdAt) jsonUInt64(*createdAt, plan.createdAt);
	if (installBytes) jsonUInt64(*installBytes, plan.installBytes);
	if (downloadBytes) jsonUInt64(*downloadBytes, plan.downloadBytes);
	plan.directDepotCount = json::intValue(root, "directDepotCount", 0);
	plan.authorizedDepotCount = json::intValue(root, "authorizedDepotCount", 0);
	plan.unresolvedDepotCount = json::intValue(root, "unresolvedDepotCount", 0);
	const auto* depots = json::get(root, "depots");
	if (!depots || depots->type != json::JsonType::Array) return false;
	for (const auto& item : depots->array) {
		if (item.type != json::JsonType::Object) continue;
		SteamInstallPlanDepot depot;
		const auto* depotId = json::get(item, "depotId");
		const auto* contentAppId = json::get(item, "contentAppId");
		const auto* manifestId = json::get(item, "manifestId");
		const auto* size = json::get(item, "size");
		const auto* download = json::get(item, "download");
		const auto* expiration = json::get(item, "cdnTokenExpiration");
		if (!depotId || !jsonUInt(*depotId, depot.depotId) || !depot.depotId) continue;
		if (contentAppId) jsonUInt(*contentAppId, depot.contentAppId);
		if (manifestId) jsonUInt64(*manifestId, depot.manifestId);
		if (size) jsonUInt64(*size, depot.size);
		if (download) jsonUInt64(*download, depot.download);
		if (expiration) jsonUInt(*expiration, depot.cdnTokenExpiration);
		depot.sharedInstall = jsonBool(item, "sharedInstall");
		depot.keyAuthorized = jsonBool(item, "keyAuthorized");
		depot.manifestCodeGranted = jsonBool(item, "manifestCodeGranted");
		depot.cdnAuthChecked = jsonBool(item, "cdnAuthChecked");
		depot.cdnTokenIssued = jsonBool(item, "cdnTokenIssued");
		depot.state = json::stringValue(item, "state");
		plan.depots.push_back(std::move(depot));
	}
	return !plan.state.empty();
}

void loadCachedInstallPlan(std::string& steamId, SteamInstallPlan& plan) {
	steamId.clear();
	plan = SteamInstallPlan();
	std::string text = readText(installPlanPath(), 4 * 1024 * 1024);
	if (text.empty()) return;
	json::JsonValue root;
	json::JsonParser parser(text);
	if (!parser.parse(root) || root.type != json::JsonType::Object) return;
	std::string checkedSteamId = json::stringValue(root, "steamId");
	SteamInstallPlan checked;
	if (checkedSteamId.empty() || !loadInstallPlanItem(root, checked) || (checked.state != "authorized" && checked.state != "partial" && checked.state != "blocked") || !steamValidateInstallPlan(checked)) return;
	uint64_t now = (uint64_t)time(nullptr);
	if (!checked.createdAt || checked.createdAt > now || now - checked.createdAt > 600) return;
	steamId = std::move(checkedSteamId);
	plan = std::move(checked);
}

json::JsonValue installPlanJson(const std::string& steamId, const SteamInstallPlan& plan) {
	json::JsonValue root = json::JsonValue::makeObject();
	json::set(root, "version", json::JsonValue::makeNumber(2));
	json::set(root, "steamId", json::JsonValue::makeString(steamId));
	json::set(root, "appId", json::JsonValue::makeNumber(plan.appId));
	json::set(root, "name", json::JsonValue::makeString(plan.name));
	json::set(root, "branch", json::JsonValue::makeString(plan.branch));
	json::set(root, "buildId", json::JsonValue::makeString(plan.buildId));
	json::set(root, "state", json::JsonValue::makeString(plan.state));
	json::set(root, "message", json::JsonValue::makeString(plan.message));
	json::set(root, "serverHost", json::JsonValue::makeString(plan.serverHost));
	json::set(root, "createdAt", json::JsonValue::makeNumber(std::to_string(plan.createdAt)));
	json::set(root, "installBytes", json::JsonValue::makeNumber(std::to_string(plan.installBytes)));
	json::set(root, "downloadBytes", json::JsonValue::makeNumber(std::to_string(plan.downloadBytes)));
	json::set(root, "directDepotCount", json::JsonValue::makeNumber(plan.directDepotCount));
	json::set(root, "authorizedDepotCount", json::JsonValue::makeNumber(plan.authorizedDepotCount));
	json::set(root, "unresolvedDepotCount", json::JsonValue::makeNumber(plan.unresolvedDepotCount));
	json::JsonValue depots = json::JsonValue::makeArray();
	for (const auto& depot : plan.depots) {
		json::JsonValue item = json::JsonValue::makeObject();
		json::set(item, "depotId", json::JsonValue::makeNumber(depot.depotId));
		json::set(item, "contentAppId", json::JsonValue::makeNumber(depot.contentAppId));
		json::set(item, "manifestId", json::JsonValue::makeNumber(std::to_string(depot.manifestId)));
		json::set(item, "size", json::JsonValue::makeNumber(std::to_string(depot.size)));
		json::set(item, "download", json::JsonValue::makeNumber(std::to_string(depot.download)));
		json::set(item, "sharedInstall", json::JsonValue::makeBoolean(depot.sharedInstall));
		json::set(item, "keyAuthorized", json::JsonValue::makeBoolean(depot.keyAuthorized));
		json::set(item, "manifestCodeGranted", json::JsonValue::makeBoolean(depot.manifestCodeGranted));
		json::set(item, "cdnAuthChecked", json::JsonValue::makeBoolean(depot.cdnAuthChecked));
		json::set(item, "cdnTokenIssued", json::JsonValue::makeBoolean(depot.cdnTokenIssued));
		json::set(item, "cdnTokenExpiration", json::JsonValue::makeNumber(depot.cdnTokenExpiration));
		json::set(item, "state", json::JsonValue::makeString(depot.state));
		depots.array.push_back(std::move(item));
	}
	json::set(root, "depots", std::move(depots));
	return root;
}

bool sameInstallPlan(const SteamInstallPlan& left, const SteamInstallPlan& right) {
	if (left.appId != right.appId || left.name != right.name || left.branch != right.branch || left.buildId != right.buildId || left.state != right.state || left.message != right.message || left.serverHost != right.serverHost || left.createdAt != right.createdAt || left.installBytes != right.installBytes || left.downloadBytes != right.downloadBytes || left.directDepotCount != right.directDepotCount || left.authorizedDepotCount != right.authorizedDepotCount || left.unresolvedDepotCount != right.unresolvedDepotCount || left.depots.size() != right.depots.size()) return false;
	for (size_t i = 0; i < left.depots.size(); i++) {
		const auto& a = left.depots[i];
		const auto& b = right.depots[i];
		if (a.depotId != b.depotId || a.contentAppId != b.contentAppId || a.manifestId != b.manifestId || a.size != b.size || a.download != b.download || a.sharedInstall != b.sharedInstall || a.keyAuthorized != b.keyAuthorized || a.manifestCodeGranted != b.manifestCodeGranted || a.cdnAuthChecked != b.cdnAuthChecked || a.cdnTokenIssued != b.cdnTokenIssued || a.cdnTokenExpiration != b.cdnTokenExpiration || a.state != b.state) return false;
	}
	return true;
}

bool saveInstallPlan(const std::string& steamId, const SteamInstallPlan& plan) {
	json::JsonValue root = installPlanJson(steamId, plan);
	std::string text = json::serialize(root);
	json::JsonValue checkedRoot;
	json::JsonParser parser(text);
	SteamInstallPlan checked;
	if (!parser.parse(checkedRoot) || !loadInstallPlanItem(checkedRoot, checked) || !sameInstallPlan(plan, checked)) return false;
	return ensureDir(steamDir()) && writeAtomic(installPlanPath(), text.data(), text.size());
}

bool sameLaunch(const SteamAppLaunchOption& left, const SteamAppLaunchOption& right) {
	return left.executable == right.executable && left.arguments == right.arguments && left.workingDir == right.workingDir && left.description == right.description && left.osList == right.osList && left.osArch == right.osArch;
}

bool sameDepot(const SteamAppDepotManifest& left, const SteamAppDepotManifest& right) {
	return left.depotId == right.depotId && left.depotFromApp == right.depotFromApp && left.manifestId == right.manifestId && left.size == right.size && left.download == right.download && left.sharedInstall == right.sharedInstall && left.osList == right.osList && left.language == right.language;
}

bool sameAppMetadata(const SteamAppMetadata& left, const SteamAppMetadata& right) {
	if (left.appId != right.appId || left.changeNumber != right.changeNumber || left.declaredSize != right.declaredSize || left.missingToken != right.missingToken || left.onlyPublic != right.onlyPublic || left.hasBuffer != right.hasBuffer || left.bufferValid != right.bufferValid || left.windowsSupported != right.windowsSupported || left.shaHex != right.shaHex || left.name != right.name || left.type != right.type || left.releaseState != right.releaseState || left.osList != right.osList || left.osArch != right.osArch || left.controllerSupport != right.controllerSupport || left.developer != right.developer || left.publisher != right.publisher || left.installDir != right.installDir || left.publicBuildId != right.publicBuildId || left.architecture != right.architecture || left.preflight != right.preflight || left.installBytes != right.installBytes || left.downloadBytes != right.downloadBytes || left.windowsDepotIds != right.windowsDepotIds || left.publicDepots.size() != right.publicDepots.size() || left.launchOptions.size() != right.launchOptions.size()) return false;
	for (size_t i = 0; i < left.publicDepots.size(); i++) if (!sameDepot(left.publicDepots[i], right.publicDepots[i])) return false;
	for (size_t i = 0; i < left.launchOptions.size(); i++) if (!sameLaunch(left.launchOptions[i], right.launchOptions[i])) return false;
	return true;
}

bool saveLicenses(const std::string& steamId, const std::vector<LicenseRecord>& licenses) {
	json::JsonValue root = json::JsonValue::makeObject();
	json::set(root, "version", json::JsonValue::makeNumber(1));
	json::set(root, "steamId", json::JsonValue::makeString(steamId));
	json::set(root, "updatedAt", json::JsonValue::makeNumber((long long)time(nullptr)));
	json::JsonValue list = json::JsonValue::makeArray();
	for (const auto& license : licenses) {
		json::JsonValue item = json::JsonValue::makeObject();
		json::set(item, "packageId", json::JsonValue::makeNumber(license.packageId));
		json::set(item, "flags", json::JsonValue::makeNumber(license.flags));
		json::set(item, "licenseType", json::JsonValue::makeNumber(license.licenseType));
		json::set(item, "ownerId", json::JsonValue::makeNumber(license.ownerId));
		if (license.masterPackageId) json::set(item, "masterPackageId", json::JsonValue::makeNumber(license.masterPackageId));
		list.array.push_back(std::move(item));
	}
	json::set(root, "licenses", std::move(list));
	std::string text = json::serialize(root);
	return ensureDir(steamDir()) && writeAtomic(licensesPath(), text.data(), text.size());
}

json::JsonValue idArray(const std::vector<uint32_t>& values) {
	json::JsonValue list = json::JsonValue::makeArray();
	for (uint32_t value : values) list.array.push_back(json::JsonValue::makeNumber(value));
	return list;
}

bool saveEntitlements(const std::string& steamId, const ConnectionState& state, std::set<uint32_t>& entitledApps) {
	std::map<uint32_t, const LicenseRecord*> licenses;
	for (const auto& license : state.licenses) licenses[license.packageId] = &license;
	std::map<uint32_t, std::vector<uint32_t>> appPackages;
	std::vector<const SteamPicsPackageInfo*> resolved;
	for (const auto& package : state.resolvedPackages) resolved.push_back(&package);
	std::sort(resolved.begin(), resolved.end(), [](const auto* left, const auto* right) { return left->packageId < right->packageId; });
	for (const auto* package : resolved) {
		for (uint32_t appId : package->appIds) {
			auto& packageIds = appPackages[appId];
			if (std::find(packageIds.begin(), packageIds.end(), package->packageId) == packageIds.end()) packageIds.push_back(package->packageId);
		}
	}
	entitledApps.clear();
	for (const auto& app : appPackages) entitledApps.insert(app.first);

	json::JsonValue root = json::JsonValue::makeObject();
	json::set(root, "version", json::JsonValue::makeNumber(1));
	json::set(root, "steamId", json::JsonValue::makeString(steamId));
	json::set(root, "updatedAt", json::JsonValue::makeNumber((long long)time(nullptr)));
	json::set(root, "ownershipState", json::JsonValue::makeString("package_license"));
	json::set(root, "downloadabilityState", json::JsonValue::makeString("unresolved"));
	json::set(root, "licenseCount", json::JsonValue::makeNumber((long long)state.licenses.size()));
	json::set(root, "resolvedPackageCount", json::JsonValue::makeNumber((long long)resolved.size()));
	json::set(root, "entitlementAppCount", json::JsonValue::makeNumber((long long)appPackages.size()));
	json::set(root, "deniedTokenPackageCount", json::JsonValue::makeNumber((long long)state.deniedPackages.size()));
	json::set(root, "missingTokenPackageCount", json::JsonValue::makeNumber((long long)state.missingTokenPackages.size()));
	json::set(root, "unknownPackageCount", json::JsonValue::makeNumber((long long)state.unknownPackages.size()));
	json::set(root, "invalidPackageCount", json::JsonValue::makeNumber((long long)state.invalidPackages.size()));

	json::JsonValue packages = json::JsonValue::makeArray();
	for (const auto* package : resolved) {
		json::JsonValue item = json::JsonValue::makeObject();
		json::set(item, "packageId", json::JsonValue::makeNumber(package->packageId));
		json::set(item, "changeNumber", json::JsonValue::makeNumber(package->changeNumber));
		json::set(item, "appIds", idArray(package->appIds));
		json::set(item, "depotIds", idArray(package->depotIds));
		if (!package->billingType.empty()) json::set(item, "billingType", json::JsonValue::makeString(package->billingType));
		if (!package->licenseType.empty()) json::set(item, "packageLicenseType", json::JsonValue::makeString(package->licenseType));
		if (!package->status.empty()) json::set(item, "packageStatus", json::JsonValue::makeString(package->status));
		auto license = licenses.find(package->packageId);
		if (license != licenses.end()) {
			json::set(item, "licenseFlags", json::JsonValue::makeNumber(license->second->flags));
			json::set(item, "licenseType", json::JsonValue::makeNumber(license->second->licenseType));
			json::set(item, "ownerId", json::JsonValue::makeNumber(license->second->ownerId));
			if (license->second->masterPackageId) json::set(item, "masterPackageId", json::JsonValue::makeNumber(license->second->masterPackageId));
		}
		packages.array.push_back(std::move(item));
	}
	json::set(root, "packages", std::move(packages));

	json::JsonValue apps = json::JsonValue::makeArray();
	for (const auto& app : appPackages) {
		json::JsonValue item = json::JsonValue::makeObject();
		json::set(item, "appId", json::JsonValue::makeNumber(app.first));
		json::set(item, "packageIds", idArray(app.second));
		apps.array.push_back(std::move(item));
	}
	json::set(root, "apps", std::move(apps));

	std::map<uint32_t, std::string> unresolved;
	for (uint32_t packageId : state.deniedPackages) unresolved.emplace(packageId, "token_denied");
	for (uint32_t packageId : state.missingTokenPackages) unresolved.emplace(packageId, "missing_token");
	for (uint32_t packageId : state.unknownPackages) unresolved.emplace(packageId, "unknown_package");
	for (uint32_t packageId : state.invalidPackages) unresolved.emplace(packageId, "invalid_product_info");
	json::JsonValue unresolvedList = json::JsonValue::makeArray();
	for (const auto& package : unresolved) {
		json::JsonValue item = json::JsonValue::makeObject();
		json::set(item, "packageId", json::JsonValue::makeNumber(package.first));
		json::set(item, "state", json::JsonValue::makeString(package.second));
		unresolvedList.array.push_back(std::move(item));
	}
	json::set(root, "unresolvedPackages", std::move(unresolvedList));
	std::string text = json::serialize(root);
	return ensureDir(steamDir()) && writeAtomic(entitlementsPath(), text.data(), text.size());
}

json::JsonValue appMetadataJson(const SteamAppMetadata& metadata) {
	json::JsonValue item = json::JsonValue::makeObject();
	json::set(item, "appId", json::JsonValue::makeNumber(metadata.appId));
	json::set(item, "changeNumber", json::JsonValue::makeNumber(metadata.changeNumber));
	json::set(item, "declaredSize", json::JsonValue::makeNumber(metadata.declaredSize));
	json::set(item, "missingToken", json::JsonValue::makeBoolean(metadata.missingToken));
	json::set(item, "onlyPublic", json::JsonValue::makeBoolean(metadata.onlyPublic));
	json::set(item, "hasBuffer", json::JsonValue::makeBoolean(metadata.hasBuffer));
	json::set(item, "bufferValid", json::JsonValue::makeBoolean(metadata.bufferValid));
	json::set(item, "windowsSupported", json::JsonValue::makeBoolean(metadata.windowsSupported));
	json::set(item, "sha", json::JsonValue::makeString(metadata.shaHex));
	json::set(item, "name", json::JsonValue::makeString(metadata.name));
	json::set(item, "type", json::JsonValue::makeString(metadata.type));
	json::set(item, "releaseState", json::JsonValue::makeString(metadata.releaseState));
	json::set(item, "osList", json::JsonValue::makeString(metadata.osList));
	json::set(item, "osArch", json::JsonValue::makeString(metadata.osArch));
	json::set(item, "controllerSupport", json::JsonValue::makeString(metadata.controllerSupport));
	json::set(item, "developer", json::JsonValue::makeString(metadata.developer));
	json::set(item, "publisher", json::JsonValue::makeString(metadata.publisher));
	json::set(item, "installDir", json::JsonValue::makeString(metadata.installDir));
	json::set(item, "publicBuildId", json::JsonValue::makeString(metadata.publicBuildId));
	json::set(item, "architecture", json::JsonValue::makeString(metadata.architecture));
	json::set(item, "preflight", json::JsonValue::makeString(metadata.preflight));
	json::set(item, "installBytes", json::JsonValue::makeNumber(std::to_string(metadata.installBytes)));
	json::set(item, "downloadBytes", json::JsonValue::makeNumber(std::to_string(metadata.downloadBytes)));
	json::set(item, "windowsDepotIds", idArray(metadata.windowsDepotIds));
	json::JsonValue publicDepots = json::JsonValue::makeArray();
	for (const auto& depot : metadata.publicDepots) {
		json::JsonValue value = json::JsonValue::makeObject();
		json::set(value, "depotId", json::JsonValue::makeNumber(depot.depotId));
		json::set(value, "depotFromApp", json::JsonValue::makeNumber(depot.depotFromApp));
		json::set(value, "manifestId", json::JsonValue::makeNumber(std::to_string(depot.manifestId)));
		json::set(value, "size", json::JsonValue::makeNumber(std::to_string(depot.size)));
		json::set(value, "download", json::JsonValue::makeNumber(std::to_string(depot.download)));
		json::set(value, "sharedInstall", json::JsonValue::makeBoolean(depot.sharedInstall));
		json::set(value, "osList", json::JsonValue::makeString(depot.osList));
		json::set(value, "language", json::JsonValue::makeString(depot.language));
		publicDepots.array.push_back(std::move(value));
	}
	json::set(item, "publicDepots", std::move(publicDepots));
	json::JsonValue launches = json::JsonValue::makeArray();
	for (const auto& launch : metadata.launchOptions) {
		json::JsonValue option = json::JsonValue::makeObject();
		json::set(option, "executable", json::JsonValue::makeString(launch.executable));
		json::set(option, "arguments", json::JsonValue::makeString(launch.arguments));
		json::set(option, "workingDir", json::JsonValue::makeString(launch.workingDir));
		json::set(option, "description", json::JsonValue::makeString(launch.description));
		json::set(option, "osList", json::JsonValue::makeString(launch.osList));
		json::set(option, "osArch", json::JsonValue::makeString(launch.osArch));
		launches.array.push_back(std::move(option));
	}
	json::set(item, "launchOptions", std::move(launches));
	return item;
}

bool saveAppInfo(const std::string& steamId, const ConnectionState& state, std::map<uint32_t, SteamAppMetadata>& output) {
	output.clear();
	for (const auto& metadata : state.resolvedApps) output[metadata.appId] = metadata;
	json::JsonValue root = json::JsonValue::makeObject();
	json::set(root, "version", json::JsonValue::makeNumber(1));
	json::set(root, "steamId", json::JsonValue::makeString(steamId));
	json::set(root, "updatedAt", json::JsonValue::makeNumber((long long)time(nullptr)));
	json::set(root, "requestedAppCount", json::JsonValue::makeNumber((long long)state.catalogAppIds.size()));
	json::set(root, "resolvedAppCount", json::JsonValue::makeNumber((long long)output.size()));
	json::set(root, "deniedTokenAppCount", json::JsonValue::makeNumber((long long)state.deniedApps.size()));
	json::set(root, "missingTokenAppCount", json::JsonValue::makeNumber((long long)state.missingTokenApps.size()));
	json::set(root, "unknownAppCount", json::JsonValue::makeNumber((long long)state.unknownApps.size()));
	json::set(root, "invalidAppCount", json::JsonValue::makeNumber((long long)state.invalidApps.size()));
	json::JsonValue apps = json::JsonValue::makeArray();
	for (const auto& app : output) apps.array.push_back(appMetadataJson(app.second));
	json::set(root, "apps", std::move(apps));
	std::map<uint32_t, std::string> unresolved;
	for (uint32_t appId : state.deniedApps) unresolved.emplace(appId, "token_denied");
	for (uint32_t appId : state.missingTokenApps) unresolved.emplace(appId, "missing_token");
	for (uint32_t appId : state.unknownApps) unresolved.emplace(appId, "unknown_app");
	for (uint32_t appId : state.invalidApps) unresolved.emplace(appId, "invalid_product_info");
	json::JsonValue unresolvedList = json::JsonValue::makeArray();
	for (const auto& app : unresolved) {
		json::JsonValue item = json::JsonValue::makeObject();
		json::set(item, "appId", json::JsonValue::makeNumber(app.first));
		json::set(item, "state", json::JsonValue::makeString(app.second));
		unresolvedList.array.push_back(std::move(item));
	}
	json::set(root, "unresolvedApps", std::move(unresolvedList));
	std::string text = json::serialize(root);
	json::JsonValue checkedRoot;
	json::JsonParser checkedParser(text);
	if (!checkedParser.parse(checkedRoot)) return false;
	const auto* checkedApps = json::get(checkedRoot, "apps");
	if (!checkedApps || checkedApps->type != json::JsonType::Array || checkedApps->array.size() != output.size()) return false;
	for (const auto& item : checkedApps->array) {
		SteamAppMetadata checked;
		if (!loadAppMetadataItem(item, checked)) return false;
		auto expected = output.find(checked.appId);
		if (expected == output.end() || !sameAppMetadata(expected->second, checked)) return false;
	}
	return ensureDir(steamDir()) && writeAtomic(appInfoPath(), text.data(), text.size());
}

void putVarint(std::vector<uint8_t>& output, uint64_t value) {
	while (value >= 0x80) {
		output.push_back((uint8_t)(value | 0x80));
		value >>= 7;
	}
	output.push_back((uint8_t)value);
}

void putKey(std::vector<uint8_t>& output, uint32_t field, uint32_t wire) {
	putVarint(output, ((uint64_t)field << 3) | wire);
}

void putUInt(std::vector<uint8_t>& output, uint32_t field, uint64_t value) {
	putKey(output, field, 0);
	putVarint(output, value);
}

void putBool(std::vector<uint8_t>& output, uint32_t field, bool value) {
	putUInt(output, field, value ? 1 : 0);
}

void putFixed64(std::vector<uint8_t>& output, uint32_t field, uint64_t value) {
	putKey(output, field, 1);
	for (int i = 0; i < 8; i++) output.push_back((uint8_t)(value >> (i * 8)));
}

void putBytes(std::vector<uint8_t>& output, uint32_t field, const void* data, size_t size) {
	putKey(output, field, 2);
	putVarint(output, size);
	const uint8_t* bytes = (const uint8_t*)data;
	output.insert(output.end(), bytes, bytes + size);
}

void putString(std::vector<uint8_t>& output, uint32_t field, const std::string& value) {
	putBytes(output, field, value.data(), value.size());
}

bool getVarint(const uint8_t* data, size_t size, size_t& offset, uint64_t& value) {
	value = 0;
	for (int shift = 0; shift <= 63; shift += 7) {
		if (offset >= size) return false;
		uint8_t byte = data[offset++];
		value |= (uint64_t)(byte & 0x7f) << shift;
		if (!(byte & 0x80)) return true;
	}
	return false;
}

bool getFixed32(const uint8_t* data, size_t size, size_t& offset, uint32_t& value) {
	if (offset + 4 > size) return false;
	value = (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) | ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
	offset += 4;
	return true;
}

bool getFixed64(const uint8_t* data, size_t size, size_t& offset, uint64_t& value) {
	if (offset + 8 > size) return false;
	value = 0;
	for (int i = 0; i < 8; i++) value |= (uint64_t)data[offset + i] << (i * 8);
	offset += 8;
	return true;
}

bool getBytes(const uint8_t* data, size_t size, size_t& offset, const uint8_t*& value, size_t& valueSize) {
	uint64_t length = 0;
	if (!getVarint(data, size, offset, length) || length > size - offset) return false;
	value = data + offset;
	valueSize = (size_t)length;
	offset += valueSize;
	return true;
}

bool skipField(const uint8_t* data, size_t size, size_t& offset, uint32_t wire) {
	uint64_t value = 0;
	const uint8_t* bytes = nullptr;
	size_t byteCount = 0;
	if (wire == 0) return getVarint(data, size, offset, value);
	if (wire == 1) return getFixed64(data, size, offset, value);
	if (wire == 2) return getBytes(data, size, offset, bytes, byteCount);
	if (wire == 5) { uint32_t fixed = 0; return getFixed32(data, size, offset, fixed); }
	return false;
}

bool readUIntField(const uint8_t* data, size_t size, uint32_t wanted, uint64_t& result) {
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if (field == wanted && wire == 0) return getVarint(data, size, offset, result);
		if (field == wanted && wire == 1) return getFixed64(data, size, offset, result);
		if (!skipField(data, size, offset, wire)) return false;
	}
	return false;
}

std::vector<uint8_t> makeHeader(uint64_t steamId, int32_t sessionId, uint64_t sourceJobId = 0, const std::string& targetJobName = {}) {
	std::vector<uint8_t> header;
	if (steamId) putFixed64(header, 1, steamId);
	if (sessionId) putUInt(header, 2, (uint32_t)sessionId);
	if (sourceJobId) putFixed64(header, 10, sourceJobId);
	if (!targetJobName.empty()) putString(header, 12, targetJobName);
	return header;
}

std::vector<uint8_t> makePacket(uint32_t message, const std::vector<uint8_t>& header, const std::vector<uint8_t>& body) {
	std::vector<uint8_t> packet;
	uint32_t raw = message | kProtoMask;
	packet.reserve(8 + header.size() + body.size());
	for (int i = 0; i < 4; i++) packet.push_back((uint8_t)(raw >> (i * 8)));
	uint32_t headerSize = (uint32_t)header.size();
	for (int i = 0; i < 4; i++) packet.push_back((uint8_t)(headerSize >> (i * 8)));
	packet.insert(packet.end(), header.begin(), header.end());
	packet.insert(packet.end(), body.begin(), body.end());
	return packet;
}

bool parsePacket(const uint8_t* data, size_t size, Packet& packet) {
	if (size < 8) return false;
	uint32_t raw = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
	if (!(raw & kProtoMask)) return false;
	uint32_t headerSize = (uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
	if (headerSize > size - 8) return false;
	packet = Packet();
	packet.message = raw & ~kProtoMask;
	const uint8_t* header = data + 8;
	size_t offset = 0;
	while (offset < headerSize) {
		uint64_t key = 0;
		if (!getVarint(header, headerSize, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if (field == 1 && wire == 1) {
			if (!getFixed64(header, headerSize, offset, packet.steamId)) return false;
		} else if (field == 2 && wire == 0) {
			uint64_t session = 0;
			if (!getVarint(header, headerSize, offset, session)) return false;
			packet.sessionId = (int32_t)session;
		} else if ((field == 10 || field == 11) && wire == 1) {
			uint64_t jobId = 0;
			if (!getFixed64(header, headerSize, offset, jobId)) return false;
			if (field == 10) packet.sourceJobId = jobId;
			else packet.targetJobId = jobId;
		} else if (field == 12 && wire == 2) {
			const uint8_t* value = nullptr;
			size_t valueSize = 0;
			if (!getBytes(header, headerSize, offset, value, valueSize) || valueSize > 512) return false;
			packet.targetJobName.assign((const char*)value, valueSize);
		} else if (field == 13 && wire == 0) {
			uint64_t result = 0;
			if (!getVarint(header, headerSize, offset, result)) return false;
			packet.result = (int32_t)result;
		} else if (!skipField(header, headerSize, offset, wire)) return false;
	}
	packet.body = data + 8 + headerSize;
	packet.bodySize = size - 8 - headerSize;
	return true;
}

bool inflateGzip(const uint8_t* data, size_t size, size_t expected, std::vector<uint8_t>& output) {
	if (size < 18 || data[0] != 0x1f || data[1] != 0x8b || data[2] != 8 || expected > 64 * 1024 * 1024) return false;
	uint8_t flags = data[3];
	size_t offset = 10;
	if (flags & 4) {
		if (offset + 2 > size) return false;
		size_t extra = (size_t)data[offset] | ((size_t)data[offset + 1] << 8);
		offset += 2;
		if (extra > size - offset) return false;
		offset += extra;
	}
	if (flags & 8) { while (offset < size && data[offset++]); }
	if (flags & 16) { while (offset < size && data[offset++]); }
	if (flags & 2) { if (offset + 2 > size) return false; offset += 2; }
	if (offset > size - 8) return false;
	output.resize(expected);
	size_t written = tinfl_decompress_mem_to_mem(output.data(), output.size(), data + offset, size - offset - 8, 0);
	if (written != expected) { output.clear(); return false; }
	return true;
}

bool validAppInfoHost(const std::string& host) {
	if (host.empty() || host.size() > 255) return false;
	for (unsigned char ch : host) if (!std::isalnum(ch) && ch != '.' && ch != '-') return false;
	std::string lower = host;
	for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
	const std::string suffix = ".steamstatic.com";
	return lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool fetchAppInfoHttp(SteamAppMetadata& metadata, const std::string& host) {
	if (!validAppInfoHost(host) || metadata.shaHex.size() != 40) return false;
	std::string urlText = "https://" + host + "/appinfo/" + std::to_string(metadata.appId) + "/sha/" + metadata.shaHex + ".txt.gz";
	std::string body;
	if (!stardew_uwp::HttpGetString(urlText, {}, body, 16 * 1024 * 1024).ok || body.empty()) return false;
	std::vector<uint8_t> bytes(body.begin(), body.end());
	ByteVectorGuard bytesGuard{ &bytes };
	const uint8_t* text = bytes.data();
	size_t textSize = bytes.size();
	std::vector<uint8_t> expanded;
	ByteVectorGuard expandedGuard{ &expanded };
	if (bytes.size() >= 18 && bytes[0] == 0x1f && bytes[1] == 0x8b) {
		uint32_t expected = (uint32_t)bytes[bytes.size() - 4] | ((uint32_t)bytes[bytes.size() - 3] << 8) | ((uint32_t)bytes[bytes.size() - 2] << 16) | ((uint32_t)bytes[bytes.size() - 1] << 24);
		if (!expected || expected > 16 * 1024 * 1024 || !inflateGzip(bytes.data(), bytes.size(), expected, expanded)) return false;
		text = expanded.data();
		textSize = expanded.size();
	}
	metadata.hasBuffer = true;
	metadata.bufferValid = steamAppParseTextMetadata(metadata.appId, text, textSize, metadata);
	return metadata.bufferValid;
}

bool parseLicense(const uint8_t* data, size_t size, LicenseRecord& license) {
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		uint64_t value = 0;
		if ((field == 1 || field == 7 || field == 9 || field == 12 || field == 17 || field == 18) && wire == 0) {
			if (!getVarint(data, size, offset, value)) return false;
			if (field == 1) license.packageId = (uint32_t)value;
			else if (field == 7) license.flags = (uint32_t)value;
			else if (field == 9) license.licenseType = (uint32_t)value;
			else if (field == 12) license.ownerId = (uint32_t)value;
			else if (field == 17) license.accessToken = value;
			else license.masterPackageId = (uint32_t)value;
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return license.packageId != 0;
}

bool parseLicenseList(const uint8_t* data, size_t size, int& result, std::vector<LicenseRecord>& licenses) {
	result = 2;
	licenses.clear();
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if (field == 1 && wire == 0) {
			uint64_t value = 0;
			if (!getVarint(data, size, offset, value)) return false;
			result = (int)value;
		} else if (field == 2 && wire == 2) {
			const uint8_t* nested = nullptr;
			size_t nestedSize = 0;
			if (!getBytes(data, size, offset, nested, nestedSize)) return false;
			LicenseRecord license;
			if (parseLicense(nested, nestedSize, license)) licenses.push_back(license);
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return true;
}

bool splitEndpoint(const std::string& endpoint, std::string& host, INTERNET_PORT& port) {
	size_t colon = endpoint.find_last_of(':');
	if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) return false;
	host = endpoint.substr(0, colon);
	unsigned long value = strtoul(endpoint.c_str() + colon + 1, nullptr, 10);
	if (!value || value > 65535) return false;
	port = (INTERNET_PORT)value;
	return true;
}

std::vector<std::string> discoverServers() {
	std::string text;
	if (!stardew_uwp::HttpGetString("https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets&maxcount=8", {}, text).ok) return {};
	json::JsonValue root;
	json::JsonParser parser(text);
	if (!parser.parse(root)) return {};
	const auto* response = json::get(root, "response");
	const auto* serverList = response ? json::get(*response, "serverlist") : nullptr;
	if (!serverList || serverList->type != json::JsonType::Array) return {};
	std::vector<std::string> servers;
	for (const auto& item : serverList->array) {
		if (item.type != json::JsonType::Object) continue;
		if (json::stringValue(item, "type") != "websockets") continue;
		std::string endpoint = json::stringValue(item, "endpoint");
		if (!endpoint.empty()) servers.push_back(endpoint);
		if (servers.size() == 5) break;
	}
	return servers;
}

bool connectWebSocket(const std::string& endpoint, WebSocket& ws) {
	std::string hostA;
	INTERNET_PORT port = 0;
	if (!splitEndpoint(endpoint, hostA, port)) return false;
	int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, hostA.data(), (int)hostA.size(), nullptr, 0);
	if (length <= 0) return false;
	std::wstring host((size_t)length, L'\0');
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, hostA.data(), (int)hostA.size(), &host[0], length)) return false;
	ws.session = WinHttpOpen(L"StardewValleyXbox-CM/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ws.session) return false;
	WinHttpSetTimeouts(ws.session, 10000, 10000, 1000, 1000);
	ws.connect = WinHttpConnect(ws.session, host.c_str(), port, 0);
	if (!ws.connect) return false;
	ws.request = WinHttpOpenRequest(ws.connect, L"GET", L"/cmsocket/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!ws.request) return false;
	if (!WinHttpSetOption(ws.request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return false;
	if (!WinHttpSendRequest(ws.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return false;
	if (!WinHttpReceiveResponse(ws.request, nullptr)) return false;
	DWORD status = 0, statusSize = sizeof(status);
	if (!WinHttpQueryHeaders(ws.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) || status != 101) return false;
	ws.socket = WinHttpWebSocketCompleteUpgrade(ws.request, 0);
	if (!ws.socket) return false;
	WinHttpCloseHandle(ws.request);
	ws.request = nullptr;
	return true;
}

bool sendPacket(WebSocket& ws, const std::vector<uint8_t>& packet) {
	if (!ws.socket || packet.empty() || packet.size() > 16 * 1024 * 1024) return false;
	return WinHttpWebSocketSend(ws.socket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (void*)packet.data(), (DWORD)packet.size()) == NO_ERROR;
}

bool receiveMessage(HINTERNET socket, std::vector<uint8_t>& message, std::vector<uint8_t>& buffer, bool& timedOut) {
	message.clear();
	timedOut = false;
	if (buffer.size() < 65536) buffer.resize(65536);
	for (;;) {
		DWORD read = 0;
		WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
		DWORD error = WinHttpWebSocketReceive(socket, buffer.data(), (DWORD)buffer.size(), &read, &type);
		if (error == ERROR_WINHTTP_TIMEOUT) { timedOut = true; return true; }
		if (error != NO_ERROR) return false;
		if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return false;
		if (type != WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) continue;
		if (message.size() + read > 16 * 1024 * 1024) return false;
		message.insert(message.end(), buffer.data(), buffer.data() + read);
		if (read) SecureZeroMemory(buffer.data(), read);
		if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) return true;
	}
}

struct ReceiveResult {
	std::vector<uint8_t> message;
	bool ok = false;
	bool timedOut = false;
};

std::future<ReceiveResult> beginReceive(HINTERNET socket) {
	return std::async(std::launch::async, [socket] {
		ReceiveResult result;
		std::vector<uint8_t> buffer(65536);
		ByteVectorGuard bufferGuard{ &buffer };
		result.ok = receiveMessage(socket, result.message, buffer, result.timedOut);
		return result;
	});
}

void finishReceive(WebSocket& ws, std::future<ReceiveResult>& receive) {
	if (!receive.valid()) return;
	if (receive.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && ws.socket) {
		WinHttpWebSocketShutdown(ws.socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
		if (receive.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
			WinHttpWebSocketClose(ws.socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
	}
	receive.wait();
}

std::vector<uint8_t> helloPacket() {
	std::vector<uint8_t> body;
	putUInt(body, 1, kProtocolVersion);
	return makePacket(kMsgHello, {}, body);
}

std::vector<uint8_t> logonPacket(const WorkerArgs& args) {
	std::vector<uint8_t> body;
	putUInt(body, 1, kProtocolVersion);
	putUInt(body, 3, 0);
	putUInt(body, 5, 1771);
	putString(body, 6, "english");
	putUInt(body, 7, 0xffffffffu);
	putBool(body, 8, true);
	putString(body, 50, args.accountName);
	putString(body, 96, "Stardew Valley Xbox");
	putBool(body, 102, true);
	putString(body, 108, args.refreshToken);
	std::vector<uint8_t> packet = makePacket(kMsgLogon, makeHeader(kClientSuppliedSteamId, 0), body);
	if (!body.empty()) SecureZeroMemory(body.data(), body.size());
	return packet;
}

std::vector<uint8_t> heartbeatPacket(const ConnectionState& state) {
	std::vector<uint8_t> body;
	putBool(body, 1, false);
	return makePacket(kMsgHeartBeat, makeHeader(state.steamId, state.sessionId), body);
}

std::vector<uint8_t> picsAccessTokenPacket(const ConnectionState& state, const std::vector<uint32_t>& packageIds, uint64_t jobId) {
	std::vector<uint8_t> body = steamPicsMakeAccessTokenRequest(packageIds);
	return makePacket(kMsgPicsAccessTokenRequest, makeHeader(state.steamId, state.sessionId, jobId), body);
}

std::vector<uint8_t> picsProductInfoPacket(const ConnectionState& state, const std::vector<SteamPicsPackageRequest>& packages, uint64_t jobId) {
	std::vector<uint8_t> body = steamPicsMakeProductInfoRequest(packages);
	std::vector<uint8_t> packet = makePacket(kMsgPicsProductInfoRequest, makeHeader(state.steamId, state.sessionId, jobId), body);
	if (!body.empty()) SecureZeroMemory(body.data(), body.size());
	return packet;
}

std::vector<uint8_t> appAccessTokenPacket(const ConnectionState& state, const std::vector<uint32_t>& appIds, uint64_t jobId) {
	std::vector<uint8_t> body = steamAppMakeAccessTokenRequest(appIds);
	return makePacket(kMsgPicsAccessTokenRequest, makeHeader(state.steamId, state.sessionId, jobId), body);
}

std::vector<uint8_t> appProductInfoPacket(const ConnectionState& state, const std::vector<SteamAppRequest>& apps, uint64_t jobId) {
	std::vector<uint8_t> body = steamAppMakeProductInfoRequest(apps);
	std::vector<uint8_t> packet = makePacket(kMsgPicsProductInfoRequest, makeHeader(state.steamId, state.sessionId, jobId), body);
	if (!body.empty()) SecureZeroMemory(body.data(), body.size());
	return packet;
}

std::vector<uint8_t> depotKeyPacket(const ConnectionState& state, uint32_t appId, uint32_t depotId, uint64_t jobId) {
	std::vector<uint8_t> body = steamContentMakeDepotKeyRequest(appId, depotId);
	return makePacket(kMsgDepotKeyRequest, makeHeader(state.steamId, state.sessionId, jobId), body);
}

std::vector<uint8_t> servicePacket(const ConnectionState& state, const std::string& method, const std::vector<uint8_t>& body, uint64_t jobId) {
	return makePacket(kMsgServiceMethodCallFromClient, makeHeader(state.steamId, state.sessionId, jobId, method), body);
}

bool inCurrentBatch(const ConnectionState& state, uint32_t packageId) {
	return std::find(state.batchPackageIds.begin(), state.batchPackageIds.end(), packageId) != state.batchPackageIds.end();
}

bool inCurrentAppBatch(const ConnectionState& state, uint32_t appId) {
	return std::find(state.batchAppIds.begin(), state.batchAppIds.end(), appId) != state.batchAppIds.end();
}

void storeResolvedPackage(ConnectionState& state, SteamPicsPackageInfo package) {
	for (auto& current : state.resolvedPackages) {
		if (current.packageId == package.packageId) {
			current = std::move(package);
			return;
		}
	}
	state.resolvedPackages.push_back(std::move(package));
}

void storeResolvedApp(ConnectionState& state, SteamAppMetadata metadata) {
	for (auto& current : state.resolvedApps) {
		if (current.appId == metadata.appId) {
			current = std::move(metadata);
			return;
		}
	}
	state.resolvedApps.push_back(std::move(metadata));
}

bool validContentHost(const std::string& host) {
	if (host.empty() || host.size() > 255) return false;
	for (unsigned char ch : host) if (!std::isalnum(ch) && ch != '.' && ch != '-') return false;
	std::string lower = host;
	for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
	const std::string steam = ".steamcontent.com";
	const std::string akamai = ".akamaized.net";
	return (lower.size() > steam.size() && lower.compare(lower.size() - steam.size(), steam.size(), steam) == 0) || (lower.size() > akamai.size() && lower.compare(lower.size() - akamai.size(), akamai.size(), akamai) == 0);
}

bool serverAllowsApp(const SteamContentServer& server, uint32_t appId) {
	return server.allowedAppIds.empty() || std::find(server.allowedAppIds.begin(), server.allowedAppIds.end(), appId) != server.allowedAppIds.end();
}

bool serverAllowsPlan(const SteamContentServer& server, const SteamInstallPlan& plan) {
	return serverAllowsApp(server, plan.appId);
}

struct InternetHandle {
	HINTERNET value = nullptr;
	~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};

bool utf8Host(const std::string& value, std::wstring& output) {
	output.clear();
	if (value.empty() || value.size() > 255) return false;
	int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), nullptr, 0);
	if (count <= 0) return false;
	output.assign((size_t)count, L'\0');
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), &output[0], count) != 0;
}

struct ContentHttpClient {
	InternetHandle session;
	InternetHandle connect;
	std::string host;

	void reset() {
		if (connect.value) { WinHttpCloseHandle(connect.value); connect.value = nullptr; }
		if (session.value) { WinHttpCloseHandle(session.value); session.value = nullptr; }
		host.clear();
	}

	bool open(const std::string& value) {
		reset();
		std::wstring wideHost;
		if (!validContentHost(value) || !utf8Host(value, wideHost)) return false;
		session.value = WinHttpOpen(L"StardewValleyXbox-Content/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session.value) return false;
		WinHttpSetTimeouts(session.value, 10000, 10000, 30000, 60000);
		connect.value = WinHttpConnect(session.value, wideHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
		if (!connect.value) { reset(); return false; }
		host = value;
		return true;
	}

	bool fetch(const std::wstring& target, size_t limit, std::vector<uint8_t>& response, DWORD& status) {
		response.clear();
		status = 0;
		if (!session.value || !connect.value || !limit || target.empty()) return false;
		InternetHandle request;
		request.value = WinHttpOpenRequest(connect.value, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
		if (!request.value) return false;
		DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
		if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) return false;
		if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) return false;
		DWORD statusSize = sizeof(status);
		if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) return false;
		if (status != 200) return true;
		DWORD contentLength = 0;
		DWORD contentLengthSize = sizeof(contentLength);
		if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize, WINHTTP_NO_HEADER_INDEX)) {
			if (!contentLength || contentLength > limit) return false;
			response.reserve(contentLength);
		}
		for (;;) {
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(request.value, &available)) return false;
			if (!available) break;
			if (response.size() > limit || (size_t)available > limit - response.size()) return false;
			size_t offset = response.size();
			response.resize(offset + available);
			DWORD read = 0;
			if (!WinHttpReadData(request.value, response.data() + offset, available, &read)) return false;
			response.resize(offset + read);
			if (!read) break;
		}
		return !response.empty();
	}
};

struct InstallChunkDownload {
	size_t fileIndex = 0;
	size_t chunkIndex = 0;
	SteamManifestChunk chunk;
	std::vector<uint8_t> expanded;
	DWORD httpStatus = 0;
	std::string error;
	bool ok = false;
};

InstallChunkDownload downloadInstallChunk(ContentHttpClient& client, const std::string& host, uint32_t depotId,
		const SteamManifestChunk& chunk, size_t fileIndex, size_t chunkIndex, const std::vector<uint8_t>& depotKey) {
	InstallChunkDownload result;
	result.fileIndex = fileIndex;
	result.chunkIndex = chunkIndex;
	result.chunk = chunk;
	std::wstring target = steamChunkRequestTarget(depotId, chunk.sha);
	std::vector<uint8_t> response;
	ByteVectorGuard responseGuard{ &response };
	bool fetched = false;
	for (int attempt = 0; attempt < 3 && !fetched; attempt++) {
		fetched = client.fetch(target, chunk.compressedBytes, response, result.httpStatus) && result.httpStatus == 200;
		if (!fetched && attempt < 2) client.open(host);
	}
	if (!target.empty()) SecureZeroMemory(&target[0], target.size() * sizeof(wchar_t));
	std::string compression;
	if (fetched) result.ok = steamChunkProcess(response.data(), response.size(), depotKey.data(), depotKey.size(),
		chunk.sha, chunk.checksum, chunk.originalBytes, chunk.compressedBytes, steamChunkDefaultLimits(),
		result.expanded, compression, result.error);
	if (!result.ok && result.error.empty()) result.error = result.httpStatus && result.httpStatus != 200
		? "Install CDN request was denied" : "Install chunk retrieval failed";
	return result;
}

bool fetchAuthorizedContent(const std::string& host, const std::wstring& target, size_t limit, std::vector<uint8_t>& response, DWORD& status) {
	ContentHttpClient client;
	return client.open(host) && client.fetch(target, limit, response, status);
}void publishInstallPlan(const std::string& steamId, const SteamInstallPlan& plan, SteamCmState state, const std::string& message) {
	AcquireSRWLockExclusive(&g_lock);
	g_installPlanSteamId = steamId;
	g_installPlan = plan;
	g_status.state = state;
	g_status.message = message;
	ReleaseSRWLockExclusive(&g_lock);
}

bool cancelContentPlan(ConnectionState& state, const std::string& steamId) {
	uint32_t appId = state.plan.appId;
	if (!installCancelRequested(appId)) return true;
	clearManifestAuthorization(state);
	clearInstallDepotKeys(state);
	state.installAttempted = true;
	state.plan.state = "cancelled";
	state.plan.message = "Install cancelled; request install again to resume";
	state.planStage = PlanStage::Complete;
	state.planDeadline = 0;
	SteamInstallStatus paused;
	AcquireSRWLockExclusive(&g_lock);
	auto found = g_installStatuses.find(appId);
	if (found != g_installStatuses.end()) paused = found->second;
	paused.appId = appId;
	paused.state = "paused";
	paused.message = "Install cancelled; press A to resume";
	g_installStatuses[appId] = paused;
	g_installPlanSteamId = steamId;
	g_installPlan = state.plan;
	g_status.state = SteamCmState::Online;
	g_status.message = state.plan.message;
	ReleaseSRWLockExclusive(&g_lock);
	clearInstallCancel(appId);
	char line[112];
	sprintf_s(line, "steam install: app=%u state=cancelled\n", appId);
	logLine(line);
	if (g_changed) g_changed();
	return true;
}

bool finishContentPlan(ConnectionState& state, const std::string& steamId) {
	clearInstallDepotKeys(state);
	if (state.plan.authorizedDepotCount == state.plan.directDepotCount && state.plan.unresolvedDepotCount == 0 && state.plan.directDepotCount > 0) {
		state.plan.state = "authorized";
		state.plan.message = state.installSucceeded ? "Full install completed" : state.installAttempted ? "Install paused; request install again to resume" : "Authorization verified";
	} else if (state.plan.authorizedDepotCount > 0) {
		state.plan.state = "partial";
		state.plan.message = "Partial authorization; unresolved depots remain";
	} else {
		state.plan.state = "blocked";
		state.plan.message = "No downloadable depot plan was authorized";
	}
	state.plan.createdAt = (uint64_t)time(nullptr);
	if (!steamValidateInstallPlan(state.plan)) {
		state.plan.state = "error";
		state.plan.message = "Install plan validation failed";
		state.planStage = PlanStage::Error;
		publishInstallPlan(steamId, state.plan, SteamCmState::Error, state.plan.message);
		return false;
	}
	if (!saveInstallPlan(steamId, state.plan)) {
		state.plan.state = "error";
		state.plan.message = "Install plan could not be cached";
		state.planStage = PlanStage::Error;
		publishInstallPlan(steamId, state.plan, SteamCmState::Error, state.plan.message);
		return false;
	}
	state.planStage = PlanStage::Complete;
	publishInstallPlan(steamId, state.plan, SteamCmState::Online, state.plan.message);
	char line[224];
	sprintf_s(line, "steam plan: app=%u depots=%d authorized=%d unresolved=%d state=%s\n", state.plan.appId, state.plan.directDepotCount, state.plan.authorizedDepotCount, state.plan.unresolvedDepotCount, state.plan.state.c_str());
	logLine(line);
	if (g_changed) g_changed();
	return true;
}

bool prepareAuthorizedInstall(ConnectionState& state, const std::string& steamId) {
	SteamManifestMetadata merged;
	std::string error;
	SteamFullInstallLimits limits;
	SteamAppMetadata appMetadata;
	bool configured = steamFullInstallMergeManifests(state.installManifests, merged, error);
	AcquireSRWLockShared(&g_lock);
	auto app = g_appMetadata.find(state.plan.appId);
	if (app != g_appMetadata.end()) appMetadata = app->second;
	ReleaseSRWLockShared(&g_lock);
	if (configured) {
		configured = false;
		for (const auto& launch : appMetadata.launchOptions) {
			if (launch.executable.empty() || (!launch.osList.empty() && launch.osList.find("windows") == std::string::npos)) continue;
			SteamFullInstallLimits candidate;
			std::string candidateError;
			if (steamFullInstallConfigure(merged, launch.executable, candidate, candidateError)) { limits = std::move(candidate); configured = true; break; }
			error = std::move(candidateError);
		}
	}
	uint64_t transferBytes = 0;
	if (configured) configured = steamFullInstallValidateManifest(merged, limits, transferBytes, error);
	if (!configured) {
		SteamInstallStatus invalid;
		invalid.state = "invalid";
		invalid.message = error.empty() ? "Install invalid: authorized manifests do not contain a declared Windows launch executable" : error;
		invalid.appId = state.plan.appId;
		invalid.buildId = state.plan.buildId;
		publishInstallStatus(invalid, nullptr);
		state.installAttempted = true;
		clearInstallDepotKeys(state);
		return finishContentPlan(state, steamId);
	}
	state.installMetadata = std::move(merged);
	state.installLimits = std::move(limits);
	SteamManifestStatus ready = manifestStatusFor(state.installMetadata, "ready", state.installManifests.size() > 1 ? "Combined Windows manifests ready; full install pending" : "Manifest ready; full install pending");
	publishManifestResult(ready, &state.installMetadata);
	SteamInstallStatus pending;
	pending.state = "pending";
	pending.message = state.installManifests.size() > 1 ? "Full install pending validated multi-depot manifest" : "Full install pending validated manifest";
	pending.appId = state.installMetadata.appId;
	pending.depotId = state.installMetadata.depotId;
	pending.manifestId = state.installMetadata.manifestId;
	pending.buildId = state.installMetadata.buildId;
	pending.totalBytes = transferBytes;
	pending.totalFiles = state.installMetadata.files.size();
	pending.totalChunks = manifestChunkCount(state.installMetadata);
	publishInstallStatus(pending, nullptr);
	state.planStage = PlanStage::RetrieveInstall;
	return true;
}

bool advanceContentPlan(ConnectionState& state, const std::string& steamId) {
	while (state.planDepotIndex < state.plan.depots.size() && !state.plan.depots[state.planDepotIndex].manifestId) state.planDepotIndex++;
	if (state.planDepotIndex >= state.plan.depots.size()) {
		if (!state.installAttempted && state.plan.authorizedDepotCount == state.plan.directDepotCount && state.plan.unresolvedDepotCount == 0 && state.plan.directDepotCount > 0 && state.installManifests.size() == (size_t)state.plan.directDepotCount && state.installDepotKeys.size() == state.installManifests.size()) return prepareAuthorizedInstall(state, steamId);
		return finishContentPlan(state, steamId);
	}
	state.planStage = PlanStage::NeedDepotKey;
	return true;
}

bool failContentDepot(ConnectionState& state, const std::string& steamId, const char* reason) {
	clearManifestAuthorization(state);
	if (state.planDepotIndex >= state.plan.depots.size()) return finishContentPlan(state, steamId);
	state.plan.depots[state.planDepotIndex].state = reason;
	state.plan.unresolvedDepotCount++;
	state.planDepotIndex++;
	state.planDeadline = 0;
	return advanceContentPlan(state, steamId);
}

bool retrieveAuthorizedManifest(ConnectionState& state, const std::string& steamId) {
	if (state.planDepotIndex >= state.plan.depots.size() || state.planServers.empty()) return finishContentPlan(state, steamId);
	SteamInstallPlanDepot& depot = state.plan.depots[state.planDepotIndex];
	SteamManifestStatus pending;
	pending.state = "pending";
	pending.message = "Retrieving authorized manifest metadata";
	pending.appId = state.plan.appId;
	pending.depotId = depot.depotId;
	pending.manifestId = depot.manifestId;
	pending.buildId = state.plan.buildId;
	publishManifestResult(pending, nullptr);
	std::wstring target = steamManifestRequestTarget(depot.depotId, depot.manifestId, state.manifestRequestCode);
	std::vector<uint8_t> response;
	ByteVectorGuard responseGuard{ &response };
	DWORD httpStatus = 0;
	bool fetched = fetchAuthorizedContent(state.planServers[0].host, target, steamManifestDefaultLimits().compressedBytes, response, httpStatus);
	if (!target.empty()) SecureZeroMemory(&target[0], target.size() * sizeof(wchar_t));
	target.clear();
	SecureZeroMemory(&state.manifestRequestCode, sizeof(state.manifestRequestCode));
	state.manifestRequestCode = 0;
	SteamManifestMetadata metadata;
	std::string error;
	bool valid = fetched && httpStatus == 200 && steamManifestParseZip(response.data(), response.size(), state.manifestDepotKey.data(), state.manifestDepotKey.size(), depot.depotId, depot.manifestId, steamManifestDefaultLimits(), metadata, error);
	if (valid) {
		metadata.steamId = steamId;
		metadata.appId = state.plan.appId;
		metadata.buildId = state.plan.buildId;
		metadata.retrievedAt = (uint64_t)time(nullptr);
		valid = saveManifestMetadata(metadata);
	}
	if (valid) {
		SteamManifestStatus ready = manifestStatusFor(metadata, "ready", "Manifest ready; full install pending");
		publishManifestResult(ready, &metadata);
		char line[256];
		sprintf_s(line, "steam manifest: app=%u depot=%u manifest=%llu files=%zu chunks=%zu state=ready\n", metadata.appId, metadata.depotId, (unsigned long long)metadata.manifestId, metadata.files.size(), manifestChunkCount(metadata));
		logLine(line);
		state.installManifests.push_back(metadata);
		state.installDepotKeys[depot.depotId] = state.manifestDepotKey;
	} else {
		if (depot.state == "authorized" && state.plan.authorizedDepotCount > 0) state.plan.authorizedDepotCount--;
		depot.state = "manifest_invalid";
		state.plan.unresolvedDepotCount++;
		DeleteFileW(manifestMetadataPath().c_str());
		SteamManifestStatus invalid = pending;
		invalid.state = "invalid";
		invalid.message = httpStatus && httpStatus != 200 ? "Manifest invalid: CDN request was denied" : "Manifest invalid: retrieval or validation failed";
		publishManifestResult(invalid, nullptr);
		char line[320];
		sprintf_s(line, "steam manifest: state=invalid http=%lu bytes=%zu reason=%s; no content chunks requested\n", (unsigned long)httpStatus, response.size(), error.empty() ? "transport" : error.c_str());
		logLine(line);
	}
	clearManifestAuthorization(state);
	steamManifestSecureClear(metadata);
	state.planDepotIndex++;
	state.planDeadline = 0;
	return advanceContentPlan(state, steamId);
}

bool retrieveAuthorizedInstall(WebSocket& ws, ConnectionState& state, const std::string& steamId) {
	if (state.planServers.empty() || state.installMetadata.files.empty() || state.installDepotKeys.empty()) return finishContentPlan(state, steamId);
	SteamManifestMetadata metadata = state.installMetadata;
	uint64_t referenceTransferBytes = 0;
	std::string error;
	if (!state.installLimits.appId || metadata.steamId != steamId || metadata.appId != state.plan.appId || metadata.buildId != state.plan.buildId || !steamFullInstallValidateManifest(metadata, state.installLimits, referenceTransferBytes, error)) {
		clearManifestAuthorization(state);
		clearInstallDepotKeys(state);
		state.installAttempted = true;
		SteamInstallStatus invalid;
		invalid.state = "invalid";
		invalid.message = error.empty() ? "Install invalid: fresh manifest identity changed" : error;
		invalid.appId = state.plan.appId;
		invalid.depotId = metadata.depotId;
		invalid.manifestId = metadata.manifestId;
		invalid.buildId = state.plan.buildId;
		publishInstallStatus(invalid, nullptr);
		return advanceContentPlan(state, steamId);
	}
	state.installAttempted = true;
	SteamFullInstallProgress progress;
	bool resumed = false;
	if (!steamFullInstallBegin(g_localState, metadata, state.installLimits, progress, referenceTransferBytes, resumed, error)) {
		clearManifestAuthorization(state);
		clearInstallDepotKeys(state);
		SteamInstallStatus invalid;
		invalid.state = "invalid";
		invalid.message = error;
		invalid.appId = metadata.appId;
		invalid.depotId = metadata.depotId;
		invalid.manifestId = metadata.manifestId;
		invalid.buildId = metadata.buildId;
		invalid.totalBytes = referenceTransferBytes;
		invalid.totalFiles = metadata.files.size();
		invalid.totalChunks = manifestChunkCount(metadata);
		publishInstallStatus(invalid, nullptr);
		steamManifestSecureClear(metadata);
		return advanceContentPlan(state, steamId);
	}
	SteamInstallStatus installing;
	installing.state = "downloading";
	installing.message = resumed ? "Resuming verified Steam install" : "Downloading verified Steam install";
	installing.appId = metadata.appId;
	installing.depotId = metadata.depotId;
	installing.manifestId = metadata.manifestId;
	installing.buildId = metadata.buildId;
	installing.transferredBytes = progress.transferredBytes;
	installing.totalBytes = referenceTransferBytes;
	installing.filesVerified = progress.filesVerified;
	installing.totalFiles = metadata.files.size();
	installing.chunksVerified = progress.chunksVerified;
	installing.totalChunks = manifestChunkCount(metadata);
	publishInstallStatus(installing, nullptr);
	constexpr size_t kParallelChunkDownloads = 16;
	constexpr size_t kChunksPerWorkerWindow = 4;
	std::vector<std::unique_ptr<ContentHttpClient>> clients;
	std::vector<std::string> clientHosts;
	// spread across the CDN hosts the plan already ranked instead of hammering the first one
	for (size_t i = 0; i < kParallelChunkDownloads; i++) {
		const std::string& host = state.planServers[i % state.planServers.size()].host;
		std::unique_ptr<ContentHttpClient> client(new ContentHttpClient());
		if (client->open(host)) {
			clients.push_back(std::move(client));
			clientHosts.push_back(host);
		}
	}
	bool valid = !clients.empty();
	if (!valid) error = "Install CDN connection could not be opened";
	std::map<std::pair<size_t, size_t>, InstallChunkDownload> prefetch;
	std::vector<std::future<std::vector<InstallChunkDownload>>> inflight;
	std::vector<InstallPrefetchItem> inflightQueue;
	size_t cursorFile = progress.nextFile;
	size_t cursorChunk = progress.currentChunk;
	while (valid && progress.nextFile < metadata.files.size()) {
		if (InterlockedCompareExchange(&g_stop, 0, 0)) { error = "Install interrupted"; valid = false; break; }
		size_t beforeFile = progress.nextFile;
		if (!steamFullInstallPrepareFile(g_localState, metadata, progress, state.installLimits, error)) { valid = false; break; }
		if (progress.nextFile != beforeFile) {
			// the window is being thrown away for a new file, so its results and failures no longer matter
			for (auto& future : inflight) { try { future.get(); } catch (...) {} }
			inflight.clear();
			prefetch.clear();
			cursorFile = progress.nextFile;
			cursorChunk = progress.currentChunk;
			installing.filesVerified = progress.filesVerified;
			installing.message = "Preparing install files";
			publishInstallStatus(installing, nullptr);
			continue;
		}
		const auto& file = metadata.files[progress.nextFile];
		uint32_t sourceDepotId = file.sourceDepotId ? file.sourceDepotId : metadata.depotId;
		auto sourceKey = state.installDepotKeys.find(sourceDepotId);
		if (sourceKey == state.installDepotKeys.end() || sourceKey->second.size() != 32) { error = "Install source depot key is unavailable"; valid = false; break; }
		while (valid && progress.currentChunk < file.chunks.size()) {
			if (InterlockedCompareExchange(&g_stop, 0, 0)) { error = "Install interrupted"; valid = false; break; }
			// the window spans files and one stays in flight during writes, so connections never idle
			auto launchWindow = [&]() {
				if (!inflight.empty()) return true;
				inflightQueue = steamInstallBuildChunkQueue(
					metadata, cursorFile, cursorChunk, clients.size() * kChunksPerWorkerWindow);
				if (inflightQueue.empty()) return true;
				cursorFile = inflightQueue.back().fileIndex;
				cursorChunk = inflightQueue.back().chunkIndex + 1;
				if (cursorChunk >= metadata.files[cursorFile].chunks.size()) {
					cursorFile++;
					cursorChunk = 0;
				}
				size_t workerCount = (std::min)(clients.size(), inflightQueue.size());
				for (size_t i = 0; i < workerCount; i++) {
					ContentHttpClient* client = clients[i].get();
					const std::string& host = clientHosts[i];
					try {
						inflight.push_back(std::async(std::launch::async, [this_queue = &inflightQueue, &state, client, &host, i, workerCount] {
							std::vector<InstallChunkDownload> workerDownloads;
							for (size_t offset = i; offset < this_queue->size(); offset += workerCount) {
								const InstallPrefetchItem& item = (*this_queue)[offset];
								auto key = state.installDepotKeys.find(item.depotId);
								if (key == state.installDepotKeys.end() || key->second.size() != 32) {
									InstallChunkDownload failed;
									failed.fileIndex = item.fileIndex;
									failed.chunkIndex = item.chunkIndex;
									failed.error = "Install source depot key is unavailable";
									workerDownloads.push_back(std::move(failed));
									continue;
								}
								workerDownloads.push_back(downloadInstallChunk(*client, host, item.depotId,
									item.chunk, item.fileIndex, item.chunkIndex, key->second));
							}
							return workerDownloads;
						}));
					} catch (...) {
						inflight.clear();
						return false;
					}
				}
				return true;
			};

			auto harvestWindow = [&]() {
				bool ok = true;
				try {
					for (auto& future : inflight) {
						for (auto& download : future.get()) {
							prefetch[std::make_pair(download.fileIndex, download.chunkIndex)] = std::move(download);
						}
					}
				} catch (...) {
					ok = false;
				}
				inflight.clear();
				return ok;
			};

			if (prefetch.find(std::make_pair(progress.nextFile, progress.currentChunk)) == prefetch.end()) {
				if (!launchWindow()) { error = "Install parallel download worker could not start"; valid = false; break; }
				if (!harvestWindow()) { error = "Install parallel download worker failed"; valid = false; break; }
			}
			// refill straight away so the next window downloads while this one is written
			if (!launchWindow()) { error = "Install parallel download worker could not start"; valid = false; break; }

			// take whatever of the window belongs to the file being written now
			std::vector<InstallChunkDownload> downloads;
			for (size_t offset = progress.currentChunk; offset < file.chunks.size(); offset++) {
				auto found = prefetch.find(std::make_pair(progress.nextFile, offset));
				if (found == prefetch.end()) break;
				downloads.push_back(std::move(found->second));
				prefetch.erase(found);
			}
			if (downloads.empty()) { error = "Install chunk window produced nothing"; valid = false; break; }
			if (installCancelRequested(metadata.appId)) { error = "Install cancelled"; valid = false; break; }
			std::vector<std::vector<uint8_t>> expanded;
			expanded.reserve(downloads.size());
			for (auto& download : downloads) {
				if (!download.ok) { error = download.error; valid = false; break; }
				expanded.push_back(std::move(download.expanded));
			}
			if (valid && !steamFullInstallWriteChunks(g_localState, metadata, progress, progress.nextFile, expanded, state.installLimits, error)) valid = false;
			if (!valid) break;
			for (size_t i = 0; i < downloads.size(); i++) {
				const auto& chunk = file.chunks[progress.currentChunk];
				progress.currentChunk++;
				progress.chunksVerified++;
				if (progress.transferredBytes > referenceTransferBytes - chunk.compressedBytes) { error = "Install transfer progress overflowed"; valid = false; break; }
				progress.transferredBytes += chunk.compressedBytes;
			}
			if (!valid) break;
			progress.updatedAt = (uint64_t)time(nullptr);
			bool cancelled = installCancelRequested(metadata.appId);
			if (installBatchNeedsCheckpoint(progress.currentChunk, file.chunks.size(), cancelled) && !steamFullInstallSaveProgress(g_localState, progress, state.installLimits)) { error = "Install progress could not be persisted"; valid = false; break; }
			if (cancelled) { error = "Install cancelled"; valid = false; break; }
			installing.transferredBytes = progress.transferredBytes;
			installing.chunksVerified = progress.chunksVerified;
			installing.filesVerified = progress.filesVerified;
			char message[160];
			double percent = referenceTransferBytes ? (100.0 * (double)progress.transferredBytes / (double)referenceTransferBytes) : 0.0;
			sprintf_s(message, "Installing Steam game %.1f%%, file %zu of %zu, %zu parallel", percent, progress.nextFile + 1, metadata.files.size(), clients.size());
			installing.message = message;
			publishInstallStatus(installing, nullptr);
			ULONGLONG now = GetTickCount64();
			if (state.loggedOn && now >= state.nextHeartbeat) {
				if (!sendPacket(ws, heartbeatPacket(state))) { error = "Steam CM heartbeat failed during install"; valid = false; break; }
				state.nextHeartbeat = now + (ULONGLONG)state.heartbeatSeconds * 1000;
			}
		}
		if (valid && !steamFullInstallFinalizeFile(g_localState, metadata, progress, state.installLimits, error)) valid = false;
		if (valid) {
			installing.filesVerified = progress.filesVerified;
			installing.message = "Verified file " + std::to_string(progress.filesVerified) + " of " + std::to_string(metadata.files.size());
			publishInstallStatus(installing, nullptr);
		}
	}
	clearManifestAuthorization(state);
	if (valid) {
		SteamInstalledRecord record;
		installing.state = "verifying";
		installing.message = "Verifying complete Steam install";
		publishInstallStatus(installing, nullptr);
		valid = steamFullInstallComplete(g_localState, metadata, progress, state.installLimits, record, error);
		if (valid) {
			SteamInstallStatus installed = installStatusFor(record, record.architecture == "x64" ? "installed" : "unsupported", record.architecture == "x64" ? "Steam game installed" : "Steam game installed but target executable is not x64");
			publishInstallStatus(installed, &record);
			state.installSucceeded = true;
			state.planDepotIndex = state.plan.depots.size();
			char line[320];
			sprintf_s(line, "steam install: app=%u files=%llu chunks=%llu bytes=%llu architecture=%s state=%s\n", record.appId, (unsigned long long)record.fileCount, (unsigned long long)record.chunkCount, (unsigned long long)record.installedBytes, record.architecture.c_str(), installed.state.c_str());
			logLine(line);
			if (g_changed) g_changed();
		}
	}
	if (!valid) {
		SteamInstallStatus paused = installing;
		paused.state = "paused";
		bool cancelled = installCancelRequested(metadata.appId);
		paused.message = cancelled ? "Install cancelled; press A to resume" : error.empty() ? "Install paused; retry to resume" : "Install paused: " + error;
		paused.transferredBytes = progress.transferredBytes;
		paused.filesVerified = progress.filesVerified;
		paused.chunksVerified = progress.chunksVerified;
		publishInstallStatus(paused, nullptr);
		char line[384];
		sprintf_s(line, "steam install: state=paused files=%llu/%llu chunks=%llu/%llu bytes=%llu/%llu reason=%s\n", (unsigned long long)progress.filesVerified, (unsigned long long)metadata.files.size(), (unsigned long long)progress.chunksVerified, (unsigned long long)manifestChunkCount(metadata), (unsigned long long)progress.transferredBytes, (unsigned long long)referenceTransferBytes, error.empty() ? "unknown" : error.c_str());
		logLine(line);
		clearInstallCancel(metadata.appId);
	}
	clearInstallDepotKeys(state);
	steamManifestSecureClear(metadata);
	state.planDeadline = 0;
	return advanceContentPlan(state, steamId);
}
std::vector<uint32_t> contentDepotGrants(const ConnectionState& state) {
	std::set<uint32_t> granted;
	AcquireSRWLockShared(&g_lock);
	granted = g_entitledDepots;
	ReleaseSRWLockShared(&g_lock);
	for (const auto& package : state.resolvedPackages) granted.insert(package.depotIds.begin(), package.depotIds.end());
	return std::vector<uint32_t>(granted.begin(), granted.end());
}

bool startContentPlan(ConnectionState& state, const std::string& steamId, uint32_t appId) {
	SteamAppMetadata metadata;
	std::map<uint32_t, SteamAppMetadata> appMetadata;
	bool entitled = false;
	AcquireSRWLockShared(&g_lock);
	auto found = g_appMetadata.find(appId);
	if (found != g_appMetadata.end()) metadata = found->second;
	appMetadata = g_appMetadata;
	entitled = g_entitledApps.find(appId) != g_entitledApps.end();
	ReleaseSRWLockShared(&g_lock);
	std::vector<uint32_t> grantedDepots = contentDepotGrants(state);
	bool built = steamBuildInstallPlan(metadata, appMetadata, entitled, grantedDepots, state.plan);
	clearManifestAuthorization(state);
	clearInstallDepotKeys(state);
	state.installManifests.clear();
	state.installMetadata = SteamManifestMetadata();
	state.installLimits = SteamFullInstallLimits();
	state.installAttempted = false;
	state.installSucceeded = false;
	state.planDepotIndex = 0;
	state.planServers.clear();
	if (!built || state.plan.directDepotCount == 0) return finishContentPlan(state, steamId);
	state.planStage = PlanStage::NeedServers;
	publishInstallPlan(steamId, state.plan, SteamCmState::PlanningContent, state.plan.message);
	if (g_changed) g_changed();
	return true;
}

bool consumePendingPlan(ConnectionState& state, const std::string& steamId) {
	if (!state.loggedOn || (state.planStage != PlanStage::Idle && state.planStage != PlanStage::Complete && state.planStage != PlanStage::Error)) return true;
	if (state.picsStage != PicsStage::Complete) {
		AcquireSRWLockShared(&g_lock);
		bool cached = g_entitlementSteamId == steamId && g_appMetadataSteamId == steamId && !g_entitledDepots.empty() && !g_appMetadata.empty();
		bool sharedMetadataMissing = false;
		auto requested = g_appMetadata.find(g_pendingPlanAppId);
		if (requested != g_appMetadata.end()) {
			for (const auto& depot : requested->second.publicDepots) {
				if (depot.depotFromApp && g_appMetadata.find(depot.depotFromApp) == g_appMetadata.end()) { sharedMetadataMissing = true; break; }
			}
		}
		ReleaseSRWLockShared(&g_lock);
		if (!cached || sharedMetadataMissing) return true;
	}
	uint32_t appId = 0;
	ULONGLONG queuedAt = 0;
	AcquireSRWLockExclusive(&g_lock);
	appId = g_pendingPlanAppId;
	queuedAt = g_pendingPlanQueuedAt;
	g_pendingPlanAppId = 0;
	g_pendingPlanQueuedAt = 0;
	ReleaseSRWLockExclusive(&g_lock);
	if (appId) {
		char line[128];
		sprintf_s(line, "steam plan: dequeued app %u after %llums\n", appId, queuedAt ? GetTickCount64() - queuedAt : 0);
		logLine(line);
	}
	return appId ? startContentPlan(state, steamId, appId) : true;
}

bool prepareNextAppBatch(ConnectionState& state, const std::string& steamId);

bool finishEntitlementResolution(ConnectionState& state, const std::string& steamId) {
	if (!state.licenses.empty() && state.resolvedPackages.empty()) {
		setStatus(SteamCmState::Error, "Steam package product information could not be resolved");
		return false;
	}
	std::set<uint32_t> entitledApps;
	if (!saveEntitlements(steamId, state, entitledApps)) {
		setStatus(SteamCmState::Error, "Steam entitlement map could not be cached");
		return false;
	}
	std::set<uint32_t> unresolvedPackages = state.deniedPackages;
	unresolvedPackages.insert(state.missingTokenPackages.begin(), state.missingTokenPackages.end());
	unresolvedPackages.insert(state.unknownPackages.begin(), state.unknownPackages.end());
	unresolvedPackages.insert(state.invalidPackages.begin(), state.invalidPackages.end());
	size_t unresolved = unresolvedPackages.size();
	std::set<uint32_t> entitledDepots;
	for (const auto& package : state.resolvedPackages) entitledDepots.insert(package.depotIds.begin(), package.depotIds.end());
	AcquireSRWLockExclusive(&g_lock);
	g_entitlementSteamId = steamId;
	g_entitledApps = entitledApps;
	g_entitledDepots = std::move(entitledDepots);
	g_status.state = SteamCmState::ResolvingAppTokens;
	g_status.message = "Steam package entitlements resolved; preparing catalog app metadata";
	g_status.licenseCount = (int)state.licenses.size();
	g_status.resolvedPackageCount = (int)state.resolvedPackages.size();
	g_status.entitlementAppCount = (int)entitledApps.size();
	ReleaseSRWLockExclusive(&g_lock);
	char line[192];
	sprintf_s(line, "steam cm: resolved %zu packages to %zu entitled apps; %zu unresolved\n", state.resolvedPackages.size(), entitledApps.size(), unresolved);
	logLine(line);
	if (g_changed) g_changed();
	if (!readCatalogAppIds(steamId, state.catalogAppIds) || state.catalogAppIds.empty()) {
		state.picsStage = PicsStage::Complete;
		setStatus(SteamCmState::Online, "Steam package entitlements resolved; catalog app metadata unavailable");
		return true;
	}
	state.appBatchOffset = 0;
	state.resolvedApps.clear();
	state.deniedApps.clear();
	state.unknownApps.clear();
	state.missingTokenApps.clear();
	state.invalidApps.clear();
	state.appHttpHost.clear();
	return prepareNextAppBatch(state, steamId);
}

bool finishAppInfoResolution(ConnectionState& state, const std::string& steamId) {
	if (!state.catalogAppIds.empty() && state.resolvedApps.empty()) {
		setStatus(SteamCmState::Error, "Steam catalog app metadata could not be resolved");
		return false;
	}
	std::map<uint32_t, SteamAppMetadata> metadata;
	if (!saveAppInfo(steamId, state, metadata)) {
		setStatus(SteamCmState::Error, "Steam app metadata could not be cached");
		return false;
	}
	std::set<uint32_t> unresolvedApps = state.deniedApps;
	unresolvedApps.insert(state.missingTokenApps.begin(), state.missingTokenApps.end());
	unresolvedApps.insert(state.unknownApps.begin(), state.unknownApps.end());
	unresolvedApps.insert(state.invalidApps.begin(), state.invalidApps.end());
	size_t unresolved = unresolvedApps.size();
	char message[160];
	if (unresolved) sprintf_s(message, "Steam app metadata ready; %zu catalog apps unresolved", unresolved);
	else strcpy_s(message, "Steam app metadata ready");
	AcquireSRWLockExclusive(&g_lock);
	g_appMetadataSteamId = steamId;
	g_appMetadata = metadata;
	if (g_manifestStatus.state == "ready" && !manifestMatchesCatalog(g_manifestMetadata, steamId, metadata)) {
		g_manifestStatus = manifestStatusFor(g_manifestMetadata, "expired", "Manifest expired: public build identity changed");
		g_manifestMetadata = SteamManifestMetadata();
		DeleteFileW(manifestMetadataPath().c_str());
	}
	g_status.state = SteamCmState::Online;
	g_status.message = message;
	g_status.appMetadataCount = (int)metadata.size();
	ReleaseSRWLockExclusive(&g_lock);
	state.picsStage = PicsStage::Complete;
	char line[192];
	sprintf_s(line, "steam cm: resolved app metadata for %zu of %zu catalog apps; %zu unresolved\n", metadata.size(), state.catalogAppIds.size(), unresolved);
	logLine(line);
	if (g_changed) g_changed();
	return true;
}

bool prepareNextAppBatch(ConnectionState& state, const std::string& steamId) {
	state.batchAppIds.clear();
	for (auto& token : state.batchAppTokens) SecureZeroMemory(&token.second, sizeof(token.second));
	state.batchAppTokens.clear();
	state.batchAppSeen.clear();
	while (state.appBatchOffset < state.catalogAppIds.size() && state.batchAppIds.size() < kAppPicsBatchSize) {
		uint32_t appId = state.catalogAppIds[state.appBatchOffset++];
		if (appId) state.batchAppIds.push_back(appId);
	}
	if (state.batchAppIds.empty()) {
		if (state.appBatchOffset < state.catalogAppIds.size()) return prepareNextAppBatch(state, steamId);
		std::set<uint32_t> requested(state.catalogAppIds.begin(), state.catalogAppIds.end());
		for (const auto& app : state.resolvedApps) {
			for (const auto& depot : app.publicDepots) {
				if (depot.depotFromApp && requested.insert(depot.depotFromApp).second) state.catalogAppIds.push_back(depot.depotFromApp);
			}
		}
		if (state.appBatchOffset < state.catalogAppIds.size()) return prepareNextAppBatch(state, steamId);
		return finishAppInfoResolution(state, steamId);
	}
	state.picsStage = PicsStage::NeedAppTokenRequest;
	return true;
}

bool prepareNextPicsBatch(ConnectionState& state, const std::string& steamId) {
	state.batchPackageIds.clear();
	for (auto& token : state.batchTokens) SecureZeroMemory(&token.second, sizeof(token.second));
	state.batchTokens.clear();
	state.batchSeen.clear();
	while (state.picsBatchOffset < state.licenses.size() && state.batchPackageIds.size() < kPicsBatchSize) {
		const LicenseRecord& license = state.licenses[state.picsBatchOffset++];
		if (!license.packageId) continue;
		state.batchPackageIds.push_back(license.packageId);
		if (license.accessToken) state.batchTokens[license.packageId] = license.accessToken;
	}
	if (state.batchPackageIds.empty()) {
		if (state.picsBatchOffset < state.licenses.size()) return prepareNextPicsBatch(state, steamId);
		return finishEntitlementResolution(state, steamId);
	}
	state.picsStage = PicsStage::NeedPackageTokenRequest;
	return true;
}

bool sendPendingPicsRequest(WebSocket& ws, ConnectionState& state) {
	if (state.picsStage == PicsStage::NeedPackageTokenRequest) {
		state.tokenJobId = state.nextJobId++;
		if (!sendPacket(ws, picsAccessTokenPacket(state, state.batchPackageIds, state.tokenJobId))) return false;
		state.picsDeadline = GetTickCount64() + kPicsRequestTimeoutMs;
		state.picsStage = PicsStage::WaitingForPackageTokens;
		char message[160];
		sprintf_s(message, "Resolving Steam package tokens %zu of %zu", state.picsBatchOffset, state.licenses.size());
		setStatus(SteamCmState::ResolvingTokens, message);
	} else if (state.picsStage == PicsStage::NeedPackageProductRequest) {
		std::vector<SteamPicsPackageRequest> packages;
		packages.reserve(state.batchPackageIds.size());
		for (uint32_t packageId : state.batchPackageIds) {
			SteamPicsPackageRequest request;
			request.packageId = packageId;
			auto token = state.batchTokens.find(packageId);
			if (token != state.batchTokens.end()) request.accessToken = token->second;
			packages.push_back(request);
		}
		state.productJobId = state.nextJobId++;
		std::vector<uint8_t> packet = picsProductInfoPacket(state, packages, state.productJobId);
		for (auto& package : packages) SecureZeroMemory(&package.accessToken, sizeof(package.accessToken));
		bool sent = sendPacket(ws, packet);
		if (!packet.empty()) SecureZeroMemory(packet.data(), packet.size());
		if (!sent) return false;
		state.picsDeadline = GetTickCount64() + kPicsRequestTimeoutMs;
		state.picsStage = PicsStage::WaitingForPackageProducts;
		char message[160];
		sprintf_s(message, "Resolving Steam package grants %zu of %zu", state.picsBatchOffset, state.licenses.size());
		setStatus(SteamCmState::ResolvingPackages, message);
	} else if (state.picsStage == PicsStage::NeedAppTokenRequest) {
		state.tokenJobId = state.nextJobId++;
		if (!sendPacket(ws, appAccessTokenPacket(state, state.batchAppIds, state.tokenJobId))) return false;
		state.picsDeadline = GetTickCount64() + kPicsRequestTimeoutMs;
		state.picsStage = PicsStage::WaitingForAppTokens;
		char message[160];
		sprintf_s(message, "Resolving Steam app tokens %zu of %zu", state.appBatchOffset, state.catalogAppIds.size());
		setStatus(SteamCmState::ResolvingAppTokens, message);
	} else if (state.picsStage == PicsStage::NeedAppProductRequest) {
		std::vector<SteamAppRequest> apps;
		apps.reserve(state.batchAppIds.size());
		for (uint32_t appId : state.batchAppIds) {
			SteamAppRequest request;
			request.appId = appId;
			auto token = state.batchAppTokens.find(appId);
			if (token != state.batchAppTokens.end()) request.accessToken = token->second;
			apps.push_back(request);
		}
		state.productJobId = state.nextJobId++;
		std::vector<uint8_t> packet = appProductInfoPacket(state, apps, state.productJobId);
		for (auto& app : apps) SecureZeroMemory(&app.accessToken, sizeof(app.accessToken));
		bool sent = sendPacket(ws, packet);
		if (!packet.empty()) SecureZeroMemory(packet.data(), packet.size());
		if (!sent) return false;
		state.picsDeadline = GetTickCount64() + kPicsRequestTimeoutMs;
		state.picsStage = PicsStage::WaitingForAppProducts;
		char message[160];
		sprintf_s(message, "Resolving Steam app metadata %zu of %zu", state.appBatchOffset, state.catalogAppIds.size());
		setStatus(SteamCmState::ResolvingApps, message);
	}
	return true;
}

bool sendPendingPlanRequest(WebSocket& ws, ConnectionState& state, const std::string& steamId) {
	if (state.planStage == PlanStage::NeedServers) {
		std::vector<uint8_t> body = steamContentMakeServerRequest(state.cellId, 20);
		state.planJobId = state.nextJobId++;
		if (!sendPacket(ws, servicePacket(state, "ContentServerDirectory.GetServersForSteamPipe#1", body, state.planJobId))) return false;
		state.planStage = PlanStage::WaitingForServers;
		state.planDeadline = GetTickCount64() + kPicsRequestTimeoutMs;
		state.plan.message = "Discovering authorized content servers";
		publishInstallPlan(steamId, state.plan, SteamCmState::PlanningContent, state.plan.message);
		return true;
	}
	if (state.planStage == PlanStage::RetrieveInstall) return retrieveAuthorizedInstall(ws, state, steamId);
	if (state.planDepotIndex >= state.plan.depots.size()) return finishContentPlan(state, steamId);
	SteamInstallPlanDepot& depot = state.plan.depots[state.planDepotIndex];
	if (state.planStage == PlanStage::NeedDepotKey) {
		state.planJobId = state.nextJobId++;
		if (!sendPacket(ws, depotKeyPacket(state, depot.contentAppId, depot.depotId, state.planJobId))) return false;
		state.planStage = PlanStage::WaitingForDepotKey;
		state.planDeadline = GetTickCount64() + kPicsRequestTimeoutMs;
		state.plan.message = "Authorizing depot key " + std::to_string(depot.depotId);
		publishInstallPlan(steamId, state.plan, SteamCmState::PlanningContent, state.plan.message);
		return true;
	}
	if (state.planStage == PlanStage::NeedManifestCode) {
		std::vector<uint8_t> body = steamContentMakeManifestCodeRequest(depot.contentAppId, depot.depotId, depot.manifestId);
		state.planJobId = state.nextJobId++;
		if (!sendPacket(ws, servicePacket(state, "ContentServerDirectory.GetManifestRequestCode#1", body, state.planJobId))) return false;
		state.planStage = PlanStage::WaitingForManifestCode;
		state.planDeadline = GetTickCount64() + kPicsRequestTimeoutMs;
		state.plan.message = "Authorizing public manifest " + std::to_string(depot.depotId);
		publishInstallPlan(steamId, state.plan, SteamCmState::PlanningContent, state.plan.message);
		return true;
	}
	if (state.planStage == PlanStage::RetrieveManifest) return retrieveAuthorizedManifest(state, steamId);
	return true;
}

bool processMessage(const uint8_t* data, size_t size, ConnectionState& state, const std::string& accountSteamId);

bool processMulti(const Packet& packet, ConnectionState& state, const std::string& accountSteamId) {
	uint64_t uncompressedSize = 0;
	const uint8_t* payload = nullptr;
	size_t payloadSize = 0;
	size_t offset = 0;
	while (offset < packet.bodySize) {
		uint64_t key = 0;
		if (!getVarint(packet.body, packet.bodySize, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if (field == 1 && wire == 0) {
			if (!getVarint(packet.body, packet.bodySize, offset, uncompressedSize)) return false;
		} else if (field == 2 && wire == 2) {
			if (!getBytes(packet.body, packet.bodySize, offset, payload, payloadSize)) return false;
		} else if (!skipField(packet.body, packet.bodySize, offset, wire)) return false;
	}
	if (!payload) return false;
	std::vector<uint8_t> expanded;
	ByteVectorGuard expandedGuard{ &expanded };
	if (uncompressedSize) {
		if (!inflateGzip(payload, payloadSize, (size_t)uncompressedSize, expanded)) return false;
		payload = expanded.data();
		payloadSize = expanded.size();
	}
	offset = 0;
	while (offset < payloadSize) {
		if (payloadSize - offset < 4) return false;
		uint32_t length = (uint32_t)payload[offset] | ((uint32_t)payload[offset + 1] << 8) | ((uint32_t)payload[offset + 2] << 16) | ((uint32_t)payload[offset + 3] << 24);
		offset += 4;
		if (!length || length > payloadSize - offset) return false;
		if (!processMessage(payload + offset, length, state, accountSteamId)) return false;
		offset += length;
	}
	return true;
}

bool processMessage(const uint8_t* data, size_t size, ConnectionState& state, const std::string& accountSteamId) {
	if (size < 4) return false;
	uint32_t raw = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
	if (!(raw & kProtoMask)) return true;
	Packet packet;
	if (!parsePacket(data, size, packet)) return false;
	if (packet.message == kMsgMulti) return processMulti(packet, state, accountSteamId);
	if (packet.message == kMsgLogOnResponse) {
		uint64_t result = 2, heartbeat = 30, cellId = 0;
		readUIntField(packet.body, packet.bodySize, 1, result);
		if (result != 1) {
			if (result == 20 || result == 48) {
				setStatus(SteamCmState::Connecting, "Steam CM requested another server");
				return false;
			}
			char line[128];
			sprintf_s(line, "Steam CM logon failed with EResult %llu", (unsigned long long)result);
			setStatus(SteamCmState::Error, line);
			return false;
		}
		if (!readUIntField(packet.body, packet.bodySize, 3, heartbeat)) readUIntField(packet.body, packet.bodySize, 2, heartbeat);
		readUIntField(packet.body, packet.bodySize, 7, cellId);
		if (heartbeat < 10 || heartbeat > 120) heartbeat = 30;
		state.steamId = packet.steamId;
		state.sessionId = packet.sessionId;
		state.cellId = cellId <= 0xffffffffull ? (uint32_t)cellId : 0;
		state.heartbeatSeconds = (int)heartbeat;
		state.nextHeartbeat = GetTickCount64() + heartbeat * 1000;
		state.loggedOn = true;
		setStatus(SteamCmState::WaitingForLicenses, "Steam CM connected; waiting for package licenses");
		return true;
	}
	if (packet.message == kMsgLicenseList) {
		int result = 2;
		std::vector<LicenseRecord> licenses;
		LicenseVectorGuard licenseGuard{ &licenses };
		if (!parseLicenseList(packet.body, packet.bodySize, result, licenses) || result != 1) {
			setStatus(SteamCmState::Error, "Steam CM license list was invalid");
			return false;
		}
		std::map<uint32_t, LicenseRecord> unique;
		for (const auto& license : licenses) if (license.packageId) unique[license.packageId] = license;
		for (auto& license : licenses) SecureZeroMemory(&license.accessToken, sizeof(license.accessToken));
		licenses.clear();
		for (auto& license : unique) {
			licenses.push_back(license.second);
			SecureZeroMemory(&license.second.accessToken, sizeof(license.second.accessToken));
		}
		if (!saveLicenses(accountSteamId, licenses)) {
			setStatus(SteamCmState::Error, "Steam package licenses could not be cached");
			return false;
		}
		state.licensesReceived = true;
		for (auto& license : state.licenses) SecureZeroMemory(&license.accessToken, sizeof(license.accessToken));
		state.licenses = std::move(licenses);
		state.picsBatchOffset = 0;
		state.resolvedPackages.clear();
		state.deniedPackages.clear();
		state.unknownPackages.clear();
		state.missingTokenPackages.clear();
		state.invalidPackages.clear();
		setStatus(SteamCmState::ResolvingTokens, "Steam package licenses received; preparing PICS resolution", (int)state.licenses.size());
		char line[160];
		sprintf_s(line, "steam cm: cached %zu package licenses; starting PICS resolution\n", state.licenses.size());
		logLine(line);
		return prepareNextPicsBatch(state, accountSteamId);
	}
	if (packet.message == kMsgDepotKeyResponse) {
		if (state.planStage != PlanStage::WaitingForDepotKey || packet.targetJobId != state.planJobId) return true;
		SteamDepotKeyResponse response;
		bool parsed = steamContentParseDepotKeyResponse(packet.body, packet.bodySize, response);
		bool granted = parsed && response.result == 1 && state.planDepotIndex < state.plan.depots.size() && response.depotId == state.plan.depots[state.planDepotIndex].depotId && response.key.size() == 32;
		if (!granted) {
			if (!response.key.empty()) SecureZeroMemory(response.key.data(), response.key.size());
			return failContentDepot(state, accountSteamId, "depot_key_denied");
		}
		clearManifestAuthorization(state);
		state.manifestDepotKey.swap(response.key);
		if (!response.key.empty()) SecureZeroMemory(response.key.data(), response.key.size());
		state.plan.depots[state.planDepotIndex].keyAuthorized = true;
		state.plan.depots[state.planDepotIndex].state = "depot_key_authorized";
		state.planStage = PlanStage::NeedManifestCode;
		state.planDeadline = 0;
		return true;
	}
	if (packet.message == kMsgServiceMethodResponse) {
		if (packet.targetJobId != state.planJobId) return true;
		if (state.planStage == PlanStage::WaitingForServers) {
			std::vector<SteamContentServer> servers;
			bool parsed = packet.result == 1 && steamContentParseServerResponse(packet.body, packet.bodySize, servers);
			state.planServers.clear();
			if (parsed) {
				for (auto& server : servers) {
					if (server.steamChinaOnly || server.useAsProxy || (server.httpsSupport != "optional" && server.httpsSupport != "mandatory") || !std::isfinite(server.weightedLoad) || !validContentHost(server.host) || !serverAllowsPlan(server, state.plan)) continue;
					state.planServers.push_back(std::move(server));
				}
			}
			if (state.planServers.empty()) {
				for (auto& depot : state.plan.depots) if (depot.manifestId && depot.state == "pending_authorization") { depot.state = "server_unavailable"; state.plan.unresolvedDepotCount++; }
				state.planDepotIndex = state.plan.depots.size();
				return finishContentPlan(state, accountSteamId);
			}
			std::sort(state.planServers.begin(), state.planServers.end(), [](const auto& left, const auto& right) { return left.weightedLoad < right.weightedLoad; });
			state.plan.serverHost = state.planServers[0].host;
			state.planDepotIndex = 0;
			state.planDeadline = 0;
			return advanceContentPlan(state, accountSteamId);
		}
		if (state.planStage == PlanStage::WaitingForManifestCode) {
			SteamManifestCodeResponse response;
			bool granted = packet.result == 1 && steamContentParseManifestCodeResponse(packet.body, packet.bodySize, response) && response.code != 0;
			if (!granted) {
				SecureZeroMemory(&response.code, sizeof(response.code));
				return failContentDepot(state, accountSteamId, "manifest_code_denied");
			}
			SteamInstallPlanDepot& depot = state.plan.depots[state.planDepotIndex];
			depot.manifestCodeGranted = true;
			depot.cdnAuthChecked = true;
			depot.state = "authorized";
			state.plan.authorizedDepotCount++;
			if (state.manifestDepotKey.size() == 32) {
				state.manifestRequestCode = response.code;
				SecureZeroMemory(&response.code, sizeof(response.code));
				state.planStage = PlanStage::RetrieveManifest;
				state.planDeadline = 0;
				return true;
			}
			SecureZeroMemory(&response.code, sizeof(response.code));
			state.planDepotIndex++;
			state.planDeadline = 0;
			return advanceContentPlan(state, accountSteamId);
		}
		return true;
	}
	if (packet.message == kMsgPicsAccessTokenResponse) {
		if (packet.targetJobId != state.tokenJobId) return true;
		if (state.picsStage == PicsStage::WaitingForPackageTokens) {
			SteamPicsTokenResponse response;
			if (!steamPicsParseAccessTokenResponse(packet.body, packet.bodySize, response)) {
				setStatus(SteamCmState::Error, "Steam PICS package-token response was invalid");
				return false;
			}
			for (auto& token : response.packageTokens) {
				if (inCurrentBatch(state, token.first) && token.second) state.batchTokens[token.first] = token.second;
				SecureZeroMemory(&token.second, sizeof(token.second));
			}
			for (uint32_t packageId : response.deniedPackageIds) if (inCurrentBatch(state, packageId)) state.deniedPackages.insert(packageId);
			state.picsStage = PicsStage::NeedPackageProductRequest;
			state.picsDeadline = 0;
			return true;
		}
		if (state.picsStage == PicsStage::WaitingForAppTokens) {
			SteamAppTokenResponse response;
			if (!steamAppParseAccessTokenResponse(packet.body, packet.bodySize, response)) {
				setStatus(SteamCmState::Error, "Steam PICS app-token response was invalid");
				return false;
			}
			for (auto& token : response.appTokens) {
				if (inCurrentAppBatch(state, token.first) && token.second) state.batchAppTokens[token.first] = token.second;
				SecureZeroMemory(&token.second, sizeof(token.second));
			}
			for (uint32_t appId : response.deniedAppIds) if (inCurrentAppBatch(state, appId)) state.deniedApps.insert(appId);
			state.picsStage = PicsStage::NeedAppProductRequest;
			state.picsDeadline = 0;
		}
		return true;
	}
	if (packet.message == kMsgPicsProductInfoResponse) {
		if (packet.targetJobId != state.productJobId) return true;
		if (state.picsStage == PicsStage::WaitingForPackageProducts) {
			SteamPicsProductInfoResponse response;
			if (!steamPicsParseProductInfoResponse(packet.body, packet.bodySize, response)) {
				setStatus(SteamCmState::Error, "Steam PICS package product-info response was invalid");
				return false;
			}
			for (auto& package : response.packages) {
				if (!inCurrentBatch(state, package.packageId)) continue;
				state.batchSeen.insert(package.packageId);
				if (package.missingToken) {
					state.missingTokenPackages.insert(package.packageId);
					continue;
				}
				if (!package.hasBuffer || !package.bufferValid) {
					state.invalidPackages.insert(package.packageId);
					continue;
				}
				state.deniedPackages.erase(package.packageId);
				state.unknownPackages.erase(package.packageId);
				state.missingTokenPackages.erase(package.packageId);
				state.invalidPackages.erase(package.packageId);
				storeResolvedPackage(state, std::move(package));
			}
			for (uint32_t packageId : response.unknownPackageIds) {
				if (!inCurrentBatch(state, packageId)) continue;
				state.batchSeen.insert(packageId);
				state.unknownPackages.insert(packageId);
			}
			state.picsDeadline = GetTickCount64() + kPicsRequestTimeoutMs;
			if (response.responsePending) return true;
			for (uint32_t packageId : state.batchPackageIds) if (!state.batchSeen.count(packageId) && !state.deniedPackages.count(packageId)) state.invalidPackages.insert(packageId);
			state.picsDeadline = 0;
			return prepareNextPicsBatch(state, accountSteamId);
		}
		if (state.picsStage == PicsStage::WaitingForAppProducts) {
			SteamAppProductInfoResponse response;
			if (!steamAppParseProductInfoResponse(packet.body, packet.bodySize, response)) {
				setStatus(SteamCmState::Error, "Steam PICS app product-info response was invalid");
				return false;
			}
			if (!response.httpHost.empty()) state.appHttpHost = response.httpHost;
			for (auto& app : response.apps) {
				if (!inCurrentAppBatch(state, app.appId)) continue;
				state.batchAppSeen.insert(app.appId);
				if (app.missingToken) {
					state.missingTokenApps.insert(app.appId);
					continue;
				}
				if (!app.bufferValid && !app.hasBuffer && !app.shaHex.empty() && !state.appHttpHost.empty()) fetchAppInfoHttp(app, state.appHttpHost);
				if (!app.bufferValid) {
					state.invalidApps.insert(app.appId);
					continue;
				}
				state.deniedApps.erase(app.appId);
				state.unknownApps.erase(app.appId);
				state.missingTokenApps.erase(app.appId);
				state.invalidApps.erase(app.appId);
				storeResolvedApp(state, std::move(app));
			}
			for (uint32_t appId : response.unknownAppIds) {
				if (!inCurrentAppBatch(state, appId)) continue;
				state.batchAppSeen.insert(appId);
				state.unknownApps.insert(appId);
			}
			state.picsDeadline = GetTickCount64() + kPicsRequestTimeoutMs;
			if (response.responsePending) return true;
			for (uint32_t appId : state.batchAppIds) if (!state.batchAppSeen.count(appId) && !state.deniedApps.count(appId)) state.invalidApps.insert(appId);
			state.picsDeadline = 0;
			return prepareNextAppBatch(state, accountSteamId);
		}
		return true;
	}
	if (packet.message == kMsgLoggedOff) {
		uint64_t result = 2;
		readUIntField(packet.body, packet.bodySize, 1, result);
		char line[128];
		sprintf_s(line, "Steam CM logged off with EResult %llu", (unsigned long long)result);
		setStatus(SteamCmState::Error, line);
		return false;
	}
	return true;
}

DWORD WINAPI cmWorker(LPVOID parameter) {
	std::unique_ptr<WorkerArgs> args((WorkerArgs*)parameter);
	std::vector<std::string> servers = discoverServers();
	if (servers.empty()) servers.push_back("cmp1-sea1.steamserver.net:443");
	for (size_t attempt = 0; attempt < servers.size() && attempt < 5 && !InterlockedCompareExchange(&g_stop, 0, 0); attempt++) {
		setStatus(SteamCmState::Connecting, "Connecting to Steam CM");
		WebSocket ws;
		if (!connectWebSocket(servers[attempt], ws)) continue;
		if (!sendPacket(ws, helloPacket())) continue;
		std::vector<uint8_t> logon = logonPacket(*args);
		bool logonSent = sendPacket(ws, logon);
		if (!logon.empty()) SecureZeroMemory(logon.data(), logon.size());
		if (!logonSent) continue;
		setStatus(SteamCmState::LoggingOn, "Logging on to Steam CM");
		ConnectionState state;
		state.nextJobId = (GetTickCount64() << 16) | 1;
		std::future<ReceiveResult> receive = beginReceive(ws.socket);
		for (;;) {
			if (InterlockedCompareExchange(&g_stop, 0, 0)) {
				if (state.plan.appId && state.planStage != PlanStage::Complete && state.planStage != PlanStage::Error) {
					AcquireSRWLockExclusive(&g_lock);
					if (!g_pendingPlanAppId) g_pendingPlanAppId = state.plan.appId;
					g_installPlan = state.plan;
					g_installPlan.state = "queued";
					g_installPlan.message = "Authorization plan queued after CM restart";
					ReleaseSRWLockExclusive(&g_lock);
				}
				finishReceive(ws, receive);
				InterlockedExchange(&g_workerActive, 0);
				clearSecret(args->refreshToken);
				return 0;
			}
			if (state.picsStage == PicsStage::NeedPackageTokenRequest || state.picsStage == PicsStage::NeedPackageProductRequest || state.picsStage == PicsStage::NeedAppTokenRequest || state.picsStage == PicsStage::NeedAppProductRequest) {
				if (!sendPendingPicsRequest(ws, state)) break;
			}
			if (!consumePendingPlan(state, args->steamId)) break;
			if (state.plan.appId && installCancelRequested(state.plan.appId) && !cancelContentPlan(state, args->steamId)) break;
			if (state.planStage == PlanStage::NeedServers || state.planStage == PlanStage::NeedDepotKey || state.planStage == PlanStage::NeedManifestCode || state.planStage == PlanStage::RetrieveManifest || state.planStage == PlanStage::RetrieveInstall) {
				if (!sendPendingPlanRequest(ws, state, args->steamId)) break;
			}
			ULONGLONG now = GetTickCount64();
			if ((state.picsStage == PicsStage::WaitingForPackageTokens || state.picsStage == PicsStage::WaitingForPackageProducts || state.picsStage == PicsStage::WaitingForAppTokens || state.picsStage == PicsStage::WaitingForAppProducts) && state.picsDeadline && now >= state.picsDeadline) {
				setStatus(SteamCmState::Error, "Steam PICS request timed out");
				break;
			}
			if ((state.planStage == PlanStage::WaitingForServers || state.planStage == PlanStage::WaitingForDepotKey || state.planStage == PlanStage::WaitingForManifestCode) && state.planDeadline && now >= state.planDeadline) {
				bool recovered = true;
				if (state.planStage == PlanStage::WaitingForServers) {
					for (auto& depot : state.plan.depots) if (depot.manifestId && depot.state == "pending_authorization") { depot.state = "server_timeout"; state.plan.unresolvedDepotCount++; }
					state.planDepotIndex = state.plan.depots.size();
					recovered = finishContentPlan(state, args->steamId);
				} else recovered = failContentDepot(state, args->steamId, "authorization_timeout");
				if (!recovered) break;
			}
			if (state.loggedOn && now >= state.nextHeartbeat) {
				if (!sendPacket(ws, heartbeatPacket(state))) break;
				state.nextHeartbeat = now + (ULONGLONG)state.heartbeatSeconds * 1000;
			}
			if (receive.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
				Sleep(10);
				continue;
			}
			ReceiveResult result = receive.get();
			if (!result.ok) break;
			receive = beginReceive(ws.socket);
			ByteVectorGuard messageGuard{ &result.message };
			if (result.timedOut) continue;
			if (!processMessage(result.message.data(), result.message.size(), state, args->steamId)) break;
		}
		finishReceive(ws, receive);
		if (!InterlockedCompareExchange(&g_stop, 0, 0) && currentState() != SteamCmState::Error && state.plan.appId && state.planStage != PlanStage::Complete && state.planStage != PlanStage::Error) {
			AcquireSRWLockExclusive(&g_lock);
			if (!g_pendingPlanAppId) g_pendingPlanAppId = state.plan.appId;
			g_installPlan = state.plan;
			g_installPlan.state = "queued";
			g_installPlan.message = "Authorization plan queued after CM reconnect";
			ReleaseSRWLockExclusive(&g_lock);
		}
		if (currentState() == SteamCmState::Error) break;
	}
	if (!InterlockedCompareExchange(&g_stop, 0, 0) && currentState() != SteamCmState::Error) setStatus(SteamCmState::Error, "Steam CM connection failed");
	clearSecret(args->refreshToken);
	InterlockedExchange(&g_workerActive, 0);
	return 0;
}

void stopWorker() {
	InterlockedExchange(&g_stop, 1);
	for (int i = 0; i < 1200 && InterlockedCompareExchange(&g_workerActive, 0, 0); i++) Sleep(50);
}

}

void steamCmInit(const wchar_t* localState, void (*log)(const char*), void (*onEntitlementsChanged)(void)) {
	g_localState = localState ? localState : L"";
	g_log = log;
	g_changed = onEntitlementsChanged;
	int cachedLicenses = loadCachedLicenseCount();
	int cachedPackages = 0;
	std::string cachedSteamId;
	std::set<uint32_t> cachedApps;
	std::set<uint32_t> cachedDepots;
	loadCachedEntitlements(cachedSteamId, cachedApps, cachedDepots, cachedPackages);
	std::string cachedAppSteamId;
	std::map<uint32_t, SteamAppMetadata> cachedAppMetadata;
	loadCachedAppInfo(cachedAppSteamId, cachedAppMetadata);
	std::string cachedPlanSteamId;
	SteamInstallPlan cachedPlan;
	loadCachedInstallPlan(cachedPlanSteamId, cachedPlan);
	SteamManifestMetadata cachedManifest;
	SteamManifestStatus cachedManifestStatus;
	loadCachedManifest(cachedAppSteamId, cachedAppMetadata, cachedManifest, cachedManifestStatus);
	std::map<uint32_t, SteamInstalledRecord> cachedInstalledRecords;
	std::map<uint32_t, SteamInstallStatus> cachedInstallStatuses;
	for (const auto& app : cachedAppMetadata) {
		SteamInstalledRecord record;
		if (!steamFullInstallLoadRecordAny(g_localState, app.first, record)) continue;
		cachedInstalledRecords[app.first] = record;
		cachedInstallStatuses[app.first] = installStatusFor(record, record.architecture == "x64" ? "installed" : "unsupported", record.architecture == "x64" ? "Installed content ready; runtime compatibility remains unknown" : "Installed content is not x64");
	}
	AcquireSRWLockExclusive(&g_lock);
	g_status = SteamCmStatus();
	g_status.state = SteamCmState::Offline;
	g_status.message = !cachedAppMetadata.empty() ? "Steam CM offline; cached app metadata available" : cachedApps.empty() ? (cachedLicenses ? "Steam CM offline; cached licenses available" : "Steam CM offline") : "Steam CM offline; cached package entitlements available";
	g_status.licenseCount = cachedLicenses;
	g_status.resolvedPackageCount = cachedPackages;
	g_status.entitlementAppCount = (int)cachedApps.size();
	g_status.appMetadataCount = (int)cachedAppMetadata.size();
	g_entitlementSteamId = cachedSteamId;
	g_entitledApps = std::move(cachedApps);
	g_entitledDepots = std::move(cachedDepots);
	g_appMetadataSteamId = cachedAppSteamId;
	g_appMetadata = std::move(cachedAppMetadata);
	g_installPlanSteamId = cachedPlanSteamId;
	g_installPlan = std::move(cachedPlan);
	g_manifestMetadata = std::move(cachedManifest);
	g_manifestStatus = std::move(cachedManifestStatus);
	g_installedRecords = std::move(cachedInstalledRecords);
	g_installStatuses = std::move(cachedInstallStatuses);
	g_pendingPlanAppId = 0;
	ReleaseSRWLockExclusive(&g_lock);
}

void steamCmStart(const std::string& refreshToken, const std::string& steamId, const std::string& accountName) {
	stopWorker();
	bool differentAccount = false;
	AcquireSRWLockExclusive(&g_lock);
	differentAccount = (!g_entitlementSteamId.empty() && g_entitlementSteamId != steamId) || (!g_appMetadataSteamId.empty() && g_appMetadataSteamId != steamId) || (!g_installPlanSteamId.empty() && g_installPlanSteamId != steamId) || (!g_manifestMetadata.steamId.empty() && g_manifestMetadata.steamId != steamId);
	if (differentAccount) {
		g_entitlementSteamId.clear();
		g_entitledApps.clear();
		g_entitledDepots.clear();
		g_appMetadataSteamId.clear();
		g_appMetadata.clear();
		g_installPlanSteamId.clear();
		g_installPlan = SteamInstallPlan();
		g_manifestMetadata = SteamManifestMetadata();
		g_manifestStatus = SteamManifestStatus();
		g_pendingPlanAppId = 0;
		g_status.resolvedPackageCount = 0;
		g_status.entitlementAppCount = 0;
		g_status.appMetadataCount = 0;
	}
	ReleaseSRWLockExclusive(&g_lock);
	if (differentAccount) {
		DeleteFileW(entitlementsPath().c_str());
		DeleteFileW(appInfoPath().c_str());
		DeleteFileW(installPlanPath().c_str());
		DeleteFileW(manifestMetadataPath().c_str());
	}
	if (InterlockedCompareExchange(&g_workerActive, 1, 0) != 0) return;
	InterlockedExchange(&g_stop, 0);
	WorkerArgs* args = new WorkerArgs();
	args->refreshToken = refreshToken;
	args->steamId = steamId;
	args->accountName = accountName;
	HANDLE thread = CreateThread(nullptr, 0, cmWorker, args, 0, nullptr);
	if (!thread) {
		clearSecret(args->refreshToken);
		delete args;
		InterlockedExchange(&g_workerActive, 0);
		setStatus(SteamCmState::Error, "Steam CM worker could not start");
		return;
	}
	CloseHandle(thread);
}

void steamCmRequireRelink() {
	stopWorker();
	setStatus(SteamCmState::NeedsRelink, "Relink Steam once to enable package licenses");
}

void steamCmForget() {
	stopWorker();
	DeleteFileW(licensesPath().c_str());
	DeleteFileW(entitlementsPath().c_str());
	DeleteFileW(appInfoPath().c_str());
	DeleteFileW(installPlanPath().c_str());
	DeleteFileW(manifestMetadataPath().c_str());
	AcquireSRWLockExclusive(&g_lock);
	g_entitlementSteamId.clear();
	g_entitledApps.clear();
	g_entitledDepots.clear();
	g_appMetadataSteamId.clear();
	g_appMetadata.clear();
	g_installPlanSteamId.clear();
	g_installPlan = SteamInstallPlan();
	g_manifestMetadata = SteamManifestMetadata();
	g_manifestStatus = SteamManifestStatus();
	g_pendingPlanAppId = 0;
	g_status = SteamCmStatus();
	g_status.state = SteamCmState::Offline;
	g_status.message = "Steam CM offline";
	ReleaseSRWLockExclusive(&g_lock);
	if (g_changed) g_changed();
}

SteamCmStatus steamCmStatus() {
	AcquireSRWLockShared(&g_lock);
	SteamCmStatus copy = g_status;
	ReleaseSRWLockShared(&g_lock);
	return copy;
}

bool steamCmRequestInstallPlan(uint32_t appId) {
	if (!appId || !InterlockedCompareExchange(&g_workerActive, 0, 0)) return false;
	AcquireSRWLockExclusive(&g_lock);
	auto metadata = g_appMetadata.find(appId);
	auto install = g_installStatuses.find(appId);
	auto installed = g_installedRecords.find(appId);
	bool currentBuildInstalled = metadata != g_appMetadata.end() && installed != g_installedRecords.end()
		&& installed->second.buildId == metadata->second.publicBuildId;
	bool installBusy = install != g_installStatuses.end() && (install->second.state == "downloading" || install->second.state == "verifying"
		|| ((install->second.state == "installed" || install->second.state == "unsupported") && currentBuildInstalled));
	bool allowed = metadata != g_appMetadata.end() && metadata->second.preflight != "unsupported_x86" && metadata->second.preflight != "blocked_non_windows" && g_entitledApps.find(appId) != g_entitledApps.end() && g_pendingPlanAppId == 0 && g_installPlan.state != "planning" && !installBusy;
	if (allowed) {
		InterlockedExchange(&g_cancelInstallAppId, 0);
		g_pendingPlanAppId = appId;
		g_pendingPlanQueuedAt = GetTickCount64();
		g_installPlan = SteamInstallPlan();
		g_installPlan.appId = appId;
		g_installPlan.name = metadata->second.name;
		g_installPlan.branch = "public";
		g_installPlan.buildId = metadata->second.publicBuildId;
		g_installPlan.installBytes = metadata->second.installBytes;
		g_installPlan.downloadBytes = metadata->second.downloadBytes;
		g_installPlan.state = "queued";
		g_installPlan.message = "Authorization, manifest, and full install queued";
		g_manifestMetadata = SteamManifestMetadata();
		g_manifestStatus = SteamManifestStatus();
		g_manifestStatus.state = "pending";
		g_manifestStatus.message = "Manifest pending fresh authorization";
		g_manifestStatus.appId = appId;
		g_manifestStatus.buildId = metadata->second.publicBuildId;
		SteamInstallStatus pendingInstall;
		pendingInstall.state = "pending";
		pendingInstall.message = "Full install pending fresh authorization";
		pendingInstall.appId = appId;
		pendingInstall.buildId = metadata->second.publicBuildId;
		g_installStatuses[appId] = pendingInstall;
		g_status.state = SteamCmState::PlanningContent;
		g_status.message = g_installPlan.message;
	}
	ReleaseSRWLockExclusive(&g_lock);
	if (allowed) {
		DeleteFileW(manifestMetadataPath().c_str());
	}
	if (allowed && g_changed) g_changed();
	return allowed;
}

bool steamCmCancelInstall(uint32_t appId) {
	if (!appId) return false;
	bool cancelable = false;
	bool queued = false;
	AcquireSRWLockExclusive(&g_lock);
	auto found = g_installStatuses.find(appId);
	bool activeStatus = found != g_installStatuses.end() && (found->second.state == "pending" || found->second.state == "downloading" || found->second.state == "verifying");
	bool activePlan = (g_pendingPlanAppId == appId) || (g_installPlan.appId == appId && (g_installPlan.state == "queued" || g_installPlan.state == "planning"));
	cancelable = activeStatus || activePlan;
	if (cancelable) {
		InterlockedExchange(&g_cancelInstallAppId, (LONG)appId);
		queued = g_pendingPlanAppId == appId;
		if (queued) {
			g_pendingPlanAppId = 0;
			g_pendingPlanQueuedAt = 0;
			SteamInstallStatus& status = g_installStatuses[appId];
			status.appId = appId;
			status.state = "paused";
			status.message = "Install cancelled; press A to resume";
			g_installPlan.state = "cancelled";
			g_installPlan.message = "Install cancelled; request install again to resume";
			g_status.state = SteamCmState::Online;
			g_status.message = g_installPlan.message;
			clearInstallCancel(appId);
		} else if (found != g_installStatuses.end()) {
			found->second.message = "Cancelling install";
		}
	}
	ReleaseSRWLockExclusive(&g_lock);
	if (cancelable) {
		char line[112];
		sprintf_s(line, "steam install: app=%u cancel requested%s\n", appId, queued ? " while queued" : "");
		logLine(line);
		if (g_changed) g_changed();
	}
	return cancelable;
}

bool steamCmGetInstallStatus(uint32_t appId, SteamInstallStatus& status) {
	AcquireSRWLockShared(&g_lock);
	auto found = g_installStatuses.find(appId);
	bool available = appId && found != g_installStatuses.end() && found->second.state != "none";
	if (available) status = found->second;
	ReleaseSRWLockShared(&g_lock);
	return available;
}