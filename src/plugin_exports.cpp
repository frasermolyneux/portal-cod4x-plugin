#include "cod4x_abi.h"

#include "portal_cod4x/plugin_runtime.h"
#include "portal_cod4x/plugin_version.h"

#include <cstring>
#include <ctime>
#include <algorithm>
#include <array>
#include <optional>
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

std::string StripPortFromAddress(std::string value)
{
    const std::size_t bracketPos = value.rfind("]:");
    if (!value.empty() && value.front() == '[' && bracketPos != std::string::npos)
    {
        return value.substr(1, bracketPos - 1);
    }

    const std::size_t colonPos = value.rfind(':');
    if (colonPos != std::string::npos && value.find('.') != std::string::npos)
    {
        return value.substr(0, colonPos);
    }

    return value;
}

std::string NetAddressToIp(const netadr_t* address)
{
    if (address == nullptr)
    {
        return "0.0.0.0";
    }

    if (address->type == NA_IP)
    {
        return std::to_string(address->address.ip[0]) + "." + std::to_string(address->address.ip[1]) + "." +
            std::to_string(address->address.ip[2]) + "." + std::to_string(address->address.ip[3]);
    }

    std::array<char, 128> buffer{};
    const char* formatted = Plugin_NET_AdrToStringShortMT(const_cast<netadr_t*>(address), buffer.data(), static_cast<int>(buffer.size()));
    if (formatted == nullptr)
    {
        return "0.0.0.0";
    }

    return StripPortFromAddress(std::string(formatted));
}

int ClientPointerToSlot(client_t* client)
{
    if (client == nullptr)
    {
        return -1;
    }

    return static_cast<int>(Plugin_GetClientNumForClient(client));
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

    bool ExecuteServerCommand(std::string_view command) override
    {
        const std::string payload(command);
        Plugin_Cbuf_AddText(payload.c_str());
        Plugin_Cbuf_AddText("\n");
        return true;
    }

    portal_cod4x::HttpRequestHandle BeginHttpRequest(
        std::string_view url,
        std::string_view method,
        std::string_view body,
        std::string_view additionalHeaders) override
    {
        std::string payloadBody(body);
        std::string payloadHeaders(additionalHeaders);

        ftRequest_t* request = Plugin_HTTP_MakeHttpRequest(
            std::string(url).c_str(),
            std::string(method).c_str(),
            payloadBody.empty() ? nullptr : reinterpret_cast<byte*>(payloadBody.data()),
            static_cast<int>(payloadBody.size()),
            payloadHeaders.empty() ? nullptr : payloadHeaders.c_str());

        // Plugin_HTTP_MakeHttpRequest copies the payload into the request object before
        // returning, so the local body/header buffers do not need to outlive this call.
        return static_cast<portal_cod4x::HttpRequestHandle>(request);
    }

    portal_cod4x::HttpRequestStatus PollHttpRequest(
        portal_cod4x::HttpRequestHandle handle,
        portal_cod4x::HttpResponse& response) override
    {
        auto* request = static_cast<ftRequest_t*>(handle);
        if (request == nullptr)
        {
            return portal_cod4x::HttpRequestStatus::Failed;
        }

        const int transferResult = Plugin_HTTP_SendReceiveData(request);
        if (transferResult == 0)
        {
            return portal_cod4x::HttpRequestStatus::Pending;
        }

        if (transferResult < 0)
        {
            return portal_cod4x::HttpRequestStatus::Failed;
        }

        response = portal_cod4x::HttpResponse{};
        response.StatusCode = request->code;

        if (request->extrecvmsg != nullptr && request->extrecvmsg->data != nullptr)
        {
            const int fullLength = request->extrecvmsg->cursize;
            const int bodyOffset = std::max(0, request->headerLength);
            const int fallbackLength = std::max(0, fullLength - bodyOffset);
            const int declaredLength = request->contentLength > 0
                ? request->contentLength
                : std::max(0, request->contentLengthArrived);
            const int bodyLength = std::min(fallbackLength, declaredLength > 0 ? declaredLength : fallbackLength);

            if (bodyLength > 0)
            {
                response.Body.assign(
                    reinterpret_cast<const char*>(request->extrecvmsg->data + bodyOffset),
                    static_cast<std::size_t>(bodyLength));
            }
        }

        return portal_cod4x::HttpRequestStatus::Completed;
    }

    void EndHttpRequest(portal_cod4x::HttpRequestHandle handle) override
    {
        auto* request = static_cast<ftRequest_t*>(handle);
        if (request != nullptr)
        {
            Plugin_HTTP_FreeObj(request);
        }
    }

    std::uint64_t GetPlayerId(int slot) const override
    {
        if (slot < 0)
        {
            return 0;
        }

        return Plugin_GetPlayerID(static_cast<unsigned int>(slot));
    }

    std::string GetPlayerName(int slot) const override
    {
        if (slot < 0)
        {
            return {};
        }

        const char* value = Plugin_GetPlayerName(slot);
        return value == nullptr ? std::string() : std::string(value);
    }

    int GetSlotCount() const override
    {
        return Plugin_GetSlotCount();
    }

    int GetPlayerScore(int slot) const override
    {
        if (slot < 0)
        {
            return 0;
        }

        return Plugin_GetClientScoreboard(slot).score;
    }

    std::string GetCvarString(std::string_view cvarName) const override
    {
        std::array<char, 256> buffer{};
        Plugin_Cvar_VariableStringBuffer(std::string(cvarName).c_str(), buffer.data(), buffer.size());
        return std::string(buffer.data());
    }

    std::int64_t GetUnixTimeSeconds() const override
    {
        return static_cast<std::int64_t>(std::time(nullptr));
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

PCL void COD4X_CALL OnFrame()
{
    Cod4xHostAdapter host;
    portal_cod4x::TickPlugin(host);
}

PCL void COD4X_CALL OnMessageSent(char* message, int slot, qboolean* show, int mode)
{
    if (show != nullptr && *show == qfalse)
    {
        return;
    }

    if (message == nullptr)
    {
        return;
    }

    Cod4xHostAdapter host;
    portal_cod4x::NotifyChatMessage(host, slot, std::string_view(message), mode != 0);
}

PCL void COD4X_CALL OnSpawnServer()
{
    Cod4xHostAdapter host;
    portal_cod4x::NotifyServerSpawned(host);
}

PCL void COD4X_CALL OnExitLevel()
{
    Cod4xHostAdapter host;
    portal_cod4x::NotifyServerExited(host);
}

PCL void COD4X_CALL OnPlayerConnect(
    int clientnum,
    netadr_t* netaddress,
    char*,
    char*,
    int,
    char*,
    int)
{
    Cod4xHostAdapter host;
    portal_cod4x::NotifyPlayerConnect(host, clientnum, NetAddressToIp(netaddress));
}

PCL void COD4X_CALL OnClientEnterWorld(client_t* client)
{
    Cod4xHostAdapter host;
    portal_cod4x::NotifyPlayerConnected(host, ClientPointerToSlot(client));
}

PCL void COD4X_CALL OnPlayerDC(client_t* client, const char*)
{
    Cod4xHostAdapter host;
    portal_cod4x::NotifyPlayerDisconnected(host, ClientPointerToSlot(client));
}

PCL void COD4X_CALL OnClientAuthorized()
{
    Cod4xHostAdapter host;
    portal_cod4x::NotifyClientAuthorized(host);
    portal_cod4x::TickPlugin(host);
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
