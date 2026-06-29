#include "portal_cod4x/plugin_runtime.h"

#include <string>

namespace portal_cod4x
{
std::string BuildOnlineBroadcastMessage(std::string_view prefix, std::string_view version)
{
    std::string normalizedPrefix = prefix.empty() ? std::string(kDefaultBotPrefix) : std::string(prefix);
    std::string normalizedVersion = version.empty() ? "0.0.0-unknown" : std::string(version);

    return normalizedPrefix + " portal-cod4x-plugin is online (version " + normalizedVersion + ")";
}

int InitializePlugin(ICod4xHost& host, std::string_view version, std::string_view prefix)
{
    const std::string onlineMessage = BuildOnlineBroadcastMessage(prefix, version);
    const std::string normalizedVersion = version.empty() ? "0.0.0-unknown" : std::string(version);

    host.Log("portal-cod4x-plugin initialized (version " + normalizedVersion + ")");
    host.BroadcastChat(onlineMessage);

    return 0;
}
}
