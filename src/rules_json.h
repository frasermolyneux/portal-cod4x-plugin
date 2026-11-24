#pragma once

#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace portal_cod4x
{
// Strict, bounded JSON reader for executable cloud commands. Duplicate keys are invalid.
struct RulesJson
{
    enum class Kind
    {
        Null,
        String,
        Number,
        Boolean,
        Object,
        Array
    };
    Kind Type = Kind::Null;
    std::string Scalar;
    std::map<std::string, RulesJson> Fields;
    std::vector<RulesJson> Items;

    const RulesJson* Find(std::string_view key) const
    {
        const auto it = Fields.find(std::string(key));
        return Type == Kind::Object && it != Fields.end() ? &it->second : nullptr;
    }

    std::optional<std::string> String(std::string_view key) const
    {
        const auto* value = Find(key);
        return value && value->Type == Kind::String ? std::optional(value->Scalar) : std::nullopt;
    }

    std::optional<std::int64_t> Integer(std::string_view key) const
    {
        const auto* value = Find(key);
        if (!value || value->Type != Kind::Number)
        {
            return std::nullopt;
        }
        std::int64_t result = 0;
        const auto parsed = std::from_chars(value->Scalar.data(), value->Scalar.data() + value->Scalar.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != value->Scalar.data() + value->Scalar.size())
        {
            return std::nullopt;
        }
        return result;
    }

    std::optional<std::uint64_t> Unsigned(std::string_view key) const
    {
        const auto* value = Find(key);
        if (!value || value->Type != Kind::Number)
        {
            return std::nullopt;
        }
        std::uint64_t result = 0;
        const auto parsed = std::from_chars(value->Scalar.data(), value->Scalar.data() + value->Scalar.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != value->Scalar.data() + value->Scalar.size())
        {
            return std::nullopt;
        }
        return result;
    }

    static std::string Quote(std::string_view value)
    {
        std::string result = "\"";
        constexpr char hex[] = "0123456789abcdef";
        for (const unsigned char c : value)
        {
            if (c == '"' || c == '\\')
            {
                result += '\\';
                result += static_cast<char>(c);
            }
            else if (c < 0x20)
            {
                result += "\\u00";
                result += hex[c >> 4];
                result += hex[c & 15];
            }
            else
            {
                result += static_cast<char>(c);
            }
        }
        return result + '"';
    }

    std::string Dump() const
    {
        if (Type == Kind::String)
        {
            return Quote(Scalar);
        }
        if (Type == Kind::Null)
        {
            return "null";
        }
        if (Type == Kind::Boolean || Type == Kind::Number)
        {
            return Scalar;
        }
        std::string result = Type == Kind::Object ? "{" : "[";
        bool first = true;
        if (Type == Kind::Object)
        {
            for (const auto& [key, value] : Fields)
            {
                if (!first)
                {
                    result += ',';
                }
                first = false;
                result += Quote(key) + ":" + value.Dump();
            }
        }
        else
        {
            for (const auto& value : Items)
            {
                if (!first)
                {
                    result += ',';
                }
                first = false;
                result += value.Dump();
            }
        }
        return result + (Type == Kind::Object ? "}" : "]");
    }
};

class RulesJsonReader
{
  public:
    static std::optional<RulesJson> Parse(std::string_view text, std::size_t maxBytes = 200 * 1024)
    {
        if (text.size() > maxBytes)
        {
            return std::nullopt;
        }
        RulesJsonReader reader(text);
        auto value = reader.Value(0);
        reader.Whitespace();
        return value && reader.position == text.size() ? std::move(value) : std::nullopt;
    }

  private:
    explicit RulesJsonReader(std::string_view text) : input(text)
    {
    }
    std::string_view input;
    std::size_t position = 0;

    void Whitespace()
    {
        while (position < input.size() && std::string_view(" \t\r\n").find(input[position]) != std::string_view::npos)
        {
            ++position;
        }
    }

    bool Take(char c)
    {
        Whitespace();
        if (position == input.size() || input[position] != c)
        {
            return false;
        }
        ++position;
        return true;
    }

    std::optional<unsigned> Hex()
    {
        if (input.size() - position < 4)
        {
            return std::nullopt;
        }
        unsigned value = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = input[position++];
            unsigned digit;
            if (c >= '0' && c <= '9')
            {
                digit = c - '0';
            }
            else if (c >= 'a' && c <= 'f')
            {
                digit = c - 'a' + 10;
            }
            else if (c >= 'A' && c <= 'F')
            {
                digit = c - 'A' + 10;
            }
            else
            {
                return std::nullopt;
            }
            value = value * 16 + digit;
        }
        return value;
    }

    static void Utf8(std::string& value, unsigned code)
    {
        if (code <= 0x7f)
        {
            value += static_cast<char>(code);
        }
        else
        {
            if (code > 0xffff)
            {
                value += static_cast<char>(0xf0 | (code >> 18));
            }
            else if (code > 0x7ff)
            {
                value += static_cast<char>(0xe0 | (code >> 12));
            }
            else
            {
                value += static_cast<char>(0xc0 | (code >> 6));
            }
            if (code > 0xffff)
            {
                value += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
            }
            if (code > 0x7ff)
            {
                value += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            }
            value += static_cast<char>(0x80 | (code & 0x3f));
        }
    }

    std::optional<std::string> String()
    {
        if (!Take('"'))
        {
            return std::nullopt;
        }
        std::string result;
        while (position < input.size())
        {
            const unsigned char c = input[position++];
            if (c == '"')
            {
                return result;
            }
            if (c < 0x20)
            {
                return std::nullopt;
            }
            if (c != '\\')
            {
                result += static_cast<char>(c);
                if (c >= 0x80)
                {
                    const unsigned length = c >= 0xc2 && c <= 0xdf   ? 2
                                            : c >= 0xe0 && c <= 0xef ? 3
                                            : c >= 0xf0 && c <= 0xf4 ? 4
                                                                     : 0;
                    if (length == 0 || input.size() - position < length - 1)
                    {
                        return std::nullopt;
                    }
                    const unsigned char next = input[position];
                    if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) || (c == 0xf0 && next < 0x90) ||
                        (c == 0xf4 && next > 0x8f))
                    {
                        return std::nullopt;
                    }
                    for (unsigned i = 1; i < length; ++i)
                    {
                        const unsigned char continuation = input[position++];
                        if ((continuation & 0xc0) != 0x80)
                        {
                            return std::nullopt;
                        }
                        result += static_cast<char>(continuation);
                    }
                }
                continue;
            }
            if (position == input.size())
            {
                return std::nullopt;
            }
            switch (input[position++])
            {
            case '"':
                result += '"';
                break;
            case '\\':
                result += '\\';
                break;
            case '/':
                result += '/';
                break;
            case 'b':
                result += '\b';
                break;
            case 'f':
                result += '\f';
                break;
            case 'n':
                result += '\n';
                break;
            case 'r':
                result += '\r';
                break;
            case 't':
                result += '\t';
                break;
            case 'u': {
                auto code = Hex();
                if (!code)
                {
                    return std::nullopt;
                }
                if (*code >= 0xd800 && *code <= 0xdbff)
                {
                    if (input.substr(position, 2) != "\\u")
                    {
                        return std::nullopt;
                    }
                    position += 2;
                    const auto low = Hex();
                    if (!low || *low < 0xdc00 || *low > 0xdfff)
                    {
                        return std::nullopt;
                    }
                    *code = 0x10000 + ((*code - 0xd800) << 10) + *low - 0xdc00;
                }
                else if (*code >= 0xdc00 && *code <= 0xdfff)
                {
                    return std::nullopt;
                }
                Utf8(result, *code);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<RulesJson> Value(unsigned depth)
    {
        if (depth > 16)
        {
            return std::nullopt;
        }
        Whitespace();
        if (position == input.size())
        {
            return std::nullopt;
        }
        RulesJson result;
        if (input[position] == '{')
        {
            result.Type = RulesJson::Kind::Object;
            ++position;
            if (Take('}'))
            {
                return result;
            }
            do
            {
                auto key = String();
                if (!key || !Take(':'))
                {
                    return std::nullopt;
                }
                auto child = Value(depth + 1);
                if (!child || !result.Fields.emplace(std::move(*key), std::move(*child)).second)
                {
                    return std::nullopt;
                }
                if (Take('}'))
                {
                    return result;
                }
            } while (Take(','));
            return std::nullopt;
        }
        if (input[position] == '[')
        {
            result.Type = RulesJson::Kind::Array;
            ++position;
            if (Take(']'))
            {
                return result;
            }
            do
            {
                auto child = Value(depth + 1);
                if (!child)
                {
                    return std::nullopt;
                }
                result.Items.push_back(std::move(*child));
                if (Take(']'))
                {
                    return result;
                }
            } while (Take(','));
            return std::nullopt;
        }
        if (input[position] == '"')
        {
            auto text = String();
            if (!text)
            {
                return std::nullopt;
            }
            result.Type = RulesJson::Kind::String;
            result.Scalar = std::move(*text);
            return result;
        }
        for (const std::string_view literal : {"true", "false", "null"})
        {
            if (input.substr(position, literal.size()) == literal)
            {
                position += literal.size();
                result.Type = literal == "null" ? RulesJson::Kind::Null : RulesJson::Kind::Boolean;
                result.Scalar = literal;
                return result;
            }
        }
        const auto start = position;
        if (input[position] == '-')
        {
            ++position;
        }
        if (position == input.size())
        {
            return std::nullopt;
        }
        if (input[position] == '0')
        {
            ++position;
        }
        else
        {
            if (input[position] < '1' || input[position] > '9')
            {
                return std::nullopt;
            }
            while (position < input.size() && input[position] >= '0' && input[position] <= '9')
            {
                ++position;
            }
        }
        if (position < input.size() && input[position] == '.')
        {
            ++position;
            const auto digits = position;
            while (position < input.size() && input[position] >= '0' && input[position] <= '9')
            {
                ++position;
            }
            if (digits == position)
            {
                return std::nullopt;
            }
        }
        if (position < input.size() && (input[position] == 'e' || input[position] == 'E'))
        {
            ++position;
            if (position < input.size() && (input[position] == '+' || input[position] == '-'))
            {
                ++position;
            }
            const auto digits = position;
            while (position < input.size() && input[position] >= '0' && input[position] <= '9')
            {
                ++position;
            }
            if (digits == position)
            {
                return std::nullopt;
            }
        }
        result.Type = RulesJson::Kind::Number;
        result.Scalar = std::string(input.substr(start, position - start));
        return result;
    }
};
} // namespace portal_cod4x
