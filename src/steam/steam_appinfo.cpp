#include "steam_appinfo.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace {

constexpr size_t kMaxProtoBytes = 16 * 1024 * 1024;
constexpr size_t kMaxTextBytes = 8 * 1024 * 1024;
constexpr size_t kMaxTokenBytes = 64 * 1024;
constexpr size_t kMaxTotalStringBytes = 4 * 1024 * 1024;
constexpr size_t kMaxNodes = 250000;
constexpr size_t kMaxIds = 100000;
constexpr size_t kMaxDepth = 64;

enum class TokenKind { String, Open, Close, End, Error };

struct Token {
	TokenKind kind = TokenKind::Error;
	std::string text;
};

struct DepotInfo {
	std::string osList;
	std::string language;
	uint32_t depotFromApp = 0;
	uint64_t manifestId = 0;
	uint64_t size = 0;
	uint64_t download = 0;
	bool sharedInstall = false;
};

struct Association {
	std::string type;
	std::string name;
};

void putVarint(std::vector<uint8_t>& output, uint64_t value) {
	while (value >= 0x80) {
		output.push_back((uint8_t)(value | 0x80));
		value >>= 7;
	}
	output.push_back((uint8_t)value);
}

void putKey(std::vector<uint8_t>& output, uint32_t field, uint32_t wire) {
	putVarint(output, ((uint64_t)field << 3) | wire);
}

void putUInt(std::vector<uint8_t>& output, uint32_t field, uint64_t value) {
	putKey(output, field, 0);
	putVarint(output, value);
}

void putBool(std::vector<uint8_t>& output, uint32_t field, bool value) {
	putUInt(output, field, value ? 1 : 0);
}

void putBytes(std::vector<uint8_t>& output, uint32_t field, const std::vector<uint8_t>& value) {
	putKey(output, field, 2);
	putVarint(output, value.size());
	output.insert(output.end(), value.begin(), value.end());
}

bool getVarint(const uint8_t* data, size_t size, size_t& offset, uint64_t& value) {
	value = 0;
	for (int shift = 0; shift <= 63; shift += 7) {
		if (offset >= size) return false;
		uint8_t byte = data[offset++];
		value |= (uint64_t)(byte & 0x7f) << shift;
		if (!(byte & 0x80)) return true;
	}
	return false;
}

bool getFixed32(const uint8_t* data, size_t size, size_t& offset, uint32_t& value) {
	if (offset + 4 > size) return false;
	value = (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) | ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
	offset += 4;
	return true;
}

bool getFixed64(const uint8_t* data, size_t size, size_t& offset, uint64_t& value) {
	if (offset + 8 > size) return false;
	value = 0;
	for (int i = 0; i < 8; i++) value |= (uint64_t)data[offset + i] << (i * 8);
	offset += 8;
	return true;
}

bool getBytes(const uint8_t* data, size_t size, size_t& offset, const uint8_t*& value, size_t& valueSize) {
	uint64_t length = 0;
	if (!getVarint(data, size, offset, length) || length > size - offset) return false;
	value = data + offset;
	valueSize = (size_t)length;
	offset += valueSize;
	return true;
}

bool skipField(const uint8_t* data, size_t size, size_t& offset, uint32_t wire) {
	uint64_t value = 0;
	uint32_t fixed = 0;
	const uint8_t* bytes = nullptr;
	size_t byteCount = 0;
	if (wire == 0) return getVarint(data, size, offset, value);
	if (wire == 1) return getFixed64(data, size, offset, value);
	if (wire == 2) return getBytes(data, size, offset, bytes, byteCount);
	if (wire == 5) return getFixed32(data, size, offset, fixed);
	return false;
}

bool appendRepeatedUInt(const uint8_t* data, size_t size, size_t& offset, uint32_t wire, std::vector<uint32_t>& output) {
	if (wire == 0) {
		uint64_t value = 0;
		if (!getVarint(data, size, offset, value) || value > std::numeric_limits<uint32_t>::max()) return false;
		output.push_back((uint32_t)value);
		return output.size() <= kMaxIds;
	}
	if (wire != 2) return false;
	const uint8_t* packed = nullptr;
	size_t packedSize = 0;
	if (!getBytes(data, size, offset, packed, packedSize)) return false;
	size_t packedOffset = 0;
	while (packedOffset < packedSize) {
		uint64_t value = 0;
		if (!getVarint(packed, packedSize, packedOffset, value) || value > std::numeric_limits<uint32_t>::max()) return false;
		output.push_back((uint32_t)value);
		if (output.size() > kMaxIds) return false;
	}
	return true;
}

bool parseAppToken(const uint8_t* data, size_t size, std::pair<uint32_t, uint64_t>& token) {
	token = {};
	bool hasAppId = false;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if ((field == 1 || field == 2) && wire == 0) {
			uint64_t value = 0;
			if (!getVarint(data, size, offset, value)) return false;
			if (field == 1) {
				if (value > std::numeric_limits<uint32_t>::max()) return false;
				token.first = (uint32_t)value;
				hasAppId = true;
			} else token.second = value;
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return hasAppId;
}

std::string hexBytes(const uint8_t* data, size_t size) {
	static const char* hex = "0123456789abcdef";
	std::string output;
	output.reserve(size * 2);
	for (size_t i = 0; i < size; i++) {
		output.push_back(hex[data[i] >> 4]);
		output.push_back(hex[data[i] & 15]);
	}
	return output;
}

std::string lower(std::string value) {
	for (char& ch : value) ch = (char)std::tolower((unsigned char)ch);
	return value;
}

bool containsWord(const std::string& value, const char* word) {
	std::string haystack = lower(value);
	std::string needle = lower(word);
	size_t offset = 0;
	while (offset <= haystack.size()) {
		size_t comma = haystack.find(',', offset);
		size_t end = comma == std::string::npos ? haystack.size() : comma;
		while (offset < end && std::isspace((unsigned char)haystack[offset])) offset++;
		while (end > offset && std::isspace((unsigned char)haystack[end - 1])) end--;
		if (haystack.substr(offset, end - offset) == needle) return true;
		if (comma == std::string::npos) break;
		offset = comma + 1;
	}
	return false;
}

bool parseUint32(const std::string& value, uint32_t& result) {
	if (value.empty()) return false;
	uint64_t parsed = 0;
	for (unsigned char ch : value) {
		if (ch < '0' || ch > '9') return false;
		parsed = parsed * 10 + (ch - '0');
		if (parsed > std::numeric_limits<uint32_t>::max()) return false;
	}
	result = (uint32_t)parsed;
	return true;
}

bool parseUint64(const std::string& value, uint64_t& result) {
	if (value.empty()) return false;
	uint64_t parsed = 0;
	for (unsigned char ch : value) {
		if (ch < '0' || ch > '9') return false;
		uint64_t digit = ch - '0';
		if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
		parsed = parsed * 10 + digit;
	}
	result = parsed;
	return true;
}

class TextParser {
public:
	TextParser(const uint8_t* data, size_t size, SteamAppMetadata& metadata)
		: data_((const char*)data), size_(size), metadata_(metadata) {}

	bool parse() {
		Token root = next();
		if (root.kind != TokenKind::String || root.text != "appinfo") return false;
		Token open = next();
		if (open.kind != TokenKind::Open) return false;
		path_.push_back("appinfo");
		if (!parseObject(1)) return false;
		path_.pop_back();
		Token end = next();
		if (end.kind != TokenKind::End) return false;
		uint32_t embedded = 0;
		if (!parseUint32(embeddedAppId_, embedded) || embedded != metadata_.appId) return false;
		finish();
		return true;
	}

private:
	const char* data_ = nullptr;
	size_t size_ = 0;
	size_t offset_ = 0;
	size_t stringBytes_ = 0;
	size_t nodes_ = 0;
	SteamAppMetadata& metadata_;
	std::vector<std::string> path_;
	std::string embeddedAppId_;
	std::map<std::string, SteamAppLaunchOption> launches_;
	std::map<std::string, Association> associations_;
	std::map<uint32_t, DepotInfo> depots_;

	void skipSpace() {
		for (;;) {
			while (offset_ < size_ && std::isspace((unsigned char)data_[offset_])) offset_++;
			if (offset_ + 1 < size_ && data_[offset_] == '/' && data_[offset_ + 1] == '/') {
				offset_ += 2;
				while (offset_ < size_ && data_[offset_] != '\n') offset_++;
				continue;
			}
			break;
		}
	}

	Token next() {
		skipSpace();
		if (offset_ >= size_) return { TokenKind::End, {} };
		char ch = data_[offset_++];
		if (ch == '{') return { TokenKind::Open, {} };
		if (ch == '}') return { TokenKind::Close, {} };
		std::string value;
		if (ch == '"') {
			bool closed = false;
			while (offset_ < size_) {
				ch = data_[offset_++];
				if (ch == '"') { closed = true; break; }
				if (ch == '\\') {
					if (offset_ >= size_) return { TokenKind::Error, {} };
					char escaped = data_[offset_++];
					if (escaped == 'n') value.push_back('\n');
					else if (escaped == 'r') value.push_back('\r');
					else if (escaped == 't') value.push_back('\t');
					else value.push_back(escaped);
				} else value.push_back(ch);
				if (value.size() > kMaxTokenBytes) return { TokenKind::Error, {} };
			}
			if (!closed) return { TokenKind::Error, {} };
		} else {
			value.push_back(ch);
			while (offset_ < size_) {
				ch = data_[offset_];
				if (std::isspace((unsigned char)ch) || ch == '{' || ch == '}') break;
				value.push_back(ch);
				offset_++;
				if (value.size() > kMaxTokenBytes) return { TokenKind::Error, {} };
			}
		}
		if (stringBytes_ + value.size() > kMaxTotalStringBytes) return { TokenKind::Error, {} };
		stringBytes_ += value.size();
		return { TokenKind::String, std::move(value) };
	}

	bool parseObject(size_t depth) {
		if (depth > kMaxDepth) return false;
		for (;;) {
			Token key = next();
			if (key.kind == TokenKind::Close) return true;
			if (key.kind != TokenKind::String || ++nodes_ > kMaxNodes) return false;
			Token value = next();
			if (value.kind == TokenKind::Open) {
				path_.push_back(key.text);
				bool ok = parseObject(depth + 1);
				path_.pop_back();
				if (!ok) return false;
			} else if (value.kind == TokenKind::String) {
				handle(key.text, value.text);
			} else return false;
		}
	}

	bool path(std::initializer_list<const char*> parts) const {
		if (path_.size() != parts.size()) return false;
		size_t index = 0;
		for (const char* part : parts) if (path_[index++] != part) return false;
		return true;
	}

	void handle(const std::string& key, const std::string& value) {
		if (path({ "appinfo" })) {
			if (key == "appid") embeddedAppId_ = value;
			return;
		}
		if (path({ "appinfo", "common" })) {
			if (key == "name") metadata_.name = value;
			else if (key == "type") metadata_.type = value;
			else if (key == "releasestate") metadata_.releaseState = value;
			else if (key == "oslist") metadata_.osList = value;
			else if (key == "osarch") metadata_.osArch = value;
			else if (key == "controller_support") metadata_.controllerSupport = value;
			return;
		}
		if (path({ "appinfo", "extended" })) {
			if (key == "developer") metadata_.developer = value;
			else if (key == "publisher") metadata_.publisher = value;
			return;
		}
		if (path_.size() == 4 && path_[0] == "appinfo" && path_[1] == "common" && path_[2] == "associations") {
			Association& association = associations_[path_[3]];
			if (key == "type") association.type = lower(value);
			else if (key == "name") association.name = value;
			return;
		}
		if (path({ "appinfo", "config" })) {
			if (key == "installdir") metadata_.installDir = value;
			return;
		}
		if (path_.size() == 4 && path_[0] == "appinfo" && path_[1] == "config" && path_[2] == "launch") {
			SteamAppLaunchOption& launch = launches_[path_[3]];
			if (key == "executable") launch.executable = value;
			else if (key == "arguments") launch.arguments = value;
			else if (key == "workingdir") launch.workingDir = value;
			else if (key == "description") launch.description = value;
			else if (key == "oslist") launch.osList = value;
			else if (key == "osarch") launch.osArch = value;
			return;
		}
		if (path_.size() == 5 && path_[0] == "appinfo" && path_[1] == "config" && path_[2] == "launch" && path_[4] == "config") {
			SteamAppLaunchOption& launch = launches_[path_[3]];
			if (key == "oslist") launch.osList = value;
			else if (key == "osarch") launch.osArch = value;
			return;
		}
		if (path_.size() >= 3 && path_[0] == "appinfo" && path_[1] == "depots") {
			if (path_[2] == "branches" && path_.size() == 4 && path_[3] == "public" && key == "buildid") metadata_.publicBuildId = value;
			uint32_t depotId = 0;
			if (!parseUint32(path_[2], depotId)) return;
			DepotInfo& depot = depots_[depotId];
			if (path_.size() == 3) {
				uint32_t linkedApp = 0;
				if (key == "depotfromapp" && parseUint32(value, linkedApp)) depot.depotFromApp = linkedApp;
				else if (key == "sharedinstall" && value == "1") depot.sharedInstall = true;
			}
			if (path_.size() == 4 && path_[3] == "config") {
				if (key == "oslist") depot.osList = value;
				else if (key == "language") depot.language = value;
			}
			if (path_.size() == 5 && path_[3] == "manifests" && path_[4] == "public") {
				uint64_t amount = 0;
				if (key == "gid" && parseUint64(value, amount)) depot.manifestId = amount;
				else if (key == "size" && parseUint64(value, amount)) depot.size = amount;
				else if (key == "download" && parseUint64(value, amount)) depot.download = amount;
			}
		}
	}

	void finish() {
		for (const auto& association : associations_) {
			if (metadata_.developer.empty() && association.second.type == "developer") metadata_.developer = association.second.name;
			if (metadata_.publisher.empty() && association.second.type == "publisher") metadata_.publisher = association.second.name;
		}
		bool windowsLaunch = false;
		bool launch64 = false;
		bool launch32 = false;
		bool launchUnknown = false;
		for (const auto& item : launches_) {
			const SteamAppLaunchOption& launch = item.second;
			if (launch.executable.empty()) continue;
			bool windows = launch.osList.empty() || containsWord(launch.osList, "windows");
			if (!windows) continue;
			windowsLaunch = true;
			metadata_.launchOptions.push_back(launch);
			std::string arch = lower(launch.osArch);
			if (arch.find("64") != std::string::npos) launch64 = true;
			else if (arch.find("32") != std::string::npos) launch32 = true;
			else launchUnknown = true;
		}
		bool windowsDepot = false;
		for (const auto& item : depots_) {
			const DepotInfo& depot = item.second;
			if (!depot.osList.empty() && !containsWord(depot.osList, "windows")) continue;
			windowsDepot = true;
			metadata_.windowsDepotIds.push_back(item.first);
			SteamAppDepotManifest manifest;
			manifest.depotId = item.first;
			manifest.depotFromApp = depot.depotFromApp;
			manifest.manifestId = depot.manifestId;
			manifest.size = depot.size;
			manifest.download = depot.download;
			manifest.sharedInstall = depot.sharedInstall;
			manifest.osList = depot.osList;
			manifest.language = depot.language;
			metadata_.publicDepots.push_back(std::move(manifest));
			if (std::numeric_limits<uint64_t>::max() - metadata_.installBytes >= depot.size) metadata_.installBytes += depot.size;
			if (std::numeric_limits<uint64_t>::max() - metadata_.downloadBytes >= depot.download) metadata_.downloadBytes += depot.download;
		}
		bool platformDeclared = !metadata_.osList.empty();
		metadata_.windowsSupported = containsWord(metadata_.osList, "windows") || windowsLaunch || windowsDepot;
		std::string commonArch = lower(metadata_.osArch);
		bool common64 = commonArch.find("64") != std::string::npos;
		bool common32 = commonArch.find("32") != std::string::npos;
		if (!metadata_.windowsSupported && platformDeclared) {
			metadata_.architecture = "not_windows";
			metadata_.preflight = "blocked_non_windows";
		} else if (!metadata_.windowsSupported) {
			metadata_.architecture = "unknown";
			metadata_.preflight = "platform_unknown";
		} else if (common64 || launch64) {
			metadata_.architecture = "x64_candidate";
			metadata_.preflight = "needs_pe_inspection";
		} else if ((common32 && !common64) || (windowsLaunch && launch32 && !launchUnknown)) {
			metadata_.architecture = "x86";
			metadata_.preflight = "unsupported_x86";
		} else {
			metadata_.architecture = "unknown";
			metadata_.preflight = "needs_pe_inspection";
		}
		std::sort(metadata_.windowsDepotIds.begin(), metadata_.windowsDepotIds.end());
		std::sort(metadata_.publicDepots.begin(), metadata_.publicDepots.end(), [](const auto& left, const auto& right) { return left.depotId < right.depotId; });
	}
};

bool parseAppInfoMessage(const uint8_t* data, size_t size, SteamAppMetadata& metadata) {
	metadata = SteamAppMetadata();
	const uint8_t* sha = nullptr;
	size_t shaSize = 0;
	const uint8_t* buffer = nullptr;
	size_t bufferSize = 0;
	bool hasAppId = false;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if ((field == 1 || field == 2 || field == 3 || field == 6 || field == 7) && wire == 0) {
			uint64_t value = 0;
			if (!getVarint(data, size, offset, value)) return false;
			if (field == 1) {
				if (value > std::numeric_limits<uint32_t>::max()) return false;
				metadata.appId = (uint32_t)value;
				hasAppId = true;
			} else if (field == 2) {
				if (value > std::numeric_limits<uint32_t>::max()) return false;
				metadata.changeNumber = (uint32_t)value;
			} else if (field == 3) metadata.missingToken = value != 0;
			else if (field == 6) metadata.onlyPublic = value != 0;
			else {
				if (value > std::numeric_limits<uint32_t>::max()) return false;
				metadata.declaredSize = (uint32_t)value;
			}
		} else if ((field == 4 || field == 5) && wire == 2) {
			const uint8_t* bytes = nullptr;
			size_t byteCount = 0;
			if (!getBytes(data, size, offset, bytes, byteCount)) return false;
			if (field == 4) { sha = bytes; shaSize = byteCount; }
			else { buffer = bytes; bufferSize = byteCount; }
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	if (!hasAppId) return false;
	metadata.shaHex = sha && shaSize ? hexBytes(sha, shaSize) : "";
	metadata.hasBuffer = buffer && bufferSize;
	if (metadata.hasBuffer) metadata.bufferValid = steamAppParseTextMetadata(metadata.appId, buffer, bufferSize, metadata);
	return true;
}

}

std::vector<uint8_t> steamAppMakeAccessTokenRequest(const std::vector<uint32_t>& appIds) {
	std::vector<uint8_t> body;
	for (uint32_t appId : appIds) if (appId) putUInt(body, 2, appId);
	return body;
}

std::vector<uint8_t> steamAppMakeProductInfoRequest(const std::vector<SteamAppRequest>& apps) {
	std::vector<uint8_t> body;
	for (const auto& app : apps) {
		if (!app.appId) continue;
		std::vector<uint8_t> nested;
		putUInt(nested, 1, app.appId);
		if (app.accessToken) putUInt(nested, 2, app.accessToken);
		putBool(nested, 3, false);
		putBytes(body, 2, nested);
		std::fill(nested.begin(), nested.end(), (uint8_t)0);
	}
	putBool(body, 3, false);
	return body;
}

bool steamAppParseAccessTokenResponse(const uint8_t* data, size_t size, SteamAppTokenResponse& response) {
	response = SteamAppTokenResponse();
	if (!data || size > kMaxProtoBytes) return false;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if (field == 3 && wire == 2) {
			const uint8_t* nested = nullptr;
			size_t nestedSize = 0;
			if (!getBytes(data, size, offset, nested, nestedSize)) return false;
			std::pair<uint32_t, uint64_t> token;
			if (!parseAppToken(nested, nestedSize, token)) return false;
			response.appTokens.push_back(token);
			if (response.appTokens.size() > kMaxIds) return false;
		} else if (field == 4) {
			if (!appendRepeatedUInt(data, size, offset, wire, response.deniedAppIds)) return false;
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return true;
}

bool steamAppParseProductInfoResponse(const uint8_t* data, size_t size, SteamAppProductInfoResponse& response) {
	response = SteamAppProductInfoResponse();
	if (!data || size > kMaxProtoBytes) return false;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if (field == 1 && wire == 2) {
			const uint8_t* nested = nullptr;
			size_t nestedSize = 0;
			if (!getBytes(data, size, offset, nested, nestedSize)) return false;
			SteamAppMetadata metadata;
			if (!parseAppInfoMessage(nested, nestedSize, metadata)) return false;
			response.apps.push_back(std::move(metadata));
			if (response.apps.size() > kMaxIds) return false;
		} else if (field == 2) {
			if (!appendRepeatedUInt(data, size, offset, wire, response.unknownAppIds)) return false;
		} else if (field == 6 && wire == 0) {
			uint64_t value = 0;
			if (!getVarint(data, size, offset, value)) return false;
			response.responsePending = value != 0;
		} else if (field == 8 && wire == 2) {
			const uint8_t* bytes = nullptr;
			size_t byteCount = 0;
			if (!getBytes(data, size, offset, bytes, byteCount) || byteCount > 1024) return false;
			response.httpHost.assign((const char*)bytes, byteCount);
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return true;
}

bool steamAppParseTextMetadata(uint32_t appId, const uint8_t* data, size_t size, SteamAppMetadata& metadata) {
	if (!appId || !data || !size || size > kMaxTextBytes) return false;
	while (size && data[size - 1] == 0) size--;
	if (!size) return false;
	metadata.appId = appId;
	TextParser parser(data, size, metadata);
	return parser.parse();
}
