#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SteamChunkLimits {
	size_t encryptedBytes = 16u * 1024u * 1024u;
	size_t decryptedBytes = 16u * 1024u * 1024u;
	size_t expandedBytes = 8u * 1024u * 1024u;
	size_t lzmaDictionaryBytes = 32u * 1024u * 1024u;
	size_t cacheBytes = 64u * 1024u;
};

SteamChunkLimits steamChunkDefaultLimits();
std::wstring steamChunkRequestTarget(uint32_t depotId, const std::array<uint8_t, 20>& sha);
bool steamChunkProcess(const uint8_t* data, size_t size, const uint8_t* depotKey, size_t depotKeySize, const std::array<uint8_t, 20>& expectedSha, uint32_t expectedChecksum, uint32_t expectedOriginalBytes, uint32_t expectedCompressedBytes, const SteamChunkLimits& limits, std::vector<uint8_t>& output, std::string& compression, std::string& error);
uint32_t steamChunkAdler32(const uint8_t* data, size_t size);
void steamChunkSecureClear(std::vector<uint8_t>& bytes);
