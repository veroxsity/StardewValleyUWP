#include "steam_content_auth.h"
#include <cstring>
#include <limits>

namespace {

constexpr size_t kMaxProtoBytes = 1024 * 1024;
constexpr size_t kMaxStringBytes = 4096;
constexpr size_t kMaxServers = 100;
constexpr size_t kMaxIds = 100000;

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

void putBytes(std::vector<uint8_t>& output, uint32_t field, const void* data, size_t size) {
	putKey(output, field, 2);
	putVarint(output, size);
	const uint8_t* bytes = (const uint8_t*)data;
	output.insert(output.end(), bytes, bytes + size);
}

void putString(std::vector<uint8_t>& output, uint32_t field, const std::string& value) {
	putBytes(output, field, value.data(), value.size());
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

bool readString(const uint8_t* data, size_t size, size_t& offset, std::string& value) {
	const uint8_t* bytes = nullptr;
	size_t byteCount = 0;
	if (!getBytes(data, size, offset, bytes, byteCount) || byteCount > kMaxStringBytes) return false;
	value.assign((const char*)bytes, byteCount);
	return true;
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

bool parseServer(const uint8_t* data, size_t size, SteamContentServer& server) {
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if ((field == 1 || field == 8 || field == 12) && wire == 2) {
			std::string value;
			if (!readString(data, size, offset, value)) return false;
			if (field == 1) server.type = std::move(value);
			else if (field == 8) server.host = std::move(value);
			else server.httpsSupport = std::move(value);
		} else if ((field == 3 || field == 7 || field == 10) && wire == 0) {
			uint64_t value = 0;
			if (!getVarint(data, size, offset, value)) return false;
			if (field == 3) server.cellId = (int32_t)value;
			else if (field == 7) server.steamChinaOnly = value != 0;
			else server.useAsProxy = value != 0;
		} else if (field == 5 && wire == 5) {
			uint32_t bits = 0;
			if (!getFixed32(data, size, offset, bits)) return false;
			std::memcpy(&server.weightedLoad, &bits, sizeof(bits));
		} else if (field == 13) {
			if (!appendRepeatedUInt(data, size, offset, wire, server.allowedAppIds)) return false;
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return !server.host.empty();
}

}

std::vector<uint8_t> steamContentMakeDepotKeyRequest(uint32_t appId, uint32_t depotId) {
	std::vector<uint8_t> body;
	putUInt(body, 1, depotId);
	putUInt(body, 2, appId);
	return body;
}

bool steamContentParseDepotKeyResponse(const uint8_t* data, size_t size, SteamDepotKeyResponse& response) {
	response = SteamDepotKeyResponse();
	if (!data || size > kMaxProtoBytes) return false;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if ((field == 1 || field == 2) && wire == 0) {
			uint64_t value = 0;
			if (!getVarint(data, size, offset, value) || value > std::numeric_limits<uint32_t>::max()) return false;
			if (field == 1) response.result = (int)value;
			else response.depotId = (uint32_t)value;
		} else if (field == 3 && wire == 2) {
			const uint8_t* bytes = nullptr;
			size_t byteCount = 0;
			if (!getBytes(data, size, offset, bytes, byteCount) || byteCount != 32) return false;
			response.key.assign(bytes, bytes + byteCount);
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return response.depotId != 0;
}

std::vector<uint8_t> steamContentMakeServerRequest(uint32_t cellId, uint32_t maxServers) {
	std::vector<uint8_t> body;
	putUInt(body, 1, cellId);
	putUInt(body, 2, maxServers ? maxServers : 20);
	return body;
}

bool steamContentParseServerResponse(const uint8_t* data, size_t size, std::vector<SteamContentServer>& servers) {
	servers.clear();
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
			SteamContentServer server;
			if (!parseServer(nested, nestedSize, server)) return false;
			servers.push_back(std::move(server));
			if (servers.size() > kMaxServers) return false;
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return !servers.empty();
}

std::vector<uint8_t> steamContentMakeManifestCodeRequest(uint32_t appId, uint32_t depotId, uint64_t manifestId) {
	std::vector<uint8_t> body;
	putUInt(body, 1, appId);
	putUInt(body, 2, depotId);
	putUInt(body, 3, manifestId);
	return body;
}

bool steamContentParseManifestCodeResponse(const uint8_t* data, size_t size, SteamManifestCodeResponse& response) {
	response = SteamManifestCodeResponse();
	if (!data || size > kMaxProtoBytes) return false;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key)) return false;
		uint32_t field = (uint32_t)(key >> 3), wire = (uint32_t)(key & 7);
		if (field == 1 && wire == 0) {
			if (!getVarint(data, size, offset, response.code)) return false;
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return true;
}
