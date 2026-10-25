#include "steam_pics.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

namespace {

constexpr size_t kMaxProtoBytes = 16 * 1024 * 1024;
constexpr size_t kMaxPackageBytes = 8 * 1024 * 1024;
constexpr size_t kMaxKeyBytes = 1024;
constexpr size_t kMaxStringBytes = 1024 * 1024;
constexpr size_t kMaxTotalStringBytes = 4 * 1024 * 1024;
constexpr size_t kMaxNodes = 250000;
constexpr size_t kMaxIds = 100000;
constexpr size_t kMaxDepth = 64;

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

bool parsePackageToken(const uint8_t* data, size_t size, std::pair<uint32_t, uint64_t>& token) {
	token = {};
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
			} else token.second = value;
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return token.first != 0;
}

bool parsePackageInfoMessage(const uint8_t* data, size_t size, SteamPicsPackageInfo& info) {
	info = SteamPicsPackageInfo();
	const uint8_t* buffer = nullptr;
	size_t bufferSize = 0;
	bool hasPackageId = false;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if ((field == 1 || field == 2 || field == 3 || field == 6) && wire == 0) {
			uint64_t value = 0;
			if (!getVarint(data, size, offset, value)) return false;
			if (field == 1) {
				if (value > std::numeric_limits<uint32_t>::max()) return false;
				info.packageId = (uint32_t)value;
				hasPackageId = true;
			} else if (field == 2) {
				if (value > std::numeric_limits<uint32_t>::max()) return false;
				info.changeNumber = (uint32_t)value;
			} else if (field == 3) info.missingToken = value != 0;
			else {
				if (value > std::numeric_limits<uint32_t>::max()) return false;
				info.declaredSize = (uint32_t)value;
			}
		} else if ((field == 4 || field == 5) && wire == 2) {
			const uint8_t* bytes = nullptr;
			size_t byteCount = 0;
			if (!getBytes(data, size, offset, bytes, byteCount)) return false;
			if (field == 5) {
				buffer = bytes;
				bufferSize = byteCount;
			}
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	if (!hasPackageId) return false;
	info.hasBuffer = buffer && bufferSize;
	if (info.hasBuffer) info.bufferValid = steamPicsParsePackageBuffer(info.packageId, buffer, bufferSize, info);
	return true;
}

bool readCString(const uint8_t* data, size_t size, size_t& offset, size_t limit, size_t& stringBytes, std::string& value) {
	if (offset >= size) return false;
	const void* end = std::memchr(data + offset, 0, size - offset);
	if (!end) return false;
	size_t length = (const uint8_t*)end - (data + offset);
	if (length > limit || stringBytes + length > kMaxTotalStringBytes) return false;
	value.assign((const char*)data + offset, length);
	stringBytes += length;
	offset += length + 1;
	return true;
}

bool skipWideString(const uint8_t* data, size_t size, size_t& offset, size_t& stringBytes) {
	size_t start = offset;
	while (offset + 1 < size) {
		if (!data[offset] && !data[offset + 1]) {
			size_t length = offset - start;
			if (length > kMaxStringBytes || stringBytes + length > kMaxTotalStringBytes) return false;
			stringBytes += length;
			offset += 2;
			return true;
		}
		offset += 2;
	}
	return false;
}

bool parseDecimalId(const std::string& value, uint32_t& id) {
	if (value.empty()) return false;
	uint64_t parsed = 0;
	for (unsigned char ch : value) {
		if (ch < '0' || ch > '9') return false;
		parsed = parsed * 10 + (ch - '0');
		if (parsed > std::numeric_limits<uint32_t>::max()) return false;
	}
	id = (uint32_t)parsed;
	return true;
}

bool scalarId(uint8_t type, const std::string& text, uint64_t unsignedValue, int64_t signedValue, uint32_t& id) {
	if (type == 1) return parseDecimalId(text, id);
	if (type == 2 || type == 10) {
		if (signedValue < 0 || (uint64_t)signedValue > std::numeric_limits<uint32_t>::max()) return false;
		id = (uint32_t)signedValue;
		return true;
	}
	if (type == 7) {
		if (unsignedValue > std::numeric_limits<uint32_t>::max()) return false;
		id = (uint32_t)unsignedValue;
		return true;
	}
	return false;
}

std::string scalarText(uint8_t type, const std::string& text, uint64_t unsignedValue, int64_t signedValue) {
	if (type == 1) return text;
	if (type == 2 || type == 10) return std::to_string(signedValue);
	if (type == 7) return std::to_string(unsignedValue);
	return "";
}

}

std::vector<uint8_t> steamPicsMakeAccessTokenRequest(const std::vector<uint32_t>& packageIds) {
	std::vector<uint8_t> body;
	for (uint32_t packageId : packageIds) if (packageId) putUInt(body, 1, packageId);
	return body;
}

std::vector<uint8_t> steamPicsMakeProductInfoRequest(const std::vector<SteamPicsPackageRequest>& packages) {
	std::vector<uint8_t> body;
	for (const auto& package : packages) {
		if (!package.packageId) continue;
		std::vector<uint8_t> nested;
		putUInt(nested, 1, package.packageId);
		if (package.accessToken) putUInt(nested, 2, package.accessToken);
		putBytes(body, 1, nested);
		std::fill(nested.begin(), nested.end(), (uint8_t)0);
	}
	putBool(body, 3, false);
	return body;
}

bool steamPicsParseAccessTokenResponse(const uint8_t* data, size_t size, SteamPicsTokenResponse& response) {
	response = SteamPicsTokenResponse();
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
			std::pair<uint32_t, uint64_t> token;
			if (!parsePackageToken(nested, nestedSize, token)) return false;
			response.packageTokens.push_back(token);
			if (response.packageTokens.size() > kMaxIds) return false;
		} else if (field == 2) {
			if (!appendRepeatedUInt(data, size, offset, wire, response.deniedPackageIds)) return false;
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return true;
}

bool steamPicsParseProductInfoResponse(const uint8_t* data, size_t size, SteamPicsProductInfoResponse& response) {
	response = SteamPicsProductInfoResponse();
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
			SteamPicsPackageInfo info;
			if (!parsePackageInfoMessage(nested, nestedSize, info)) return false;
			response.packages.push_back(std::move(info));
			if (response.packages.size() > kMaxIds) return false;
		} else if (field == 4) {
			if (!appendRepeatedUInt(data, size, offset, wire, response.unknownPackageIds)) return false;
		} else if (field == 6 && wire == 0) {
			uint64_t value = 0;
			if (!getVarint(data, size, offset, value)) return false;
			response.responsePending = value != 0;
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return true;
}

bool steamPicsParsePackageBuffer(uint32_t packageId, const uint8_t* data, size_t size, SteamPicsPackageInfo& info) {
	if (!data || size < 7 || size > kMaxPackageBytes) return false;
	uint32_t version = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
	if (version != 1) return false;
	size_t offset = 4;
	size_t nodes = 0;
	size_t stringBytes = 0;
	std::vector<std::string> path;
	std::set<uint32_t> seenAppIds;
	std::set<uint32_t> seenDepotIds;
	bool sawRoot = false;
	bool finished = false;
	std::string expectedRoot = std::to_string(packageId);
	while (offset < size) {
		uint8_t type = data[offset++];
		if (type == 8 || type == 11) {
			if (path.empty()) {
				finished = true;
				break;
			}
			path.pop_back();
			continue;
		}
		if (++nodes > kMaxNodes) return false;
		std::string key;
		if (!readCString(data, size, offset, kMaxKeyBytes, stringBytes, key)) return false;
		if (type == 0) {
			if (path.size() >= kMaxDepth) return false;
			if (path.empty()) {
				if (sawRoot || key != expectedRoot) return false;
				sawRoot = true;
			} else if (path.size() == 1 && (path[0] != expectedRoot)) return false;
			else if (path.size() == 2 && (path[1] == "appids" || path[1] == "depotids")) return false;
			path.push_back(key);
			continue;
		}
		if (path.empty()) return false;
		std::string text;
		uint64_t unsignedValue = 0;
		int64_t signedValue = 0;
		if (type == 1) {
			if (!readCString(data, size, offset, kMaxStringBytes, stringBytes, text)) return false;
		} else if (type == 2 || type == 3 || type == 4 || type == 6) {
			uint32_t value = 0;
			if (!getFixed32(data, size, offset, value)) return false;
			signedValue = (int32_t)value;
			unsignedValue = value;
		} else if (type == 5) {
			if (!skipWideString(data, size, offset, stringBytes)) return false;
		} else if (type == 7 || type == 10) {
			uint64_t value = 0;
			if (!getFixed64(data, size, offset, value)) return false;
			unsignedValue = value;
			signedValue = (int64_t)value;
		} else return false;
		if (path.size() == 2 && (path[1] == "appids" || path[1] == "depotids")) {
			uint32_t id = 0;
			if (!scalarId(type, text, unsignedValue, signedValue, id)) return false;
			auto& values = path[1] == "appids" ? info.appIds : info.depotIds;
			auto& seen = path[1] == "appids" ? seenAppIds : seenDepotIds;
			if (id && seen.insert(id).second) values.push_back(id);
			if (values.size() > kMaxIds) return false;
		} else if (path.size() == 1) {
			std::string value = scalarText(type, text, unsignedValue, signedValue);
			if (key == "billingtype") info.billingType = value;
			else if (key == "licensetype") info.licenseType = value;
			else if (key == "status") info.status = value;
			else if (key == "packageid" && !value.empty()) {
				uint32_t embedded = 0;
				if (!parseDecimalId(value, embedded) || embedded != packageId) return false;
			}
		}
	}
	return finished && sawRoot && path.empty() && offset == size;
}
