#include "portal_cod4x/chat_messages.h"

namespace portal_cod4x
{
namespace
{
std::size_t UnitSize(std::string_view text, std::size_t offset)
{
    const auto c = static_cast<unsigned char>(text[offset]);
    if (c < 0x20 || c == 0x7f || c == '"')
    {
        return 0;
    }
    if (c < 0x80)
    {
        return c == '^' && offset + 1 < text.size() && text[offset + 1] >= '0' && text[offset + 1] <= '9' ? 2 : 1;
    }
    const std::size_t length = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!length || offset + length > text.size())
    {
        return 0;
    }
    for (std::size_t i = 1; i < length; ++i)
    {
        const auto next = static_cast<unsigned char>(text[offset + i]);
        if (next < 0x80 || next > 0xbf)
        {
            return 0;
        }
    }
    const auto next = static_cast<unsigned char>(text[offset + 1]);
    if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) || (c == 0xf0 && next < 0x90) ||
        (c == 0xf4 && next >= 0x90))
    {
        return 0;
    }
    return length;
}
} // namespace

bool IsValidChatText(std::string_view text)
{
    if (text.empty())
    {
        return false;
    }
    for (std::size_t i = 0; i < text.size();)
    {
        const auto length = UnitSize(text, i);
        if (!length)
        {
            return false;
        }
        i += length;
    }
    return true;
}

std::optional<std::vector<std::string>> PrepareChatMessages(const std::vector<std::string>& messages)
{
    if (messages.empty())
    {
        return std::nullopt;
    }
    for (const auto& text : messages)
    {
        if (!IsValidChatText(text))
        {
            return std::nullopt;
        }
    }
    std::vector<std::string> fragments;
    for (const auto& text : messages)
    {
        std::string colour;
        std::size_t start = 0;
        while (start < text.size())
        {
            auto end = start;
            auto wordEnd = start;
            while (end < text.size())
            {
                const auto length = UnitSize(text, end);
                if (colour.size() + end - start + length > kChatPayloadBytes)
                {
                    break;
                }
                end += length;
                if (text[end - 1] == ' ')
                {
                    wordEnd = end;
                }
            }
            if (end < text.size() && wordEnd > start)
            {
                end = wordEnd;
            }
            fragments.push_back(colour + text.substr(start, end - start));
            for (auto i = start; i < end;)
            {
                const auto length = UnitSize(text, i);
                if (length == 2 && text[i] == '^')
                {
                    colour = text.substr(i, 2);
                }
                i += length;
            }
            start = end;
        }
    }
    return fragments;
}
} // namespace portal_cod4x
