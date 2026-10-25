#include "steam_chunk.h"
#include "third_party/lzma/LzmaDec.h"
#include "third_party/miniz/miniz.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "third_party/zstd/zstd.h"
#include <windows.h>
#include <bcrypt.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

constexpr uint32_t kVZstdHeader = 0x615a5356u;
constexpr uint16_t kVZipHeader = 0x5a56u;
constexpr uint16_t kVZipFooter = 0x767au;
constexpr size_t kVZipHeaderBytes = 12;
constexpr size_t kVZipFooterBytes = 10;
constexpr size_t kVZstdHeaderBytes = 8;
constexpr size_t kVZstdFooterBytes = 15;

struct SensitiveBytes {
	std::vector<uint8_t> value;
	~SensitiveBytes() { if (!value.empty()) SecureZeroMemory(value.data(), value.size()); }
};

uint16_t readLe16(const uint8_t* data) {
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

uint32_t readLe32(const uint8_t* data) {
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
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

bool hashSha1(const uint8_t* data, size_t size, std::array<uint8_t, 20>& output) {
	if ((!data && size) || size > (std::numeric_limits<ULONG>::max)()) return false;
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0))) return false;
	NTSTATUS status = BCryptHash(algorithm, nullptr, 0, (PUCHAR)data, (ULONG)size, output.data(), (ULONG)output.size());
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return BCRYPT_SUCCESS(status);
}

class AesChunkDecryptor {
public:
	~AesChunkDecryptor() {
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

	bool decrypt(const uint8_t* data, size_t size, size_t limit, std::vector<uint8_t>& output) {
		output.clear();
		if (!data || size < 32 || size > limit || (size % 16) != 0 || size > (std::numeric_limits<ULONG>::max)()) return false;
		uint8_t iv[16] = {};
		ULONG written = 0;
		if (!BCRYPT_SUCCESS(BCryptDecrypt(ecbKey_, (PUCHAR)data, 16, nullptr, nullptr, 0, iv, sizeof(iv), &written, 0)) || written != sizeof(iv)) {
			SecureZeroMemory(iv, sizeof(iv));
			return false;
		}
		output.resize(size - 16);
		uint8_t ivCopy[16];
		std::memcpy(ivCopy, iv, sizeof(iv));
		NTSTATUS status = BCryptDecrypt(cbcKey_, (PUCHAR)data + 16, (ULONG)(size - 16), nullptr, ivCopy, sizeof(ivCopy), output.data(), (ULONG)output.size(), &written, BCRYPT_BLOCK_PADDING);
		SecureZeroMemory(iv, sizeof(iv));
		SecureZeroMemory(ivCopy, sizeof(ivCopy));
		if (!BCRYPT_SUCCESS(status) || !written || written > output.size()) {
			steamChunkSecureClear(output);
			return false;
		}
		output.resize(written);
		return true;
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

struct LzmaAllocator {
	ISzAlloc api;
	size_t limit;
	size_t used;
};

void* lzmaAlloc(ISzAllocPtr value, size_t size) {
	LzmaAllocator* allocator = (LzmaAllocator*)value;
	if (!size || size > allocator->limit - allocator->used || size > (std::numeric_limits<size_t>::max)() - sizeof(size_t)) return nullptr;
	size_t* block = (size_t*)std::malloc(sizeof(size_t) + size);
	if (!block) return nullptr;
	*block = size;
	allocator->used += size;
	return block + 1;
}

void lzmaFree(ISzAllocPtr value, void* address) {
	if (!address) return;
	LzmaAllocator* allocator = (LzmaAllocator*)value;
	size_t* block = (size_t*)address - 1;
	size_t size = *block;
	if (size <= allocator->used) allocator->used -= size;
	SecureZeroMemory(address, size);
	std::free(block);
}

bool decompressVZip(const uint8_t* data, size_t size, uint32_t expectedSize, const SteamChunkLimits& limits, std::vector<uint8_t>& output, std::string& error) {
	if (!data || size < kVZipHeaderBytes + kVZipFooterBytes || readLe16(data) != kVZipHeader || data[2] != 'a' || readLe16(data + size - 2) != kVZipFooter) { error = "Chunk VZip framing is invalid"; return false; }
	uint32_t dictionarySize = readLe32(data + 8);
	uint32_t footerCrc = readLe32(data + size - kVZipFooterBytes);
	uint32_t footerSize = readLe32(data + size - 6);
	if (footerSize != expectedSize || footerSize > limits.expandedBytes || dictionarySize > limits.lzmaDictionaryBytes) { error = "Chunk VZip size exceeds policy"; return false; }
	size_t payloadSize = size - kVZipHeaderBytes - kVZipFooterBytes;
	if (!payloadSize) { error = "Chunk VZip payload is empty"; return false; }
	output.assign(expectedSize, 0);
	SizeT destinationSize = output.size();
	SizeT sourceSize = payloadSize;
	ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
	LzmaAllocator allocator = { { lzmaAlloc, lzmaFree }, limits.lzmaDictionaryBytes, 0 };
	SRes result = LzmaDecode(output.data(), &destinationSize, data + kVZipHeaderBytes, &sourceSize, data + 7, LZMA_PROPS_SIZE, LZMA_FINISH_END, &status, &allocator.api);
	if (result != SZ_OK || destinationSize != expectedSize || sourceSize != payloadSize || (status != LZMA_STATUS_FINISHED_WITH_MARK && status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)) { error = "Chunk VZip decompression failed"; steamChunkSecureClear(output); return false; }
	mz_ulong crc = mz_crc32(MZ_CRC32_INIT, output.data(), output.size());
	if ((uint32_t)crc != footerCrc) { error = "Chunk VZip CRC is invalid"; steamChunkSecureClear(output); return false; }
	return true;
}

bool decompressVZstd(const uint8_t* data, size_t size, uint32_t expectedSize, const SteamChunkLimits& limits, std::vector<uint8_t>& output, std::string& error) {
	if (!data || size < kVZstdHeaderBytes + kVZstdFooterBytes || readLe32(data) != kVZstdHeader || data[size - 3] != 'z' || data[size - 2] != 's' || data[size - 1] != 'v') { error = "Chunk VZstd framing is invalid"; return false; }
	uint32_t headerCrc = readLe32(data + 4);
	uint32_t footerCrc = readLe32(data + size - kVZstdFooterBytes);
	uint32_t footerSize = readLe32(data + size - 11);
	if (headerCrc != footerCrc || footerSize != expectedSize || footerSize > limits.expandedBytes) { error = "Chunk VZstd metadata is invalid"; return false; }
	const uint8_t* payload = data + kVZstdHeaderBytes;
	size_t payloadSize = size - kVZstdHeaderBytes - kVZstdFooterBytes;
	if (!payloadSize) { error = "Chunk VZstd payload is empty"; return false; }
	unsigned long long frameSize = ZSTD_getFrameContentSize(payload, payloadSize);
	if (frameSize != ZSTD_CONTENTSIZE_UNKNOWN && frameSize != expectedSize) { error = "Chunk Zstd frame size is invalid"; return false; }
	ZSTD_FrameHeader frame = {};
	size_t frameResult = ZSTD_getFrameHeader(&frame, payload, payloadSize);
	if (ZSTD_isError(frameResult) || frameResult != 0 || frame.frameType != ZSTD_frame || frame.windowSize > limits.expandedBytes || frame.dictID != 0) { error = "Chunk Zstd frame policy is invalid"; return false; }
	output.assign(expectedSize, 0);
	size_t written = ZSTD_decompress(output.data(), output.size(), payload, payloadSize);
	if (ZSTD_isError(written) || written != expectedSize) { error = "Chunk VZstd decompression failed"; steamChunkSecureClear(output); return false; }
	mz_ulong crc = mz_crc32(MZ_CRC32_INIT, output.data(), output.size());
	if ((uint32_t)crc != footerCrc) { error = "Chunk VZstd CRC is invalid"; steamChunkSecureClear(output); return false; }
	return true;
}

bool decompressZip(const uint8_t* data, size_t size, uint32_t expectedSize, const SteamChunkLimits& limits, std::vector<uint8_t>& output, std::string& error) {
	if (!data || size < 22 || expectedSize > limits.expandedBytes) { error = "Chunk ZIP size is invalid"; return false; }
	mz_zip_archive zip = {};
	if (!mz_zip_reader_init_mem(&zip, data, size, 0)) { error = "Chunk ZIP framing is invalid"; return false; }
	bool ok = false;
	do {
		if (mz_zip_reader_get_num_files(&zip) != 1) { error = "Chunk ZIP entry count is invalid"; break; }
		mz_zip_archive_file_stat stat = {};
		if (!mz_zip_reader_file_stat(&zip, 0, &stat) || stat.m_is_directory || (stat.m_bit_flag & 1) || stat.m_uncomp_size != expectedSize || stat.m_uncomp_size > limits.expandedBytes || stat.m_comp_size > limits.decryptedBytes) { error = "Chunk ZIP entry is invalid"; break; }
		output.assign(expectedSize, 0);
		if (!mz_zip_reader_extract_to_mem(&zip, 0, output.data(), output.size(), 0)) { error = "Chunk ZIP integrity check failed"; steamChunkSecureClear(output); break; }
		ok = true;
	} while (false);
	mz_zip_reader_end(&zip);
	return ok;
}

}

SteamChunkLimits steamChunkDefaultLimits() {
	return SteamChunkLimits();
}

std::wstring steamChunkRequestTarget(uint32_t depotId, const std::array<uint8_t, 20>& sha) {
	if (!depotId) return L"";
	std::string hex = shaHex(sha);
	return L"/depot/" + std::to_wstring(depotId) + L"/chunk/" + std::wstring(hex.begin(), hex.end());
}

uint32_t steamChunkAdler32(const uint8_t* data, size_t size) {
	if (!data && size) return 0;
	uint32_t a = 0;
	uint32_t b = 0;
	for (size_t i = 0; i < size; i++) {
		a = (a + data[i]) % 65521u;
		b = (b + a) % 65521u;
	}
	return a | (b << 16);
}

bool steamChunkProcess(const uint8_t* data, size_t size, const uint8_t* depotKey, size_t depotKeySize, const std::array<uint8_t, 20>& expectedSha, uint32_t expectedChecksum, uint32_t expectedOriginalBytes, uint32_t expectedCompressedBytes, const SteamChunkLimits& limits, std::vector<uint8_t>& output, std::string& compression, std::string& error) {
	steamChunkSecureClear(output);
	compression.clear();
	error.clear();
	if (!data || !expectedOriginalBytes || !expectedCompressedBytes || size != expectedCompressedBytes || size > limits.encryptedBytes || expectedOriginalBytes > limits.expandedBytes) { error = "Chunk response size is invalid"; return false; }
	AesChunkDecryptor decryptor;
	if (!decryptor.initialize(depotKey, depotKeySize)) { error = "Chunk depot key is invalid"; return false; }
	SensitiveBytes decrypted;
	if (!decryptor.decrypt(data, size, limits.decryptedBytes, decrypted.value) || decrypted.value.size() < 4) { error = "Chunk decryption failed"; return false; }
	bool decoded = false;
	if (readLe32(decrypted.value.data()) == kVZstdHeader) {
		compression = "vzip-zstd";
		decoded = decompressVZstd(decrypted.value.data(), decrypted.value.size(), expectedOriginalBytes, limits, output, error);
	} else if (readLe16(decrypted.value.data()) == kVZipHeader && decrypted.value[2] == 'a') {
		compression = "vzip-lzma";
		decoded = decompressVZip(decrypted.value.data(), decrypted.value.size(), expectedOriginalBytes, limits, output, error);
	} else if (decrypted.value[0] == 'P' && decrypted.value[1] == 'K' && decrypted.value[2] == 0x03 && decrypted.value[3] == 0x04) {
		compression = "pkzip";
		decoded = decompressZip(decrypted.value.data(), decrypted.value.size(), expectedOriginalBytes, limits, output, error);
	} else error = "Chunk compression format is invalid";
	if (!decoded) { steamChunkSecureClear(output); return false; }
	if (steamChunkAdler32(output.data(), output.size()) != expectedChecksum) { error = "Chunk Adler-32 is invalid"; steamChunkSecureClear(output); return false; }
	std::array<uint8_t, 20> sha{};
	if (!hashSha1(output.data(), output.size(), sha) || sha != expectedSha) { error = "Chunk SHA-1 is invalid"; SecureZeroMemory(sha.data(), sha.size()); steamChunkSecureClear(output); return false; }
	SecureZeroMemory(sha.data(), sha.size());
	return true;
}

void steamChunkSecureClear(std::vector<uint8_t>& bytes) {
	if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
	bytes.clear();
}
