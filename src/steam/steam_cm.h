#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "steam_appinfo.h"
#include "steam_install_plan.h"

struct SteamInstalledRecord;

enum class SteamCmState {
	Offline,
	NeedsRelink,
	Connecting,
	LoggingOn,
	WaitingForLicenses,
	ResolvingTokens,
	ResolvingPackages,
	ResolvingAppTokens,
	ResolvingApps,
	PlanningContent,
	Online,
	Error,
};

struct SteamCmStatus {
	SteamCmState state = SteamCmState::Offline;
	std::string message;
	int licenseCount = 0;
	int resolvedPackageCount = 0;
	int entitlementAppCount = 0;
	int appMetadataCount = 0;
};

struct SteamManifestStatus {
	std::string state = "none";
	std::string message;
	uint32_t appId = 0;
	uint32_t depotId = 0;
	uint64_t manifestId = 0;
	std::string buildId;
	uint64_t retrievedAt = 0;
	uint64_t originalBytes = 0;
	uint64_t compressedBytes = 0;
	uint64_t fileCount = 0;
	uint64_t chunkCount = 0;
};

struct SteamInstallStatus {
	std::string state = "none";
	std::string message;
	uint32_t appId = 0;
	uint32_t depotId = 0;
	uint64_t manifestId = 0;
	std::string buildId;
	uint64_t transferredBytes = 0;
	uint64_t totalBytes = 0;
	uint64_t filesVerified = 0;
	uint64_t totalFiles = 0;
	uint64_t chunksVerified = 0;
	uint64_t totalChunks = 0;
	uint64_t installedBytes = 0;
	std::string executable;
	std::string architecture;
};

void steamCmInit(const wchar_t* localState, void (*log)(const char*), void (*onEntitlementsChanged)(void));
void steamCmStart(const std::string& refreshToken, const std::string& steamId, const std::string& accountName);
void steamCmRequireRelink();
void steamCmForget();
SteamCmStatus steamCmStatus();
bool steamCmRequestInstallPlan(uint32_t appId);
bool steamCmCancelInstall(uint32_t appId);
bool steamCmGetInstallStatus(uint32_t appId, SteamInstallStatus& status);
