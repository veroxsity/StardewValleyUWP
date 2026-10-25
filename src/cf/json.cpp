#include "json.h"

#include <cstdlib>

namespace stardew_uwp
{
const JsonValue *JsonValue::Find(const std::string &name) const
{
    for (const auto &member : members)
    {
        if (member.first == name)
        {
            return &member.second;
        }
    }
    return nullptr;
}

const JsonValue *JsonValue::At(std::size_t index) const
{
    return index < items.size() ? &items[index] : nullptr;
}

std::size_t JsonValue::Size() const
{
    return kind == Kind::Array ? items.size() : members.size();
}

std::string JsonValue::AsString(const std::string &fallback) const
{
    return kind == Kind::String ? text : fallback;
}

int64_t JsonValue::AsInt64(int64_t fallback) const
{
    return kind == Kind::Number ? static_cast<int64_t>(number) : fallback;
}

bool JsonValue::AsBool(bool fallback) const
{
    return kind == Kind::Bool ? boolean : fallback;
}

std::string JsonValue::StringField(const std::string &name, const std::string &fallback) const
{
    const JsonValue *value = Find(name);
    return value == nullptr ? fallback : value->AsString(fallback);
}

int64_t JsonValue::Int64Field(const std::string &name, int64_t fallback) const
{
    const JsonValue *value = Find(name);
    return value == nullptr ? fallback : value->AsInt64(fallback);
}

bool JsonValue::BoolField(const std::string &name, bool fallback) const
{
    const JsonValue *value = Find(name);
    return value == nullptr ? fallback : value->AsBool(fallback);
}

namespace
{
constexpr int kMaxDepth = 64;

struct Parser
{
    const std::string &input;
    std::size_t at = 0;
    int depth = 0;
    std::string error;

    explicit Parser(const std::string &source) : input(source) {}

    void SkipSpace()
    {
        while (at < input.size())
        {
            const char ch = input[at];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
            {
                at++;
                continue;
            }
            break;
        }
    }

    bool Fail(const char *reason)
    {
        if (error.empty())
        {
            error = std::string(reason) + " at " + std::to_string(at);
        }
        return false;
    }

    void AppendUtf8(uint32_t code, std::string &out)
    {
        if (code < 0x80)
        {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else if (code < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool ParseHex4(uint32_t &value)
    {
        if (at + 4 > input.size())
        {
            return Fail("truncated escape");
        }

        value = 0;
        for (int i = 0; i < 4; i++)
        {
            const char ch = input[at++];
            value <<= 4;
            if (ch >= '0' && ch <= '9')
            {
                value |= static_cast<uint32_t>(ch - '0');
            }
            else if (ch >= 'a' && ch <= 'f')
            {
                value |= static_cast<uint32_t>(ch - 'a' + 10);
            }
            else if (ch >= 'A' && ch <= 'F')
            {
                value |= static_cast<uint32_t>(ch - 'A' + 10);
            }
            else
            {
                return Fail("bad hex escape");
            }
        }
        return true;
    }

    bool ParseString(std::string &out)
    {
        if (at >= input.size() || input[at] != '"')
        {
            return Fail("expected a string");
        }
        at++;

        while (at < input.size())
        {
            const char ch = input[at++];
            if (ch == '"')
            {
                return true;
            }
            if (ch != '\\')
            {
                out.push_back(ch);
                continue;
            }
            if (at >= input.size())
            {
                return Fail("truncated escape");
            }

            const char escape = input[at++];
            switch (escape)
            {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
            {
                uint32_t code = 0;
                if (!ParseHex4(code))
                {
                    return false;
                }
                // a surrogate pair is two escapes, and mod titles do carry emoji
                if (code >= 0xD800 && code <= 0xDBFF && at + 1 < input.size() && input[at] == '\\' && input[at + 1] == 'u')
                {
                    const std::size_t mark = at;
                    at += 2;
                    uint32_t low = 0;
                    if (ParseHex4(low) && low >= 0xDC00 && low <= 0xDFFF)
                    {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    else
                    {
                        at = mark;
                    }
                }
                AppendUtf8(code, out);
                break;
            }
            default:
                return Fail("unknown escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseValue(JsonValue &out)
    {
        if (depth >= kMaxDepth)
        {
            return Fail("nested too deep");
        }

        SkipSpace();
        if (at >= input.size())
        {
            return Fail("unexpected end");
        }

        const char ch = input[at];
        if (ch == '{')
        {
            return ParseObject(out);
        }
        if (ch == '[')
        {
            return ParseArray(out);
        }
        if (ch == '"')
        {
            out.kind = JsonValue::Kind::String;
            return ParseString(out.text);
        }
        if (input.compare(at, 4, "true") == 0)
        {
            at += 4;
            out.kind = JsonValue::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (input.compare(at, 5, "false") == 0)
        {
            at += 5;
            out.kind = JsonValue::Kind::Bool;
            out.boolean = false;
            return true;
        }
        if (input.compare(at, 4, "null") == 0)
        {
            at += 4;
            out.kind = JsonValue::Kind::Null;
            return true;
        }
        return ParseNumber(out);
    }

    bool ParseNumber(JsonValue &out)
    {
        const std::size_t start = at;
        if (at < input.size() && (input[at] == '-' || input[at] == '+'))
        {
            at++;
        }
        while (at < input.size())
        {
            const char ch = input[at];
            const bool part = (ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || ch == '-' || ch == '+';
            if (!part)
            {
                break;
            }
            at++;
        }
        if (at == start)
        {
            return Fail("expected a value");
        }

        out.kind = JsonValue::Kind::Number;
        out.number = std::strtod(input.substr(start, at - start).c_str(), nullptr);
        return true;
    }

    bool ParseArray(JsonValue &out)
    {
        out.kind = JsonValue::Kind::Array;
        at++;
        depth++;
        SkipSpace();
        if (at < input.size() && input[at] == ']')
        {
            at++;
            depth--;
            return true;
        }

        for (;;)
        {
            JsonValue item;
            if (!ParseValue(item))
            {
                return false;
            }
            out.items.push_back(std::move(item));

            SkipSpace();
            if (at >= input.size())
            {
                return Fail("unterminated array");
            }
            if (input[at] == ',')
            {
                at++;
                continue;
            }
            if (input[at] == ']')
            {
                at++;
                depth--;
                return true;
            }
            return Fail("expected , or ]");
        }
    }

    bool ParseObject(JsonValue &out)
    {
        out.kind = JsonValue::Kind::Object;
        at++;
        depth++;
        SkipSpace();
        if (at < input.size() && input[at] == '}')
        {
            at++;
            depth--;
            return true;
        }

        for (;;)
        {
            SkipSpace();
            std::string name;
            if (!ParseString(name))
            {
                return false;
            }

            SkipSpace();
            if (at >= input.size() || input[at] != ':')
            {
                return Fail("expected :");
            }
            at++;

            JsonValue value;
            if (!ParseValue(value))
            {
                return false;
            }
            out.members.emplace_back(std::move(name), std::move(value));

            SkipSpace();
            if (at >= input.size())
            {
                return Fail("unterminated object");
            }
            if (input[at] == ',')
            {
                at++;
                continue;
            }
            if (input[at] == '}')
            {
                at++;
                depth--;
                return true;
            }
            return Fail("expected , or }");
        }
    }
};
}

bool JsonParse(const std::string &input, JsonValue &out, std::string &error)
{
    Parser parser(input);
    out = JsonValue();
    if (!parser.ParseValue(out))
    {
        error = parser.error;
        return false;
    }

    parser.SkipSpace();
    if (parser.at != input.size())
    {
        error = "trailing data at " + std::to_string(parser.at);
        return false;
    }
    return true;
}
}
