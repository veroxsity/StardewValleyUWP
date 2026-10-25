#include "steam_install_queue.h"

std::vector<InstallPrefetchItem> steamInstallBuildChunkQueue(
	const SteamManifestMetadata& metadata,
	size_t startFile,
	size_t startChunk,
	size_t limit) {
	std::vector<InstallPrefetchItem> queue;
	if (!limit) return queue;
	queue.reserve(limit);

	for (size_t fileIndex = startFile; fileIndex < metadata.files.size() && queue.size() < limit; fileIndex++) {
		const auto& file = metadata.files[fileIndex];
		size_t first = (fileIndex == startFile) ? startChunk : 0;
		for (size_t chunkIndex = first; chunkIndex < file.chunks.size() && queue.size() < limit; chunkIndex++) {
			InstallPrefetchItem item;
			item.fileIndex = fileIndex;
			item.chunkIndex = chunkIndex;
			item.depotId = file.sourceDepotId ? file.sourceDepotId : metadata.depotId;
			item.chunk = file.chunks[chunkIndex];
			queue.push_back(item);
		}
	}
	return queue;
}
