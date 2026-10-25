#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace json {

enum class JsonType { Null, Boolean, Number, String, Array, Object };

struct JsonValue {
	JsonType type = JsonType::Null;
	bool boolean = false;
	std::string number;
	std::string string;
	std::vector<JsonValue> array;
	std::vector<std::pair<std::string, JsonValue>> object;

	static JsonValue makeBoolean(bool value) {
		JsonValue result;
		result.type = JsonType::Boolean;
		result.boolean = value;
		return result;
	}

	static JsonValue makeNumber(std::string value) {
		JsonValue result;
		result.type = JsonType::Number;
		result.number = std::move(value);
		return result;
	}

	static JsonValue makeNumber(long long value) {
		return makeNumber(std::to_string(value));
	}

	static JsonValue makeString(std::string value) {
		JsonValue result;
		result.type = JsonType::String;
		result.string = std::move(value);
		return result;
	}

	static JsonValue makeArray() {
		JsonValue result;
		result.type = JsonType::Array;
		return result;
	}

	static JsonValue makeObject() {
		JsonValue result;
		result.type = JsonType::Object;
		return result;
	}
};

inline const JsonValue* get(const JsonValue& value, const char* key) {
	if (value.type != JsonType::Object) return nullptr;
	for (const auto& member : value.object) if (member.first == key) return &member.second;
	return nullptr;
}

inline JsonValue* get(JsonValue& value, const char* key) {
	if (value.type != JsonType::Object) return nullptr;
	for (auto& member : value.object) if (member.first == key) return &member.second;
	return nullptr;
}

inline void set(JsonValue& value, const char* key, JsonValue memberValue) {
	if (value.type != JsonType::Object) value = JsonValue::makeObject();
	for (auto& member : value.object) {
		if (member.first == key) {
			member.second = std::move(memberValue);
			return;
		}
	}
	value.object.emplace_back(key, std::move(memberValue));
}

inline std::string stringValue(const JsonValue& value, const char* key, const char* fallback = "") {
	const JsonValue* member = get(value, key);
	return member && member->type == JsonType::String ? member->string : fallback;
}

inline int intValue(const JsonValue& value, const char* key, int fallback = 0) {
	const JsonValue* member = get(value, key);
	if (!member || member->type != JsonType::Number) return fallback;
	char* end = nullptr;
	long result = std::strtol(member->number.c_str(), &end, 10);
	return end && !*end ? (int)result : fallback;
}

class JsonParser {
public:
	explicit JsonParser(const std::string& text) : text_(text) {}

	bool parse(JsonValue& value) {
		skipSpace();
		if (!parseValue(value)) return false;
		skipSpace();
		return pos_ == text_.size();
	}

private:
	const std::string& text_;
	size_t pos_ = 0;

	void skipSpace() {
		while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) pos_++;
	}

	bool consume(char ch) {
		if (pos_ >= text_.size() || text_[pos_] != ch) return false;
		pos_++;
		return true;
	}

	bool parseValue(JsonValue& value) {
		skipSpace();
		if (pos_ >= text_.size()) return false;
		char ch = text_[pos_];
		if (ch == '{') return parseObject(value);
		if (ch == '[') return parseArray(value);
		if (ch == '"') {
			value.type = JsonType::String;
			return parseString(value.string);
		}
		if (ch == 't') return parseLiteral("true", JsonValue::makeBoolean(true), value);
		if (ch == 'f') return parseLiteral("false", JsonValue::makeBoolean(false), value);
		if (ch == 'n') return parseLiteral("null", JsonValue(), value);
		return parseNumber(value);
	}

	bool parseLiteral(const char* literal, JsonValue literalValue, JsonValue& value) {
		size_t length = 0;
		while (literal[length]) length++;
		if (text_.compare(pos_, length, literal) != 0) return false;
		pos_ += length;
		value = std::move(literalValue);
		return true;
	}

	static int hexDigit(char ch) {
		if (ch >= '0' && ch <= '9') return ch - '0';
		if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
		if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
		return -1;
	}

	bool parseHex4(unsigned& value) {
		if (pos_ + 4 > text_.size()) return false;
		value = 0;
		for (int i = 0; i < 4; i++) {
			int digit = hexDigit(text_[pos_++]);
			if (digit < 0) return false;
			value = (value << 4) | (unsigned)digit;
		}
		return true;
	}

	static void appendUtf8(std::string& output, unsigned codepoint) {
		if (codepoint <= 0x7f) output += (char)codepoint;
		else if (codepoint <= 0x7ff) {
			output += (char)(0xc0 | (codepoint >> 6));
			output += (char)(0x80 | (codepoint & 0x3f));
		} else if (codepoint <= 0xffff) {
			output += (char)(0xe0 | (codepoint >> 12));
			output += (char)(0x80 | ((codepoint >> 6) & 0x3f));
			output += (char)(0x80 | (codepoint & 0x3f));
		} else {
			output += (char)(0xf0 | (codepoint >> 18));
			output += (char)(0x80 | ((codepoint >> 12) & 0x3f));
			output += (char)(0x80 | ((codepoint >> 6) & 0x3f));
			output += (char)(0x80 | (codepoint & 0x3f));
		}
	}

	bool parseString(std::string& value) {
		if (!consume('"')) return false;
		value.clear();
		while (pos_ < text_.size()) {
			unsigned char ch = (unsigned char)text_[pos_++];
			if (ch == '"') return true;
			if (ch < 0x20) return false;
			if (ch != '\\') {
				value += (char)ch;
				continue;
			}
			if (pos_ >= text_.size()) return false;
			char escape = text_[pos_++];
			if (escape == '"' || escape == '\\' || escape == '/') value += escape;
			else if (escape == 'b') value += '\b';
			else if (escape == 'f') value += '\f';
			else if (escape == 'n') value += '\n';
			else if (escape == 'r') value += '\r';
			else if (escape == 't') value += '\t';
			else if (escape == 'u') {
				unsigned codepoint = 0;
				if (!parseHex4(codepoint)) return false;
				if (codepoint >= 0xd800 && codepoint <= 0xdbff && pos_ + 6 <= text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
					pos_ += 2;
					unsigned low = 0;
					if (!parseHex4(low) || low < 0xdc00 || low > 0xdfff) return false;
					codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
				}
				appendUtf8(value, codepoint);
			} else return false;
		}
		return false;
	}

	bool parseNumber(JsonValue& value) {
		size_t start = pos_;
		if (pos_ < text_.size() && text_[pos_] == '-') pos_++;
		if (pos_ >= text_.size()) return false;
		if (text_[pos_] == '0') pos_++;
		else {
			if (text_[pos_] < '1' || text_[pos_] > '9') return false;
			while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') pos_++;
		}
		if (pos_ < text_.size() && text_[pos_] == '.') {
			pos_++;
			if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') return false;
			while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') pos_++;
		}
		if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
			pos_++;
			if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) pos_++;
			if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') return false;
			while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') pos_++;
		}
		value = JsonValue::makeNumber(text_.substr(start, pos_ - start));
		return true;
	}

	bool parseArray(JsonValue& value) {
		if (!consume('[')) return false;
		value = JsonValue::makeArray();
		skipSpace();
		if (consume(']')) return true;
		for (;;) {
			JsonValue item;
			if (!parseValue(item)) return false;
			value.array.push_back(std::move(item));
			skipSpace();
			if (consume(']')) return true;
			if (!consume(',')) return false;
			skipSpace();
		}
	}

	bool parseObject(JsonValue& value) {
		if (!consume('{')) return false;
		value = JsonValue::makeObject();
		skipSpace();
		if (consume('}')) return true;
		for (;;) {
			std::string key;
			if (!parseString(key)) return false;
			skipSpace();
			if (!consume(':')) return false;
			JsonValue memberValue;
			if (!parseValue(memberValue)) return false;
			value.object.emplace_back(std::move(key), std::move(memberValue));
			skipSpace();
			if (consume('}')) return true;
			if (!consume(',')) return false;
			skipSpace();
		}
	}
};

inline void appendEscaped(const std::string& value, std::string& output) {
	static const char* hex = "0123456789abcdef";
	output += '"';
	for (unsigned char ch : value) {
		if (ch == '"') output += "\\\"";
		else if (ch == '\\') output += "\\\\";
		else if (ch == '\b') output += "\\b";
		else if (ch == '\f') output += "\\f";
		else if (ch == '\n') output += "\\n";
		else if (ch == '\r') output += "\\r";
		else if (ch == '\t') output += "\\t";
		else if (ch < 0x20) {
			output += "\\u00";
			output += hex[(ch >> 4) & 0xf];
			output += hex[ch & 0xf];
		} else output += (char)ch;
	}
	output += '"';
}

inline void serializeValue(const JsonValue& value, std::string& output, int depth) {
	if (value.type == JsonType::Null) output += "null";
	else if (value.type == JsonType::Boolean) output += value.boolean ? "true" : "false";
	else if (value.type == JsonType::Number) output += value.number;
	else if (value.type == JsonType::String) appendEscaped(value.string, output);
	else if (value.type == JsonType::Array) {
		output += '[';
		if (!value.array.empty()) output += '\n';
		for (size_t i = 0; i < value.array.size(); i++) {
			output.append((size_t)(depth + 1) * 2, ' ');
			serializeValue(value.array[i], output, depth + 1);
			if (i + 1 < value.array.size()) output += ',';
			output += '\n';
		}
		if (!value.array.empty()) output.append((size_t)depth * 2, ' ');
		output += ']';
	} else {
		output += '{';
		if (!value.object.empty()) output += '\n';
		for (size_t i = 0; i < value.object.size(); i++) {
			output.append((size_t)(depth + 1) * 2, ' ');
			appendEscaped(value.object[i].first, output);
			output += ": ";
			serializeValue(value.object[i].second, output, depth + 1);
			if (i + 1 < value.object.size()) output += ',';
			output += '\n';
		}
		if (!value.object.empty()) output.append((size_t)depth * 2, ' ');
		output += '}';
	}
}

inline std::string serialize(const JsonValue& value) {
	std::string output;
	serializeValue(value, output, 0);
	output += '\n';
	return output;
}

}
