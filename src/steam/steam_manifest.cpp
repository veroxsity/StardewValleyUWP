#include "steam_manifest.h"
#include "json_value.h"
#include "third_party/miniz/miniz.h"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace {

constexpr uint32_t kPayloadMagic = 0x71f617d0u;
constexpr uint32_t kMetadataMagic = 0x1f4812beu;
constexpr uint32_t kSignatureMagic = 0x1b81b817u;
constexpr uint32_t kEndMagic = 0x32c415abu;
constexpr uint32_t kDirectoryFlag = 64u;
constexpr uint32_t kSymlinkFlag = 512u;
constexpr uint32_t kKnownFlags = 1023u;
constexpr size_t kMaxMetadataBytes = 64u * 1024u;
constexpr size_t kMaxSignatureBytes = 4096u;
constexpr uint64_t kFreshnessSeconds = 24ull * 60ull * 60ull;

struct SensitiveBytes {
	std::vector<uint8_t> value;
	~SensitiveBytes() {
		if (!value.empty()) SecureZeroMemory(value.data(), value.size());
	}
};

struct RawMetadata {
	uint32_t depotId = 0;
	uint64_t manifestId = 0;
	uint32_t creationTime = 0;
	bool filenamesEncrypted = false;
	uint64_t originalBytes = 0;
	uint64_t compressedBytes = 0;
	uint32_t uniqueChunks = 0;
	uint32_t encryptedCrc = 0;
	uint32_t clearCrc = 0;
	bool hasDepotId = false;
	bool hasManifestId = false;
	bool hasEncrypted = false;
	bool hasOriginalBytes = false;
	bool hasCompressedBytes = false;
	bool hasUniqueChunks = false;
	bool hasEncryptedCrc = false;
	bool hasClearCrc = false;
};

struct CaseInsensitiveLess {
	bool operator()(const std::wstring& left, const std::wstring& right) const {
		int result = CompareStringOrdinal(left.c_str(), (int)left.size(), right.c_str(), (int)right.size(), TRUE);
		return result == CSTR_LESS_THAN;
	}
};

bool addChecked(uint64_t& total, uint64_t value, uint64_t limit) {
	if (value > limit || total > limit - value) return false;
	total += value;
	return true;
}

bool getVarint(const uint8_t* data, size_t size, size_t& offset, uint64_t& value) {
	value = 0;
	for (int shift = 0; shift <= 63; shift += 7) {
		if (offset >= size) return false;
		uint8_t byte = data[offset++];
		if (shift == 63 && (byte & 0xfe)) return false;
		value |= (uint64_t)(byte & 0x7f) << shift;
		if (!(byte & 0x80)) return true;
	}
	return false;
}

bool getFixed32(const uint8_t* data, size_t size, size_t& offset, uint32_t& value) {
	if (offset > size || size - offset < 4) return false;
	value = (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) | ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
	offset += 4;
	return true;
}

bool getFixed64(const uint8_t* data, size_t size, size_t& offset, uint64_t& value) {
	if (offset > size || size - offset < 8) return false;
	value = 0;
	for (int i = 0; i < 8; i++) value |= (uint64_t)data[offset + i] << (i * 8);
	offset += 8;
	return true;
}

bool getBytes(const uint8_t* data, size_t size, size_t& offset, const uint8_t*& value, size_t& valueSize) {
	uint64_t length = 0;
	if (!getVarint(data, size, offset, length) || length > size - offset || length > (std::numeric_limits<size_t>::max)()) return false;
	value = data + offset;
	valueSize = (size_t)length;
	offset += valueSize;
	return true;
}

bool skipField(const uint8_t* data, size_t size, size_t& offset, uint32_t wire) {
	uint64_t value64 = 0;
	uint32_t value32 = 0;
	const uint8_t* bytes = nullptr;
	size_t byteCount = 0;
	if (wire == 0) return getVarint(data, size, offset, value64);
	if (wire == 1) return getFixed64(data, size, offset, value64);
	if (wire == 2) return getBytes(data, size, offset, bytes, byteCount);
	if (wire == 5) return getFixed32(data, size, offset, value32);
	return false;
}

uint32_t readLe32(const uint8_t* data) {
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

bool validUtf8(const std::string& value, std::wstring* wide = nullptr) {
	if (value.empty()) return false;
	if (value.size() > (size_t)(std::numeric_limits<int>::max)()) return false;
	int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), nullptr, 0);
	if (count <= 0) return false;
	if (wide) {
		wide->assign((size_t)count, L'\0');
		if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), &(*wide)[0], count)) return false;
	}
	return true;
}

bool reservedComponent(const std::wstring& component) {
	size_t dot = component.find(L'.');
	std::wstring stem = component.substr(0, dot);
	for (wchar_t& ch : stem) if (ch >= L'a' && ch <= L'z') ch = (wchar_t)(ch - L'a' + L'A');
	if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL") return true;
	if (stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) && stem[3] >= L'1' && stem[3] <= L'9') return true;
	return false;
}

bool normalizePath(const std::string& raw, size_t limit, std::string& normalized, std::wstring& duplicateKey) {
	normalized.clear();
	duplicateKey.clear();
	if (raw.empty() || raw.size() > limit || raw.find('\0') != std::string::npos) return false;
	std::string value = raw;
	for (char& ch : value) if (ch == '/') ch = '\\';
	if (value.empty() || value[0] == '\\' || value.find(':') != std::string::npos) return false;
	if (value.size() >= 2 && value[0] == '\\' && value[1] == '\\') return false;
	size_t offset = 0;
	std::string result;
	std::wstring wideResult;
	while (offset <= value.size()) {
		size_t slash = value.find('\\', offset);
		size_t end = slash == std::string::npos ? value.size() : slash;
		std::string component = value.substr(offset, end - offset);
		if (component.empty() || component == "." || component == "..") return false;
		if (component.back() == '.' || component.back() == ' ') return false;
		for (unsigned char ch : component) if (ch < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '|' || ch == '?' || ch == '*') return false;
		std::wstring wideComponent;
		if (!validUtf8(component, &wideComponent) || wideComponent.size() > 255 || reservedComponent(wideComponent)) return false;
		if (!result.empty()) {
			result.push_back('\\');
			wideResult.push_back(L'\\');
		}
		result += component;
		wideResult += wideComponent;
		if (slash == std::string::npos) break;
		offset = slash + 1;
	}
	if (result.empty() || result.size() > limit) return false;
	normalized = std::move(result);
	duplicateKey = std::move(wideResult);
	return true;
}

class AesFilenameDecryptor {
public:
	~AesFilenameDecryptor() {
		if (ecbKey_) BCryptDestroyKey(ecbKey_);
		if (cbcKey_) BCryptDestroyKey(cbcKey_);
		if (ecbAlg_) BCryptCloseAlgorithmProvider(ecbAlg_, 0);
		if (cbcAlg_) BCryptCloseAlgorithmProvider(cbcAlg_, 0);
		if (!ecbObject_.empty()) SecureZeroMemory(ecbObject_.data(), ecbObject_.size());
		if (!cbcObject_.empty()) SecureZeroMemory(cbcObject_.data(), cbcObject_.size());
	}

	bool initialize(const uint8_t* key, size_t keySize) {
		if (!key || keySize != 32) return false;
		return initializeOne(BCRYPT_CHAIN_MODE_ECB, key, ecbAlg_, ecbKey_, ecbObject_) && initializeOne(BCRYPT_CHAIN_MODE_CBC, key, cbcAlg_, cbcKey_, cbcObject_);
	}

	bool decrypt(const std::string& encoded, size_t encodedLimit, size_t pathLimit, std::string& output) {
		output.clear();
		if (encoded.empty() || encoded.size() > encodedLimit || encoded.size() > (size_t)(std::numeric_limits<DWORD>::max)()) return false;
		DWORD decodedSize = 0;
		if (!CryptStringToBinaryA(encoded.data(), (DWORD)encoded.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &decodedSize, nullptr, nullptr)) return false;
		if (decodedSize < 32 || decodedSize > encodedLimit || (decodedSize % 16) != 0) return false;
		SensitiveBytes decoded;
		decoded.value.resize(decodedSize);
		if (!CryptStringToBinaryA(encoded.data(), (DWORD)encoded.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, decoded.value.data(), &decodedSize, nullptr, nullptr)) return false;
		decoded.value.resize(decodedSize);
		uint8_t iv[16] = {};
		ULONG written = 0;
		if (!BCRYPT_SUCCESS(BCryptDecrypt(ecbKey_, decoded.value.data(), 16, nullptr, nullptr, 0, iv, sizeof(iv), &written, 0)) || written != sizeof(iv)) {
			SecureZeroMemory(iv, sizeof(iv));
			return false;
		}
		SensitiveBytes plaintext;
		plaintext.value.resize(decoded.value.size() - 16);
		uint8_t ivCopy[16];
		std::memcpy(ivCopy, iv, sizeof(iv));
		NTSTATUS status = BCryptDecrypt(cbcKey_, decoded.value.data() + 16, (ULONG)(decoded.value.size() - 16), nullptr, ivCopy, sizeof(ivCopy), plaintext.value.data(), (ULONG)plaintext.value.size(), &written, BCRYPT_BLOCK_PADDING);
		SecureZeroMemory(iv, sizeof(iv));
		SecureZeroMemory(ivCopy, sizeof(ivCopy));
		if (!BCRYPT_SUCCESS(status) || !written || written > plaintext.value.size() || plaintext.value[written - 1] != 0) return false;
		written--;
		if (!written || written > pathLimit || std::memchr(plaintext.value.data(), 0, written)) return false;
		output.assign((const char*)plaintext.value.data(), written);
		return validUtf8(output);
	}

private:
	bool initializeOne(const wchar_t* mode, const uint8_t* key, BCRYPT_ALG_HANDLE& algorithm, BCRYPT_KEY_HANDLE& keyHandle, std::vector<uint8_t>& object) {
		if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0))) return false;
		if (!BCRYPT_SUCCESS(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, (PUCHAR)mode, (ULONG)((wcslen(mode) + 1) * sizeof(wchar_t)), 0))) return false;
		DWORD objectLength = 0;
		ULONG resultLength = 0;
		if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objectLength, sizeof(objectLength), &resultLength, 0)) || resultLength != sizeof(objectLength) || !objectLength) return false;
		object.resize(objectLength);
		return BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(algorithm, &keyHandle, object.data(), objectLength, (PUCHAR)key, 32, 0));
	}

	BCRYPT_ALG_HANDLE ecbAlg_ = nullptr;
	BCRYPT_ALG_HANDLE cbcAlg_ = nullptr;
	BCRYPT_KEY_HANDLE ecbKey_ = nullptr;
	BCRYPT_KEY_HANDLE cbcKey_ = nullptr;
	std::vector<uint8_t> ecbObject_;
	std::vector<uint8_t> cbcObject_;
};

bool parseChunk(const uint8_t* data, size_t size, SteamManifestChunk& chunk) {
	chunk = SteamManifestChunk();
	bool hasSha = false;
	bool hasChecksum = false;
	bool hasOffset = false;
	bool hasOriginal = false;
	bool hasCompressed = false;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key) || !(key >> 3)) return false;
		uint32_t field = (uint32_t)(key >> 3);
		uint32_t wire = (uint32_t)(key & 7);
		if (field == 1 && wire == 2) {
			const uint8_t* bytes = nullptr;
			size_t count = 0;
			if (hasSha || !getBytes(data, size, offset, bytes, count) || count != chunk.sha.size()) return false;
			std::memcpy(chunk.sha.data(), bytes, count);
			hasSha = true;
		} else if (field == 2 && wire == 5) {
			if (hasChecksum || !getFixed32(data, size, offset, chunk.checksum)) return false;
			hasChecksum = true;
		} else if ((field == 3 || field == 4 || field == 5) && wire == 0) {
			uint64_t value = 0;
			if (!getVarint(data, size, offset, value)) return false;
			if (field == 3) {
				if (hasOffset) return false;
				chunk.offset = value;
				hasOffset = true;
			} else if (field == 4) {
				if (hasOriginal || value > 0xffffffffull) return false;
				chunk.originalBytes = (uint32_t)value;
				hasOriginal = true;
			} else {
				if (hasCompressed || value > 0xffffffffull) return false;
				chunk.compressedBytes = (uint32_t)value;
				hasCompressed = true;
			}
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return hasSha && hasChecksum && hasOffset && hasOriginal && hasCompressed;
}

bool parseFile(const uint8_t* data, size_t size, const SteamManifestLimits& limits, size_t& chunkCount, SteamManifestFile& file) {
	file = SteamManifestFile();
	bool hasPath = false;
	bool hasSize = false;
	bool hasFlags = false;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key) || !(key >> 3)) return false;
		uint32_t field = (uint32_t)(key >> 3);
		uint32_t wire = (uint32_t)(key & 7);
		if ((field == 1 || field == 4 || field == 5 || field == 7) && wire == 2) {
			const uint8_t* bytes = nullptr;
			size_t count = 0;
			if (!getBytes(data, size, offset, bytes, count)) return false;
			if (field == 1) {
				if (hasPath || count > limits.encryptedNameBytes) return false;
				file.path.assign((const char*)bytes, count);
				hasPath = true;
			} else if (field == 4) {
				if (count > 64) return false;
			} else if (field == 5) {
				if (file.hasContentSha || count != file.contentSha.size()) return false;
				std::memcpy(file.contentSha.data(), bytes, count);
				file.hasContentSha = true;
			} else {
				if (!file.linkTarget.empty() || count > limits.encryptedNameBytes) return false;
				file.linkTarget.assign((const char*)bytes, count);
			}
		} else if ((field == 2 || field == 3) && wire == 0) {
			uint64_t value = 0;
			if (!getVarint(data, size, offset, value)) return false;
			if (field == 2) {
				if (hasSize) return false;
				file.size = value;
				hasSize = true;
			} else {
				if (hasFlags || value > 0xffffffffull) return false;
				file.flags = (uint32_t)value;
				hasFlags = true;
			}
		} else if (field == 6 && wire == 2) {
			const uint8_t* nested = nullptr;
			size_t nestedSize = 0;
			if (!getBytes(data, size, offset, nested, nestedSize) || chunkCount >= limits.chunkCount) return false;
			SteamManifestChunk chunk;
			if (!parseChunk(nested, nestedSize, chunk)) return false;
			file.chunks.push_back(chunk);
			chunkCount++;
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return hasPath && hasSize && hasFlags;
}

bool parsePayload(const uint8_t* data, size_t size, const SteamManifestLimits& limits, SteamManifestMetadata& metadata) {
	metadata.files.clear();
	size_t chunkCount = 0;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key) || !(key >> 3)) return false;
		uint32_t field = (uint32_t)(key >> 3);
		uint32_t wire = (uint32_t)(key & 7);
		if (field == 1 && wire == 2) {
			const uint8_t* nested = nullptr;
			size_t nestedSize = 0;
			if (!getBytes(data, size, offset, nested, nestedSize) || metadata.files.size() >= limits.fileCount) return false;
			SteamManifestFile file;
			if (!parseFile(nested, nestedSize, limits, chunkCount, file)) return false;
			metadata.files.push_back(std::move(file));
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return !metadata.files.empty();
}

bool parseMetadata(const uint8_t* data, size_t size, RawMetadata& metadata) {
	metadata = RawMetadata();
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key) || !(key >> 3)) return false;
		uint32_t field = (uint32_t)(key >> 3);
		uint32_t wire = (uint32_t)(key & 7);
		if (field >= 1 && field <= 9 && wire == 0) {
			uint64_t value = 0;
			if (!getVarint(data, size, offset, value)) return false;
			if ((field == 1 || field == 3 || field == 7 || field == 8 || field == 9) && value > 0xffffffffull) return false;
			switch (field) {
				case 1: if (metadata.hasDepotId) return false; metadata.depotId = (uint32_t)value; metadata.hasDepotId = true; break;
				case 2: if (metadata.hasManifestId) return false; metadata.manifestId = value; metadata.hasManifestId = true; break;
				case 3: metadata.creationTime = (uint32_t)value; break;
				case 4: if (metadata.hasEncrypted || value > 1) return false; metadata.filenamesEncrypted = value != 0; metadata.hasEncrypted = true; break;
				case 5: if (metadata.hasOriginalBytes) return false; metadata.originalBytes = value; metadata.hasOriginalBytes = true; break;
				case 6: if (metadata.hasCompressedBytes) return false; metadata.compressedBytes = value; metadata.hasCompressedBytes = true; break;
				case 7: if (metadata.hasUniqueChunks) return false; metadata.uniqueChunks = (uint32_t)value; metadata.hasUniqueChunks = true; break;
				case 8: if (metadata.hasEncryptedCrc) return false; metadata.encryptedCrc = (uint32_t)value; metadata.hasEncryptedCrc = true; break;
				case 9: if (metadata.hasClearCrc) return false; metadata.clearCrc = (uint32_t)value; metadata.hasClearCrc = true; break;
				default: break;
			}
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return metadata.hasDepotId && metadata.hasManifestId && metadata.hasEncrypted && metadata.hasOriginalBytes && metadata.hasCompressedBytes && metadata.hasUniqueChunks;
}

bool parseSignature(const uint8_t* data, size_t size) {
	bool found = false;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key = 0;
		if (!getVarint(data, size, offset, key) || !(key >> 3)) return false;
		uint32_t field = (uint32_t)(key >> 3);
		uint32_t wire = (uint32_t)(key & 7);
		if (field == 1 && wire == 2) {
			const uint8_t* bytes = nullptr;
			size_t count = 0;
			if (found || !getBytes(data, size, offset, bytes, count) || !count || count > kMaxSignatureBytes) return false;
			found = true;
		} else if (!skipField(data, size, offset, wire)) return false;
	}
	return found;
}

std::string shaHex(const std::array<uint8_t, 20>& sha) {
	static const char digits[] = "0123456789abcdef";
	std::string value(40, '0');
	for (size_t i = 0; i < sha.size(); i++) {
		value[i * 2] = digits[sha[i] >> 4];
		value[i * 2 + 1] = digits[sha[i] & 15];
	}
	return value;
}

bool parseHex(const std::string& value, std::array<uint8_t, 20>& sha) {
	if (value.size() != sha.size() * 2) return false;
	for (size_t i = 0; i < sha.size(); i++) {
		auto nibble = [](char ch, uint8_t& result) {
			if (ch >= '0' && ch <= '9') result = (uint8_t)(ch - '0');
			else if (ch >= 'a' && ch <= 'f') result = (uint8_t)(ch - 'a' + 10);
			else if (ch >= 'A' && ch <= 'F') result = (uint8_t)(ch - 'A' + 10);
			else return false;
			return true;
		};
		uint8_t high = 0;
		uint8_t low = 0;
		if (!nibble(value[i * 2], high) || !nibble(value[i * 2 + 1], low)) return false;
		sha[i] = (uint8_t)((high << 4) | low);
	}
	return true;
}

bool jsonUInt64(const json::JsonValue& value, uint64_t& result) {
	if (value.type != json::JsonType::Number || value.number.empty() || value.number[0] == '-') return false;
	char* end = nullptr;
	errno = 0;
	unsigned long long parsed = _strtoui64(value.number.c_str(), &end, 10);
	if (errno == ERANGE || !end || *end) return false;
	result = (uint64_t)parsed;
	return true;
}

bool jsonUInt32(const json::JsonValue& value, uint32_t& result) {
	uint64_t parsed = 0;
	if (!jsonUInt64(value, parsed) || parsed > 0xffffffffull) return false;
	result = (uint32_t)parsed;
	return true;
}

const json::JsonValue* member(const json::JsonValue& value, const char* name) {
	return json::get(value, name);
}

bool validateContent(SteamManifestMetadata& metadata, const SteamManifestLimits& limits, std::string& error) {
	if (!metadata.depotId || !metadata.manifestId || metadata.files.empty() || metadata.files.size() > limits.fileCount) { error = "Manifest identity or file count is invalid"; return false; }
	if (metadata.originalBytes > limits.aggregateBytes || metadata.compressedBytes > limits.aggregateBytes) { error = "Manifest aggregate size exceeds policy"; return false; }
	std::set<std::wstring, CaseInsensitiveLess> paths;
	std::map<std::array<uint8_t, 20>, std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> uniqueChunks;
	uint64_t originalTotal = 0;
	uint64_t compressedTotal = 0;
	size_t chunkCount = 0;
	for (auto& file : metadata.files) {
		std::wstring duplicateKey;
		std::string normalized;
		if (!normalizePath(file.path, limits.pathBytes, normalized, duplicateKey) || !paths.insert(duplicateKey).second) { error = "Manifest path is unsafe or duplicated"; return false; }
		file.path = std::move(normalized);
		if (file.flags & ~kKnownFlags) { error = "Manifest file flags are invalid"; return false; }
		bool directory = (file.flags & kDirectoryFlag) != 0;
		bool symlink = (file.flags & kSymlinkFlag) != 0;
		if (directory && symlink) { error = "Manifest file type is invalid"; return false; }
		if (file.size > limits.fileBytes || !addChecked(originalTotal, file.size, limits.aggregateBytes)) { error = "Manifest file size exceeds policy"; return false; }
		if (symlink) {
			std::wstring linkKey;
			std::string normalizedLink;
			if (file.linkTarget.empty() || !normalizePath(file.linkTarget, limits.pathBytes, normalizedLink, linkKey)) { error = "Manifest link target is unsafe"; return false; }
			file.linkTarget = std::move(normalizedLink);
		} else if (!file.linkTarget.empty()) { error = "Manifest link target is unexpected"; return false; }
		if (directory || symlink) {
			if (file.size || !file.chunks.empty()) { error = "Manifest non-file entry has content"; return false; }
			continue;
		}
		if (!file.hasContentSha) { error = "Manifest file hash is missing"; return false; }
		if (!file.size) {
			if (!file.chunks.empty()) { error = "Manifest empty file has chunks"; return false; }
			continue;
		}
		if (file.chunks.empty()) { error = "Manifest file has no chunks"; return false; }
		std::sort(file.chunks.begin(), file.chunks.end(), [](const SteamManifestChunk& left, const SteamManifestChunk& right) {
			if (left.offset != right.offset) return left.offset < right.offset;
			return left.sha < right.sha;
		});
		uint64_t expectedOffset = 0;
		for (const auto& chunk : file.chunks) {
			if (++chunkCount > limits.chunkCount || !chunk.originalBytes || !chunk.compressedBytes || chunk.originalBytes > limits.chunkOriginalBytes || chunk.compressedBytes > limits.chunkCompressedBytes) { error = "Manifest chunk size exceeds policy"; return false; }
			if (chunk.offset != expectedOffset || chunk.originalBytes > file.size - expectedOffset) { error = "Manifest chunk range is invalid"; return false; }
			expectedOffset += chunk.originalBytes;
			auto inserted = uniqueChunks.emplace(chunk.sha, std::make_pair(chunk.checksum, std::make_pair(chunk.originalBytes, chunk.compressedBytes)));
			if (!inserted.second && inserted.first->second != std::make_pair(chunk.checksum, std::make_pair(chunk.originalBytes, chunk.compressedBytes))) { error = "Manifest chunk identity is inconsistent"; return false; }
		}
		if (expectedOffset != file.size) { error = "Manifest chunk coverage is incomplete"; return false; }
	}
	for (const auto& chunk : uniqueChunks) if (!addChecked(compressedTotal, chunk.second.second.second, limits.aggregateBytes)) { error = "Manifest compressed size overflows"; return false; }
	if (originalTotal != metadata.originalBytes) { error = "Manifest original total mismatch expected=" + std::to_string(metadata.originalBytes) + " actual=" + std::to_string(originalTotal); return false; }
	if (compressedTotal != metadata.compressedBytes) { error = "Manifest compressed total mismatch expected=" + std::to_string(metadata.compressedBytes) + " actual=" + std::to_string(compressedTotal); return false; }
	if (uniqueChunks.size() != metadata.uniqueChunks) { error = "Manifest unique chunk mismatch expected=" + std::to_string(metadata.uniqueChunks) + " actual=" + std::to_string(uniqueChunks.size()); return false; }
	return true;
}

json::JsonValue number(uint64_t value) {
	return json::JsonValue::makeNumber(std::to_string(value));
}

}

SteamManifestLimits steamManifestDefaultLimits() {
	return SteamManifestLimits();
}

uint64_t steamManifestFreshnessSeconds() {
	return kFreshnessSeconds;
}

std::wstring steamManifestRequestTarget(uint32_t depotId, uint64_t manifestId, uint64_t requestCode) {
	if (!depotId || !manifestId || !requestCode) return L"";
	return L"/depot/" + std::to_wstring(depotId) + L"/manifest/" + std::to_wstring(manifestId) + L"/5/" + std::to_wstring(requestCode);
}

bool steamManifestParseZip(const uint8_t* data, size_t size, const uint8_t* depotKey, size_t depotKeySize, uint32_t expectedDepotId, uint64_t expectedManifestId, const SteamManifestLimits& limits, SteamManifestMetadata& metadata, std::string& error) {
	metadata = SteamManifestMetadata();
	error.clear();
	if (!data || !size || size > limits.compressedBytes || !expectedDepotId || !expectedManifestId) { error = "Manifest response size or identity is invalid"; return false; }
	mz_zip_archive zip = {};
	if (!mz_zip_reader_init_mem(&zip, data, size, 0)) { error = "Manifest response is not a valid ZIP"; return false; }
	bool result = false;
	do {
		if (mz_zip_reader_get_num_files(&zip) != 1) { error = "Manifest ZIP entry count is invalid"; break; }
		mz_zip_archive_file_stat stat = {};
		if (!mz_zip_reader_file_stat(&zip, 0, &stat) || stat.m_is_directory || (stat.m_bit_flag & 1) || (stat.m_method != 0 && stat.m_method != MZ_DEFLATED) || stat.m_uncomp_size == 0 || stat.m_uncomp_size > limits.expandedBytes || stat.m_comp_size > limits.compressedBytes || !stat.m_filename || std::strlen(stat.m_filename) > 64 || std::strpbrk(stat.m_filename, "\\/:") != nullptr) { error = "Manifest ZIP entry is invalid"; break; }
		SensitiveBytes expanded;
		expanded.value.resize((size_t)stat.m_uncomp_size);
		if (!mz_zip_reader_extract_to_mem(&zip, 0, expanded.value.data(), expanded.value.size(), 0)) { error = "Manifest ZIP integrity check failed"; break; }
		const uint8_t* payload = nullptr;
		size_t payloadSize = 0;
		const uint8_t* metadataBytes = nullptr;
		size_t metadataSize = 0;
		const uint8_t* signature = nullptr;
		size_t signatureSize = 0;
		size_t offset = 0;
		bool ended = false;
		while (offset < expanded.value.size()) {
			if (expanded.value.size() - offset < 4) { error = "Manifest framing is truncated"; break; }
			uint32_t magic = readLe32(expanded.value.data() + offset);
			offset += 4;
			if (magic == kEndMagic) {
				ended = offset == expanded.value.size();
				if (!ended) error = "Manifest has trailing data";
				break;
			}
			if (expanded.value.size() - offset < 4) { error = "Manifest section length is truncated"; break; }
			uint32_t sectionSize = readLe32(expanded.value.data() + offset);
			offset += 4;
			if (!sectionSize || sectionSize > expanded.value.size() - offset) { error = "Manifest section size is invalid"; break; }
			if (magic == kPayloadMagic && !payload) { payload = expanded.value.data() + offset; payloadSize = sectionSize; }
			else if (magic == kMetadataMagic && !metadataBytes && sectionSize <= kMaxMetadataBytes) { metadataBytes = expanded.value.data() + offset; metadataSize = sectionSize; }
			else if (magic == kSignatureMagic && !signature && sectionSize <= kMaxSignatureBytes) { signature = expanded.value.data() + offset; signatureSize = sectionSize; }
			else { error = "Manifest section is duplicated or unknown"; break; }
			offset += sectionSize;
		}
		if (!ended || !payload || !metadataBytes || !signature) {
			if (error.empty()) error = "Manifest sections are incomplete";
			break;
		}
		RawMetadata raw;
		if (!parseMetadata(metadataBytes, metadataSize, raw) || !parseSignature(signature, signatureSize)) { error = "Manifest metadata or signature section is invalid"; break; }
		if (raw.depotId != expectedDepotId || raw.manifestId != expectedManifestId) { error = "Manifest identity does not match the request"; break; }
		uint8_t lengthBytes[4] = {(uint8_t)payloadSize, (uint8_t)(payloadSize >> 8), (uint8_t)(payloadSize >> 16), (uint8_t)(payloadSize >> 24)};
		mz_ulong crc = mz_crc32(MZ_CRC32_INIT, lengthBytes, sizeof(lengthBytes));
		crc = mz_crc32(crc, payload, payloadSize);
		uint32_t expectedCrc = raw.filenamesEncrypted ? raw.encryptedCrc : raw.clearCrc;
		bool hasCrc = raw.filenamesEncrypted ? raw.hasEncryptedCrc : raw.hasClearCrc;
		if (!hasCrc || (uint32_t)crc != expectedCrc) { error = "Manifest payload checksum is invalid"; break; }
		metadata.depotId = raw.depotId;
		metadata.manifestId = raw.manifestId;
		metadata.creationTime = raw.creationTime;
		metadata.originalBytes = raw.originalBytes;
		metadata.compressedBytes = raw.compressedBytes;
		metadata.uniqueChunks = raw.uniqueChunks;
		if (!parsePayload(payload, payloadSize, limits, metadata)) { error = "Manifest payload is malformed or exceeds policy"; break; }
		if (raw.filenamesEncrypted) {
			AesFilenameDecryptor decryptor;
			if (!decryptor.initialize(depotKey, depotKeySize)) { error = "Manifest filename key is invalid"; break; }
			bool decrypted = true;
			for (auto& file : metadata.files) {
				std::string path;
				if (!decryptor.decrypt(file.path, limits.encryptedNameBytes, limits.pathBytes, path)) { decrypted = false; break; }
				file.path = std::move(path);
				if (!file.linkTarget.empty()) {
					std::string link;
					if (!decryptor.decrypt(file.linkTarget, limits.encryptedNameBytes, limits.pathBytes, link)) { decrypted = false; break; }
					file.linkTarget = std::move(link);
				}
			}
			if (!decrypted) { error = "Manifest filename decryption failed"; break; }
		} else if (depotKeySize && depotKeySize != 32) { error = "Manifest filename key is invalid"; break; }
		if (!validateContent(metadata, limits, error)) break;
		result = true;
	} while (false);
	mz_zip_reader_end(&zip);
	if (!result) metadata = SteamManifestMetadata();
	return result;
}

bool steamManifestValidate(const SteamManifestMetadata& metadata, const SteamManifestLimits& limits, std::string& error) {
	SteamManifestMetadata checked = metadata;
	error.clear();
	if (checked.steamId.empty() || checked.steamId.size() > 32 || !checked.appId || checked.buildId.empty() || checked.buildId.size() > 64 || !checked.retrievedAt) { error = "Manifest cache identity is invalid"; return false; }
	return validateContent(checked, limits, error);
}

std::string steamManifestSerialize(const SteamManifestMetadata& metadata, const SteamManifestLimits& limits) {
	std::string error;
	if (!steamManifestValidate(metadata, limits, error)) return "";
	json::JsonValue root = json::JsonValue::makeObject();
	json::set(root, "version", number(1));
	json::set(root, "steamId", json::JsonValue::makeString(metadata.steamId));
	json::set(root, "appId", number(metadata.appId));
	json::set(root, "depotId", number(metadata.depotId));
	json::set(root, "manifestId", number(metadata.manifestId));
	json::set(root, "buildId", json::JsonValue::makeString(metadata.buildId));
	json::set(root, "retrievedAt", number(metadata.retrievedAt));
	json::set(root, "creationTime", number(metadata.creationTime));
	json::set(root, "originalBytes", number(metadata.originalBytes));
	json::set(root, "compressedBytes", number(metadata.compressedBytes));
	json::set(root, "uniqueChunks", number(metadata.uniqueChunks));
	json::set(root, "fileCount", number(metadata.files.size()));
	size_t totalChunks = 0;
	json::JsonValue files = json::JsonValue::makeArray();
	for (const auto& file : metadata.files) {
		json::JsonValue item = json::JsonValue::makeObject();
		json::set(item, "path", json::JsonValue::makeString(file.path));
		json::set(item, "linkTarget", json::JsonValue::makeString(file.linkTarget));
		json::set(item, "size", number(file.size));
		json::set(item, "flags", number(file.flags));
		json::set(item, "contentSha", json::JsonValue::makeString(file.hasContentSha ? shaHex(file.contentSha) : ""));
		json::JsonValue chunks = json::JsonValue::makeArray();
		for (const auto& chunk : file.chunks) {
			json::JsonValue value = json::JsonValue::makeObject();
			json::set(value, "sha", json::JsonValue::makeString(shaHex(chunk.sha)));
			json::set(value, "checksum", number(chunk.checksum));
			json::set(value, "offset", number(chunk.offset));
			json::set(value, "originalBytes", number(chunk.originalBytes));
			json::set(value, "compressedBytes", number(chunk.compressedBytes));
			chunks.array.push_back(std::move(value));
			totalChunks++;
		}
		json::set(item, "chunks", std::move(chunks));
		files.array.push_back(std::move(item));
	}
	json::set(root, "chunkCount", number(totalChunks));
	json::set(root, "files", std::move(files));
	std::string text = json::serialize(root);
	return text.size() <= limits.cacheBytes ? text : "";
}

bool steamManifestDeserialize(const std::string& text, const SteamManifestLimits& limits, SteamManifestMetadata& metadata) {
	metadata = SteamManifestMetadata();
	if (text.empty() || text.size() > limits.cacheBytes) return false;
	json::JsonValue root;
	json::JsonParser parser(text);
	if (!parser.parse(root) || root.type != json::JsonType::Object) return false;
	uint64_t version = 0;
	uint64_t fileCount = 0;
	uint64_t chunkCountExpected = 0;
	const auto* versionValue = member(root, "version");
	const auto* appIdValue = member(root, "appId");
	const auto* depotIdValue = member(root, "depotId");
	const auto* manifestIdValue = member(root, "manifestId");
	const auto* retrievedAtValue = member(root, "retrievedAt");
	const auto* creationTimeValue = member(root, "creationTime");
	const auto* originalValue = member(root, "originalBytes");
	const auto* compressedValue = member(root, "compressedBytes");
	const auto* uniqueValue = member(root, "uniqueChunks");
	const auto* fileCountValue = member(root, "fileCount");
	const auto* chunkCountValue = member(root, "chunkCount");
	if (!versionValue || !jsonUInt64(*versionValue, version) || version != 1 || !appIdValue || !jsonUInt32(*appIdValue, metadata.appId) || !depotIdValue || !jsonUInt32(*depotIdValue, metadata.depotId) || !manifestIdValue || !jsonUInt64(*manifestIdValue, metadata.manifestId) || !retrievedAtValue || !jsonUInt64(*retrievedAtValue, metadata.retrievedAt) || !creationTimeValue || !jsonUInt32(*creationTimeValue, metadata.creationTime) || !originalValue || !jsonUInt64(*originalValue, metadata.originalBytes) || !compressedValue || !jsonUInt64(*compressedValue, metadata.compressedBytes) || !uniqueValue || !jsonUInt32(*uniqueValue, metadata.uniqueChunks) || !fileCountValue || !jsonUInt64(*fileCountValue, fileCount) || !chunkCountValue || !jsonUInt64(*chunkCountValue, chunkCountExpected)) return false;
	metadata.steamId = json::stringValue(root, "steamId");
	metadata.buildId = json::stringValue(root, "buildId");
	const auto* files = member(root, "files");
	if (!files || files->type != json::JsonType::Array || files->array.size() != fileCount || fileCount > limits.fileCount || chunkCountExpected > limits.chunkCount) return false;
	size_t chunkCount = 0;
	for (const auto& item : files->array) {
		if (item.type != json::JsonType::Object) return false;
		SteamManifestFile file;
		file.path = json::stringValue(item, "path");
		file.linkTarget = json::stringValue(item, "linkTarget");
		const auto* sizeValue = member(item, "size");
		const auto* flagsValue = member(item, "flags");
		const auto* shaValue = member(item, "contentSha");
		const auto* chunks = member(item, "chunks");
		if (!sizeValue || !jsonUInt64(*sizeValue, file.size) || !flagsValue || !jsonUInt32(*flagsValue, file.flags) || !shaValue || shaValue->type != json::JsonType::String || !chunks || chunks->type != json::JsonType::Array) return false;
		if (!shaValue->string.empty()) {
			if (!parseHex(shaValue->string, file.contentSha)) return false;
			file.hasContentSha = true;
		}
		if (chunkCount > limits.chunkCount - chunks->array.size()) return false;
		for (const auto& value : chunks->array) {
			if (value.type != json::JsonType::Object) return false;
			SteamManifestChunk chunk;
			const auto* chunkSha = member(value, "sha");
			const auto* checksum = member(value, "checksum");
			const auto* offset = member(value, "offset");
			const auto* original = member(value, "originalBytes");
			const auto* compressed = member(value, "compressedBytes");
			if (!chunkSha || chunkSha->type != json::JsonType::String || !parseHex(chunkSha->string, chunk.sha) || !checksum || !jsonUInt32(*checksum, chunk.checksum) || !offset || !jsonUInt64(*offset, chunk.offset) || !original || !jsonUInt32(*original, chunk.originalBytes) || !compressed || !jsonUInt32(*compressed, chunk.compressedBytes)) return false;
			file.chunks.push_back(chunk);
			chunkCount++;
		}
		metadata.files.push_back(std::move(file));
	}
	if (chunkCount != chunkCountExpected) return false;
	std::string error;
	return steamManifestValidate(metadata, limits, error);
}

SteamManifestCacheState steamManifestClassify(const SteamManifestMetadata& metadata, const std::string& steamId, uint32_t appId, uint32_t depotId, uint64_t manifestId, const std::string& buildId, uint64_t now, uint64_t freshnessSeconds) {
	std::string error;
	if (!steamManifestValidate(metadata, steamManifestDefaultLimits(), error)) return SteamManifestCacheState::Invalid;
	if (metadata.steamId != steamId || metadata.appId != appId || metadata.depotId != depotId || metadata.manifestId != manifestId || metadata.buildId != buildId || !freshnessSeconds || metadata.retrievedAt > now || now - metadata.retrievedAt > freshnessSeconds) return SteamManifestCacheState::Expired;
	return SteamManifestCacheState::Ready;
}

void steamManifestSecureClear(SteamManifestMetadata& metadata) {
	for (auto& file : metadata.files) {
		if (!file.path.empty()) SecureZeroMemory(&file.path[0], file.path.size());
		if (!file.linkTarget.empty()) SecureZeroMemory(&file.linkTarget[0], file.linkTarget.size());
		SecureZeroMemory(file.contentSha.data(), file.contentSha.size());
		for (auto& chunk : file.chunks) SecureZeroMemory(chunk.sha.data(), chunk.sha.size());
	}
	if (!metadata.steamId.empty()) SecureZeroMemory(&metadata.steamId[0], metadata.steamId.size());
	if (!metadata.buildId.empty()) SecureZeroMemory(&metadata.buildId[0], metadata.buildId.size());
	metadata = SteamManifestMetadata();
}
