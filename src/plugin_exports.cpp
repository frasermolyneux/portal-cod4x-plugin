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
#include <unordered_map>

namespace
{
constexpr int kBroadcastSlot = -1;
constexpr int kPortalPluginHealthDefaultPower = 98;
constexpr int kPortalPluginLogLevelDefaultPower = 98;
constexpr int kDumpBanListDefaultPower = 30;
constexpr int kDumpPortalBanListDefaultPower = 30;
constexpr std::size_t kAuthRejectMessageCapacity = 1024;
constexpr std::string_view kPortalPluginLogLevelUsage =
    "portalpluginloglevel <debug|info|error|1|2|3>";

void COD4X_CALL CmdPortalPluginHealth();
void COD4X_CALL CmdPortalPluginLogLevel();
void COD4X_CALL CmdDumpBanList();
void COD4X_CALL CmdDumpPortalBanList();

struct PluginCommandRegistration
{
    const char* Name;
    xcommand_t Handler;
    int DefaultPower;
};

constexpr PluginCommandRegistration kPluginCommandRegistrations[] = {
    {portal_cod4x::kPortalPluginHealthCommandName.data(), &CmdPortalPluginHealth, kPortalPluginHealthDefaultPower},
    {portal_cod4x::kPortalPluginLogLevelCommandName.data(), &CmdPortalPluginLogLevel, kPortalPluginLogLevelDefaultPower},
    // Replaces the simplebanlist-provided `dumpbanlist` so the agent's RCON reconcile can import
    // server-created bans. Emits only the plugin's server-originated pending bans (portal-origin
    // bans are already in the portal, so surfacing them would risk re-import of lifted bans).
    {"dumpbanlist", &CmdDumpBanList, kDumpBanListDefaultPower},
    {"dumpportalbanlist", &CmdDumpPortalBanList, kDumpPortalBanListDefaultPower},
};

std::unordered_map<const ftRequest_t*, std::int64_t> g_pendingIngestLogDeadlineByRequest;

void RegisterPluginCommands()
{
    for (const auto& command : kPluginCommandRegistrations)
    {
        Plugin_AddCommand(command.Name, command.Handler, command.DefaultPower);
    }
}

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

    void SendChat(int slot, std::string_view message) override
    {
        if (slot < 0)
        {
            return;
        }

        const std::string payload(message);
        Plugin_ChatPrintf(slot, "%s", payload.c_str());
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

    void DropPlayer(int slot, std::string_view reason) override
    {
        if (slot < 0)
        {
            return;
        }

        const std::string payload(reason);
        Plugin_DropClient(static_cast<unsigned int>(slot), payload.c_str());
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
            const bool debugLoggingEnabled =
                portal_cod4x::GetPluginLogLevelValue() <= static_cast<int>(portal_cod4x::PluginRuntime::LogLevel::Debug);
            const bool isIngestRequest = std::string_view(request->url).find("/ingest/") != std::string_view::npos;
            if (debugLoggingEnabled && isIngestRequest)
            {
                const std::int64_t nowUnixSeconds = static_cast<std::int64_t>(std::time(nullptr));
                auto [deadlineIt, inserted] = g_pendingIngestLogDeadlineByRequest.emplace(request, nowUnixSeconds + 5);
                if (inserted || nowUnixSeconds >= deadlineIt->second)
                {
                    std::string logMessage = "ingest transport pending url=";
                    logMessage += request->url;
                    logMessage += " socketReady=" + std::to_string(request->socketReady ? 1 : 0);
                    logMessage += " sendRemainingBytes=" + std::to_string(std::max(0, request->sendmsg.cursize));
                    logMessage += " totalReceivedBytes=" + std::to_string(std::max(0, request->totalreceivedbytes));
                    logMessage += " httpCode=" + std::to_string(request->code);
                    logMessage += " headerLength=" + std::to_string(std::max(0, request->headerLength));
                    logMessage += " contentLength=" + std::to_string(std::max(0, request->contentLength));
                    Plugin_Printf("%s\n", logMessage.c_str());
                    deadlineIt->second = nowUnixSeconds + 5;
                }
            }

            return portal_cod4x::HttpRequestStatus::Pending;
        }

        g_pendingIngestLogDeadlineByRequest.erase(request);

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
            g_pendingIngestLogDeadlineByRequest.erase(request);
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

    std::uint64_t GetPlayerSteamId(int slot) const override
    {
        if (slot < 0)
        {
            return 0;
        }

        return Plugin_GetPlayerSteamID(static_cast<unsigned int>(slot));
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

    bool CanPlayerUseCommand(int slot, std::string_view commandName) const override
    {
        if (slot < 0)
        {
            return true;
        }

        return Plugin_CanPlayerUseCommand(slot, std::string(commandName).c_str()) == qtrue;
    }

    std::int64_t GetUnixTimeSeconds() const override
    {
        return static_cast<std::int64_t>(std::time(nullptr));
    }
};

void PrintCommandOutput(std::string_view output)
{
    // Print line-by-line so the RCON response is assembled the same way the reference simplebanlist
    // plugin produced it, and to stay clear of the engine print buffer limit.
    std::size_t lineStart = 0;
    while (lineStart < output.size())
    {
        std::size_t lineEnd = output.find('\n', lineStart);
        if (lineEnd == std::string::npos)
        {
            lineEnd = output.size();
        }

        const std::string line(output.substr(lineStart, lineEnd - lineStart));
        Plugin_Printf("%s\n", line.c_str());
        lineStart = lineEnd + 1;
    }
}

void COD4X_CALL CmdDumpBanList()
{
    PrintCommandOutput(portal_cod4x::RenderServerBanListDump());
}

void COD4X_CALL CmdDumpPortalBanList()
{
    PrintCommandOutput(portal_cod4x::RenderPortalBanListDump());
}

void COD4X_CALL CmdPortalPluginHealth()
{
    Cod4xHostAdapter host;
    portal_cod4x::NotifyPortalPluginHealthCommand(host, Plugin_Cmd_GetInvokerSlot());
}

void COD4X_CALL CmdPortalPluginLogLevel()
{
    Cod4xHostAdapter host;
    const int invokerSlot = Plugin_Cmd_GetInvokerSlot();
    const int argc = Plugin_Cmd_Argc();

    const auto logCurrentLevel = [&]() {
        const std::string summary = "plugin log level is " +
            portal_cod4x::GetPluginLogLevelName() +
            " (" + std::to_string(portal_cod4x::GetPluginLogLevelValue()) + ")";
        host.Log(summary);
        if (invokerSlot >= 0)
        {
            host.SendChat(invokerSlot, summary);
        }
    };

    if (argc < 2)
    {
        logCurrentLevel();
        host.Log("usage: " + std::string(kPortalPluginLogLevelUsage));
        if (invokerSlot >= 0)
        {
            host.SendChat(invokerSlot, "Usage: " + std::string(kPortalPluginLogLevelUsage));
        }

        return;
    }

    if (argc > 2)
    {
        host.Log("too many arguments for portalpluginloglevel");
        host.Log("usage: " + std::string(kPortalPluginLogLevelUsage));
        if (invokerSlot >= 0)
        {
            host.SendChat(invokerSlot, "Too many arguments. Usage: " + std::string(kPortalPluginLogLevelUsage));
        }

        return;
    }

    const char* rawLevelToken = Plugin_Cmd_Argv(1);
    const std::string levelToken = rawLevelToken == nullptr ? std::string() : std::string(rawLevelToken);
    if (!portal_cod4x::TrySetPluginLogLevel(host, levelToken, true))
    {
        host.Log("invalid portalpluginloglevel value: " + levelToken);
        host.Log("usage: " + std::string(kPortalPluginLogLevelUsage));
        if (invokerSlot >= 0)
        {
            host.SendChat(invokerSlot, "Invalid log level. Usage: " + std::string(kPortalPluginLogLevelUsage));
        }

        return;
    }

    if (invokerSlot >= 0)
    {
        host.SendChat(
            invokerSlot,
            "plugin log level set to " +
                portal_cod4x::GetPluginLogLevelName() +
                " (" + std::to_string(portal_cod4x::GetPluginLogLevelValue()) + ")");
    }
}
}

PCL int COD4X_CALL OnInit()
{
    Cod4xHostAdapter host;
    const int initializeResult = portal_cod4x::InitializePlugin(
        host,
        portal_cod4x::kPluginSemanticVersion,
        portal_cod4x::kDefaultBotPrefix);

    if (initializeResult == 0)
    {
        RegisterPluginCommands();
    }

    return initializeResult;
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

PCL void COD4X_CALL OnPlayerGotAuthInfo(
    netadr_t*,
    std::uint64_t* playerId,
    std::uint64_t*,
    char* rejectMessage,
    qboolean*,
    client_t*)
{
    if (playerId == nullptr || rejectMessage == nullptr || rejectMessage[0] != '\0')
    {
        return;
    }

    std::string banMessage;
    if (!portal_cod4x::TryGetAuthenticatedPlayerBanMessage(*playerId, banMessage))
    {
        return;
    }

    CopyToBuffer(rejectMessage, kAuthRejectMessageCapacity, banMessage);
}

PCL void COD4X_CALL OnClientCommand(client_t* client, const char* command)
{
    if (command == nullptr)
    {
        return;
    }

    Cod4xHostAdapter host;
    portal_cod4x::NotifyClientCommand(host, ClientPointerToSlot(client), std::string_view(command));
}

PCL void COD4X_CALL OnPlayerGetBanStatus(baninfo_t* baninfo, char* message, int len)
{
    if (baninfo == nullptr || message == nullptr || len <= 0)
    {
        return;
    }

    if (message[0] != '\0')
    {
        return;
    }

    std::string banMessage;
    if (!portal_cod4x::TryGetPlayerBanMessage(baninfo->playerid, banMessage))
    {
        return;
    }

    CopyToBuffer(message, static_cast<std::size_t>(len), banMessage);
    CopyToBuffer(baninfo->message, sizeof(baninfo->message), banMessage);
}

PCL void COD4X_CALL OnPlayerAddBan(baninfo_t* baninfo)
{
    if (baninfo == nullptr)
    {
        return;
    }

    portal_cod4x::NotifyPlayerBanAdded(
        baninfo->playerid,
        std::string_view(baninfo->message),
        baninfo->adminsteamid,
        std::string_view(baninfo->playername),
        static_cast<std::int64_t>(baninfo->expire));
}

PCL void COD4X_CALL OnPlayerRemoveBan(baninfo_t* baninfo)
{
    if (baninfo == nullptr)
    {
        return;
    }

    portal_cod4x::NotifyPlayerBanRemoved(baninfo->playerid);
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
