#pragma once
#include "steam_manifest.h"

#include <cstdint>
#include <string>
#include <vector>

// one continuous chunk queue, batching per file idles connections on depots of small files
struct InstallPrefetchItem {
	size_t fileIndex = 0;
	size_t chunkIndex = 0;
	uint32_t depotId = 0;
	SteamManifestChunk chunk;
};

// chunks still to fetch in write order from the resume point, the caller skips verified files
std::vector<InstallPrefetchItem> steamInstallBuildChunkQueue(
	const SteamManifestMetadata& metadata,
	size_t startFile,
	size_t startChunk,
	size_t limit);
