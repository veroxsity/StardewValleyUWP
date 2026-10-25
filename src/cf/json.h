#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// only enough to read the CurseForge api, doubles hold every id and count it returns exactly
namespace stardew_uwp
{
class JsonValue
{
public:
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;

    const JsonValue *Find(const std::string &name) const;
    const JsonValue *At(std::size_t index) const;
    std::size_t Size() const;

    std::string AsString(const std::string &fallback = std::string()) const;
    int64_t AsInt64(int64_t fallback = 0) const;
    bool AsBool(bool fallback = false) const;

    std::string StringField(const std::string &name, const std::string &fallback = std::string()) const;
    int64_t Int64Field(const std::string &name, int64_t fallback = 0) const;
    bool BoolField(const std::string &name, bool fallback = false) const;
};

bool JsonParse(const std::string &input, JsonValue &out, std::string &error);
}
