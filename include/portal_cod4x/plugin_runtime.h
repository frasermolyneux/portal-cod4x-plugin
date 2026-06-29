#pragma once

#include <string>
#include <string_view>

namespace portal_cod4x
{
class ICod4xHost
{
public:
    virtual ~ICod4xHost() = default;

    virtual void BroadcastChat(std::string_view message) = 0;
    virtual void Log(std::string_view message) = 0;
};

inline constexpr std::string_view kDefaultBotPrefix = "^4[^1XI-BOT^4]^7";

std::string BuildOnlineBroadcastMessage(std::string_view prefix, std::string_view version);
int InitializePlugin(ICod4xHost& host, std::string_view version, std::string_view prefix = kDefaultBotPrefix);
}
