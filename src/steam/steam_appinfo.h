#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct SteamAppRequest {
	uint32_t appId = 0;
	uint64_t accessToken = 0;
};

struct SteamAppLaunchOption {
	std::string executable;
	std::string arguments;
	std::string workingDir;
	std::string description;
	std::string osList;
	std::string osArch;
};

struct SteamAppDepotManifest {
	uint32_t depotId = 0;
	uint32_t depotFromApp = 0;
	uint64_t manifestId = 0;
	uint64_t size = 0;
	uint64_t download = 0;
	bool sharedInstall = false;
	std::string osList;
	std::string language;
};

struct SteamAppMetadata {
	uint32_t appId = 0;
	uint32_t changeNumber = 0;
	uint32_t declaredSize = 0;
	bool missingToken = false;
	bool onlyPublic = false;
	bool hasBuffer = false;
	bool bufferValid = false;
	bool windowsSupported = false;
	std::string shaHex;
	std::string name;
	std::string type;
	std::string releaseState;
	std::string osList;
	std::string osArch;
	std::string controllerSupport;
	std::string developer;
	std::string publisher;
	std::string installDir;
	std::string publicBuildId;
	std::string architecture;
	std::string preflight;
	uint64_t installBytes = 0;
	uint64_t downloadBytes = 0;
	std::vector<uint32_t> windowsDepotIds;
	std::vector<SteamAppDepotManifest> publicDepots;
	std::vector<SteamAppLaunchOption> launchOptions;
};

struct SteamAppTokenResponse {
	std::vector<std::pair<uint32_t, uint64_t>> appTokens;
	std::vector<uint32_t> deniedAppIds;
};

struct SteamAppProductInfoResponse {
	bool responsePending = false;
	std::string httpHost;
	std::vector<SteamAppMetadata> apps;
	std::vector<uint32_t> unknownAppIds;
};

std::vector<uint8_t> steamAppMakeAccessTokenRequest(const std::vector<uint32_t>& appIds);
std::vector<uint8_t> steamAppMakeProductInfoRequest(const std::vector<SteamAppRequest>& apps);
bool steamAppParseAccessTokenResponse(const uint8_t* data, size_t size, SteamAppTokenResponse& response);
bool steamAppParseProductInfoResponse(const uint8_t* data, size_t size, SteamAppProductInfoResponse& response);
bool steamAppParseTextMetadata(uint32_t appId, const uint8_t* data, size_t size, SteamAppMetadata& metadata);
