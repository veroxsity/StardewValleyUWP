#pragma once
#include "steam_manifest.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SteamFullInstallLimits {
	uint32_t appId = 0;
	uint32_t depotId = 0;
	uint64_t manifestId = 0;
	std::string buildId;
	std::string executablePath;
	std::string executableSha;
	uint64_t executableBytes = 0;
	uint64_t installBytes = 0;
	uint64_t uniqueCompressedBytes = 0;
	size_t fileCount = 0;
	size_t chunkCount = 0;
	size_t uniqueChunkCount = 0;
	uint64_t maxInstallBytes = 64ull * 1024ull * 1024ull * 1024ull;
	uint64_t maxTransferBytes = 64ull * 1024ull * 1024ull * 1024ull;
	uint64_t freeSpaceMarginBytes = 64ull * 1024ull * 1024ull;
	size_t pathBytes = 1024;
	size_t fullPathChars = 1024;
	size_t recordBytes = 64u * 1024u;
};

struct SteamFullInstallProgress {
	std::string steamId;
	uint32_t appId = 0;
	uint32_t depotId = 0;
	uint64_t manifestId = 0;
	std::string buildId;
	size_t nextFile = 0;
	size_t currentChunk = 0;
	uint64_t filesVerified = 0;
	uint64_t chunksVerified = 0;
	uint64_t transferredBytes = 0;
	uint64_t updatedAt = 0;
};

struct SteamInstalledRecord {
	std::string steamId;
	uint32_t appId = 0;
	uint32_t depotId = 0;
	uint64_t manifestId = 0;
	std::string buildId;
	std::string installDir;
	std::string executable;
	std::string architecture;
	uint64_t installedBytes = 0;
	uint64_t fileCount = 0;
	uint64_t chunkCount = 0;
	uint64_t installedAt = 0;
};

SteamFullInstallLimits steamFullInstallDefaultLimits();
std::string steamFullInstallUnrealShippingCandidate(const std::string& executablePath);
bool steamFullInstallMergeManifests(const std::vector<SteamManifestMetadata>& manifests, SteamManifestMetadata& merged, std::string& error);
bool steamFullInstallConfigure(const SteamManifestMetadata& metadata, const std::string& executablePath, SteamFullInstallLimits& limits, std::string& error);
bool steamFullInstallValidateManifest(const SteamManifestMetadata& metadata, const SteamFullInstallLimits& limits, uint64_t& referenceTransferBytes, std::string& error);
bool steamFullInstallBegin(const std::wstring& localState, const SteamManifestMetadata& metadata, const SteamFullInstallLimits& limits, SteamFullInstallProgress& progress, uint64_t& referenceTransferBytes, bool& resumed, std::string& error);
bool steamFullInstallPrepareFile(const std::wstring& localState, const SteamManifestMetadata& metadata, SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits, std::string& error);
bool steamFullInstallWriteChunks(const std::wstring& localState, const SteamManifestMetadata& metadata, const SteamFullInstallProgress& progress, size_t fileIndex, const std::vector<std::vector<uint8_t>>& chunks, const SteamFullInstallLimits& limits, std::string& error);
bool steamFullInstallSaveProgress(const std::wstring& localState, const SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits);
bool steamFullInstallFinalizeFile(const std::wstring& localState, const SteamManifestMetadata& metadata, SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits, std::string& error);
bool steamFullInstallComplete(const std::wstring& localState, const SteamManifestMetadata& metadata, const SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits, SteamInstalledRecord& record, std::string& error);
bool steamFullInstallLoadRecordAny(const std::wstring& localState, uint32_t appId, SteamInstalledRecord& record);
void steamFullInstallClearStaging(const std::wstring& localState, uint32_t appId, const SteamFullInstallLimits& limits);
std::string steamFullInstallSerializeProgress(const SteamFullInstallProgress& progress, const SteamFullInstallLimits& limits);
bool steamFullInstallDeserializeProgress(const std::string& text, const SteamFullInstallLimits& limits, SteamFullInstallProgress& progress);
std::string steamFullInstallSerializeRecord(const SteamInstalledRecord& record, const SteamFullInstallLimits& limits);
