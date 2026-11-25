#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace portal_cod4x
{
inline constexpr std::size_t kChatPayloadBytes = 149;

bool IsValidChatText(std::string_view text);
std::optional<std::vector<std::string>> PrepareChatMessages(const std::vector<std::string>& messages);
} // namespace portal_cod4x
