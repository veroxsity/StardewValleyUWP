#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct SteamPicsPackageRequest {
	uint32_t packageId = 0;
	uint64_t accessToken = 0;
};

struct SteamPicsTokenResponse {
	std::vector<std::pair<uint32_t, uint64_t>> packageTokens;
	std::vector<uint32_t> deniedPackageIds;
};

struct SteamPicsPackageInfo {
	uint32_t packageId = 0;
	uint32_t changeNumber = 0;
	uint32_t declaredSize = 0;
	bool missingToken = false;
	bool hasBuffer = false;
	bool bufferValid = false;
	std::string billingType;
	std::string licenseType;
	std::string status;
	std::vector<uint32_t> appIds;
	std::vector<uint32_t> depotIds;
};

struct SteamPicsProductInfoResponse {
	bool responsePending = false;
	std::vector<SteamPicsPackageInfo> packages;
	std::vector<uint32_t> unknownPackageIds;
};

std::vector<uint8_t> steamPicsMakeAccessTokenRequest(const std::vector<uint32_t>& packageIds);
std::vector<uint8_t> steamPicsMakeProductInfoRequest(const std::vector<SteamPicsPackageRequest>& packages);
bool steamPicsParseAccessTokenResponse(const uint8_t* data, size_t size, SteamPicsTokenResponse& response);
bool steamPicsParseProductInfoResponse(const uint8_t* data, size_t size, SteamPicsProductInfoResponse& response);
bool steamPicsParsePackageBuffer(uint32_t packageId, const uint8_t* data, size_t size, SteamPicsPackageInfo& info);
