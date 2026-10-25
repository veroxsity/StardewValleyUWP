#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "steam_appinfo.h"

struct SteamInstallPlanDepot {
	uint32_t depotId = 0;
	uint32_t contentAppId = 0;
	uint64_t manifestId = 0;
	uint64_t size = 0;
	uint64_t download = 0;
	bool sharedInstall = false;
	bool keyAuthorized = false;
	bool manifestCodeGranted = false;
	bool cdnAuthChecked = false;
	bool cdnTokenIssued = false;
	uint32_t cdnTokenExpiration = 0;
	std::string state;
};

struct SteamInstallPlan {
	uint32_t appId = 0;
	std::string name;
	std::string branch = "public";
	std::string buildId;
	std::string state;
	std::string message;
	std::string serverHost;
	uint64_t createdAt = 0;
	uint64_t installBytes = 0;
	uint64_t downloadBytes = 0;
	int directDepotCount = 0;
	int authorizedDepotCount = 0;
	int unresolvedDepotCount = 0;
	std::vector<SteamInstallPlanDepot> depots;
};

bool steamBuildInstallPlan(const SteamAppMetadata& metadata, const std::map<uint32_t, SteamAppMetadata>& appMetadata,
	bool entitled, const std::vector<uint32_t>& grantedDepotIds, SteamInstallPlan& plan);
bool steamValidateInstallPlan(const SteamInstallPlan& plan);
