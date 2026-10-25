#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SteamContentServer {
	std::string type;
	std::string host;
	std::string httpsSupport;
	int32_t cellId = 0;
	float weightedLoad = 0;
	bool steamChinaOnly = false;
	bool useAsProxy = false;
	std::vector<uint32_t> allowedAppIds;
};

struct SteamDepotKeyResponse {
	int result = 2;
	uint32_t depotId = 0;
	std::vector<uint8_t> key;
};

struct SteamManifestCodeResponse {
	uint64_t code = 0;
};

std::vector<uint8_t> steamContentMakeDepotKeyRequest(uint32_t appId, uint32_t depotId);
bool steamContentParseDepotKeyResponse(const uint8_t* data, size_t size, SteamDepotKeyResponse& response);
std::vector<uint8_t> steamContentMakeServerRequest(uint32_t cellId, uint32_t maxServers);
bool steamContentParseServerResponse(const uint8_t* data, size_t size, std::vector<SteamContentServer>& servers);
std::vector<uint8_t> steamContentMakeManifestCodeRequest(uint32_t appId, uint32_t depotId, uint64_t manifestId);
bool steamContentParseManifestCodeResponse(const uint8_t* data, size_t size, SteamManifestCodeResponse& response);
