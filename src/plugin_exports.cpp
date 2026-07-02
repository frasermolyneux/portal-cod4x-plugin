#include "cod4x_abi.h"

#include "portal_cod4x/plugin_runtime.h"
#include "portal_cod4x/plugin_version.h"

#include <cstring>
#include <string>
#include <string_view>

namespace
{
constexpr int kBroadcastSlot = -1;

std::string BuildShortDescription()
{
    std::string description = "Portal CoD4x plugin skeleton (v";
    description += portal_cod4x::kPluginSemanticVersion;
    description += ')';
    return description;
}

void CopyToBuffer(char* destination, std::size_t destinationLength, std::string_view value)
{
    if (destination == nullptr || destinationLength == 0)
    {
        return;
    }

    const std::size_t copyLength = (value.size() < destinationLength - 1) ? value.size() : destinationLength - 1;
    std::memset(destination, 0, destinationLength);
    std::memcpy(destination, value.data(), copyLength);
}

class Cod4xHostAdapter final : public portal_cod4x::ICod4xHost
{
public:
    void BroadcastChat(std::string_view message) override
    {
        const std::string payload(message);
        Plugin_ChatPrintf(kBroadcastSlot, "%s", payload.c_str());
    }

    void Log(std::string_view message) override
    {
        const std::string payload(message);
        Plugin_Printf("%s\n", payload.c_str());
    }
};
}

PCL int COD4X_CALL OnInit()
{
    Cod4xHostAdapter host;

    return portal_cod4x::InitializePlugin(
        host,
        portal_cod4x::kPluginSemanticVersion,
        portal_cod4x::kDefaultBotPrefix);
}

PCL void COD4X_CALL OnTerminate()
{
    Plugin_Printf("portal-cod4x-plugin terminated\n");
}

PCL void COD4X_CALL OnInfoRequest(pluginInfo_t* info)
{
    if (info == nullptr)
    {
        return;
    }

    std::memset(info, 0, sizeof(*info));

    info->handlerVersion.major = PLUGIN_HANDLER_VERSION_MAJOR;
    info->handlerVersion.minor = PLUGIN_HANDLER_VERSION_MINOR;
    info->pluginVersion.major = portal_cod4x::kPluginVersionMajor;
    info->pluginVersion.minor = portal_cod4x::kPluginVersionMinor;

    CopyToBuffer(info->fullName, sizeof(info->fullName), "portal-cod4x-plugin");
    const std::string shortDescription = BuildShortDescription();
    CopyToBuffer(info->shortDescription, sizeof(info->shortDescription), shortDescription);
    CopyToBuffer(
        info->longDescription,
        sizeof(info->longDescription),
        "Initial portal-cod4x-plugin skeleton. Broadcasts an online message with plugin version on load.");
}
