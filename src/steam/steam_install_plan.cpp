#include "steam_install_plan.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

namespace {

std::string lower(std::string value) {
	for (char& ch : value) ch = (char)std::tolower((unsigned char)ch);
	return value;
}

bool includesEnglish(const std::string& value) {
	if (value.empty()) return true;
	std::string normalized = lower(value);
	size_t offset = 0;
	while (offset <= normalized.size()) {
		size_t comma = normalized.find(',', offset);
		size_t end = comma == std::string::npos ? normalized.size() : comma;
		while (offset < end && std::isspace((unsigned char)normalized[offset])) offset++;
		while (end > offset && std::isspace((unsigned char)normalized[end - 1])) end--;
		if (normalized.substr(offset, end - offset) == "english") return true;
		if (comma == std::string::npos) break;
		offset = comma + 1;
	}
	return false;
}

bool addBounded(uint64_t& total, uint64_t value) {
	if ((std::numeric_limits<uint64_t>::max)() - total < value) return false;
	total += value;
	return true;
}

}

bool steamBuildInstallPlan(const SteamAppMetadata& metadata, const std::map<uint32_t, SteamAppMetadata>& appMetadata,
		bool entitled, const std::vector<uint32_t>& grantedDepotIds, SteamInstallPlan& plan) {
	plan = SteamInstallPlan();
	plan.appId = metadata.appId;
	plan.name = metadata.name;
	plan.branch = "public";
	plan.buildId = metadata.publicBuildId;
	plan.state = "planning";
	plan.message = "Discovering authorized content servers";
	if (!metadata.appId || !entitled || metadata.preflight == "unsupported_x86" || metadata.preflight == "blocked_non_windows") {
		plan.state = "blocked";
		plan.message = "Title is not eligible for an install authorization plan";
		return false;
	}
	std::set<uint32_t> granted(grantedDepotIds.begin(), grantedDepotIds.end());
	if (granted.empty()) {
		plan.state = "blocked";
		plan.message = "No package depot grants are available";
		return false;
	}
	std::set<uint32_t> unique;
	for (const auto& configured : metadata.publicDepots) {
		SteamAppDepotManifest source = configured;
		if (source.depotFromApp) {
			auto app = appMetadata.find(source.depotFromApp);
			if (app != appMetadata.end()) {
				auto shared = std::find_if(app->second.publicDepots.begin(), app->second.publicDepots.end(),
					[&](const SteamAppDepotManifest& depot) { return depot.depotId == source.depotId; });
				if (shared != app->second.publicDepots.end()) {
					source.manifestId = shared->manifestId;
					source.size = shared->size;
					source.download = shared->download;
				}
			}
		}
		if (!includesEnglish(source.language)) continue;
		if (!source.depotId || !unique.insert(source.depotId).second) continue;
		if (granted.find(source.depotId) == granted.end()) continue;
		SteamInstallPlanDepot depot;
		depot.depotId = source.depotId;
		depot.contentAppId = source.depotFromApp ? source.depotFromApp : metadata.appId;
		depot.sharedInstall = source.sharedInstall || source.depotFromApp != 0;
		depot.manifestId = source.manifestId;
		depot.size = source.size;
		depot.download = source.download;
		if (!addBounded(plan.installBytes, depot.size) || !addBounded(plan.downloadBytes, depot.download)) {
			plan.state = "blocked";
			plan.message = "Install plan size overflow";
			plan.unresolvedDepotCount++;
			return false;
		}
		if (depot.sharedInstall && !depot.manifestId) {
			depot.state = "shared_manifest_unresolved";
			plan.unresolvedDepotCount++;
		} else if (depot.manifestId) {
			depot.state = "pending_authorization";
			plan.directDepotCount++;
		} else {
			depot.state = "public_manifest_missing";
			plan.unresolvedDepotCount++;
		}
		plan.depots.push_back(std::move(depot));
	}
	if (plan.directDepotCount == 0) {
		plan.state = "blocked";
		plan.message = "No package-granted direct public manifest is available";
		return false;
	}
	return steamValidateInstallPlan(plan);
}

bool steamValidateInstallPlan(const SteamInstallPlan& plan) {
	if (!plan.appId || plan.branch != "public" || plan.directDepotCount < 0 || plan.authorizedDepotCount < 0 || plan.unresolvedDepotCount < 0 || plan.authorizedDepotCount > plan.directDepotCount || plan.depots.size() > 10000) return false;
	if (plan.state != "planning" && plan.state != "authorized" && plan.state != "partial" && plan.state != "blocked") return false;
	std::set<uint32_t> unique;
	int direct = 0;
	int unresolved = 0;
	int authorized = 0;
	uint64_t installBytes = 0;
	uint64_t downloadBytes = 0;
	for (const auto& depot : plan.depots) {
		if (!depot.depotId || !depot.contentAppId || !unique.insert(depot.depotId).second) return false;
		if (!addBounded(installBytes, depot.size) || !addBounded(downloadBytes, depot.download)) return false;
		if (depot.manifestId) direct++;
		else unresolved++;
		if (depot.keyAuthorized && !depot.manifestId) return false;
		if (depot.manifestCodeGranted && !depot.keyAuthorized) return false;
		if (depot.cdnAuthChecked && !depot.manifestCodeGranted) return false;
		if (depot.cdnTokenIssued && !depot.cdnAuthChecked) return false;
		if (depot.state == "authorized") {
			if (!depot.keyAuthorized || !depot.manifestCodeGranted || !depot.cdnAuthChecked) return false;
			authorized++;
		} else if (depot.cdnAuthChecked) return false;
	}
	if (direct != plan.directDepotCount || authorized != plan.authorizedDepotCount || installBytes != plan.installBytes || downloadBytes != plan.downloadBytes) return false;
	if (plan.state == "authorized" && (authorized != direct || plan.unresolvedDepotCount != 0)) return false;
	if (plan.state == "partial" && (authorized == 0 || plan.unresolvedDepotCount != (int)plan.depots.size() - authorized)) return false;
	if (plan.state == "blocked" && (authorized != 0 || plan.unresolvedDepotCount != (int)plan.depots.size())) return false;
	if (plan.state == "planning" && authorized > direct) return false;
	return plan.unresolvedDepotCount >= unresolved;
}
