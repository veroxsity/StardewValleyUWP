#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SteamManifestLimits {
	size_t compressedBytes = 64u * 1024u * 1024u;
	size_t expandedBytes = 128u * 1024u * 1024u;
	size_t cacheBytes = 128u * 1024u * 1024u;
	size_t fileCount = 200000;
	size_t chunkCount = 1000000;
	size_t encryptedNameBytes = 4096;
	size_t pathBytes = 1024;
	uint32_t chunkOriginalBytes = 8u * 1024u * 1024u;
	uint32_t chunkCompressedBytes = 16u * 1024u * 1024u;
	uint64_t fileBytes = 256ull * 1024ull * 1024ull * 1024ull;
	uint64_t aggregateBytes = 1024ull * 1024ull * 1024ull * 1024ull;
};

struct SteamManifestChunk {
	std::array<uint8_t, 20> sha{};
	uint32_t checksum = 0;
	uint64_t offset = 0;
	uint32_t originalBytes = 0;
	uint32_t compressedBytes = 0;
};

struct SteamManifestFile {
	std::string path;
	std::string linkTarget;
	uint32_t sourceDepotId = 0;
	uint64_t size = 0;
	uint32_t flags = 0;
	std::array<uint8_t, 20> contentSha{};
	bool hasContentSha = false;
	std::vector<SteamManifestChunk> chunks;
};

struct SteamManifestMetadata {
	std::string steamId;
	uint32_t appId = 0;
	uint32_t depotId = 0;
	uint64_t manifestId = 0;
	std::string buildId;
	uint64_t retrievedAt = 0;
	uint32_t creationTime = 0;
	uint64_t originalBytes = 0;
	uint64_t compressedBytes = 0;
	uint32_t uniqueChunks = 0;
	std::vector<SteamManifestFile> files;
};

enum class SteamManifestCacheState {
	Ready,
	Expired,
	Invalid,
};

SteamManifestLimits steamManifestDefaultLimits();
uint64_t steamManifestFreshnessSeconds();
std::wstring steamManifestRequestTarget(uint32_t depotId, uint64_t manifestId, uint64_t requestCode);
bool steamManifestParseZip(const uint8_t* data, size_t size, const uint8_t* depotKey, size_t depotKeySize, uint32_t expectedDepotId, uint64_t expectedManifestId, const SteamManifestLimits& limits, SteamManifestMetadata& metadata, std::string& error);
bool steamManifestValidate(const SteamManifestMetadata& metadata, const SteamManifestLimits& limits, std::string& error);
std::string steamManifestSerialize(const SteamManifestMetadata& metadata, const SteamManifestLimits& limits);
bool steamManifestDeserialize(const std::string& text, const SteamManifestLimits& limits, SteamManifestMetadata& metadata);
SteamManifestCacheState steamManifestClassify(const SteamManifestMetadata& metadata, const std::string& steamId, uint32_t appId, uint32_t depotId, uint64_t manifestId, const std::string& buildId, uint64_t now, uint64_t freshnessSeconds);
void steamManifestSecureClear(SteamManifestMetadata& metadata);
