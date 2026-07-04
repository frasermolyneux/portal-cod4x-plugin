#include "portal_cod4x/plugin_runtime.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
class FakeHost final : public portal_cod4x::ICod4xHost
{
public:
    struct PrivateChatMessage
    {
        int Slot;
        std::string Message;
    };

    struct RequestRecord
    {
        std::string Method;
        std::string Url;
        std::string Body;
        std::string Headers;
    };

    std::vector<std::string> BroadcastMessages;
    std::vector<PrivateChatMessage> PrivateMessages;
    std::vector<std::string> Logs;
    std::vector<std::string> ExecutedCommands;
    std::vector<RequestRecord> Requests;
    std::unordered_map<std::string, portal_cod4x::HttpResponse> Responses;
    std::unordered_map<int, std::uint64_t> PlayerIds;
    std::unordered_map<int, std::uint64_t> PlayerSteamIds;
    std::unordered_map<int, std::string> PlayerNames;
    std::unordered_map<int, int> PlayerScores;
    std::unordered_map<std::string, std::string> CvarValues;
    std::int64_t CurrentTime = 0;

    void BroadcastChat(std::string_view message) override
    {
        BroadcastMessages.emplace_back(message);
    }

    void SendChat(int slot, std::string_view message) override
    {
        PrivateMessages.push_back(PrivateChatMessage{slot, std::string(message)});
    }

    void Log(std::string_view message) override
    {
        Logs.emplace_back(message);
    }

    bool ExecuteServerCommand(std::string_view command) override
    {
        ExecutedCommands.emplace_back(command);
        return true;
    }

    portal_cod4x::HttpRequestHandle BeginHttpRequest(
        std::string_view url,
        std::string_view method,
        std::string_view body,
        std::string_view additionalHeaders) override
    {
        Requests.push_back(RequestRecord{std::string(method), std::string(url), std::string(body), std::string(additionalHeaders)});

        const std::string key = std::string(method) + " " + std::string(url);
        auto* pending = new PendingRequest{};

        const auto it = Responses.find(key);
        if (it != Responses.end())
        {
            pending->HasResponse = true;
            pending->Response = it->second;
        }

        return static_cast<portal_cod4x::HttpRequestHandle>(pending);
    }

    portal_cod4x::HttpRequestStatus PollHttpRequest(
        portal_cod4x::HttpRequestHandle handle,
        portal_cod4x::HttpResponse& response) override
    {
        auto* pending = static_cast<PendingRequest*>(handle);
        if (pending == nullptr || !pending->HasResponse)
        {
            return portal_cod4x::HttpRequestStatus::Failed;
        }

        response = pending->Response;
        return portal_cod4x::HttpRequestStatus::Completed;
    }

    void EndHttpRequest(portal_cod4x::HttpRequestHandle handle) override
    {
        delete static_cast<PendingRequest*>(handle);
    }

    std::uint64_t GetPlayerId(int slot) const override
    {
        const auto it = PlayerIds.find(slot);
        return it == PlayerIds.end() ? 0 : it->second;
    }

    std::uint64_t GetPlayerSteamId(int slot) const override
    {
        const auto it = PlayerSteamIds.find(slot);
        return it == PlayerSteamIds.end() ? 0 : it->second;
    }

    std::string GetPlayerName(int slot) const override
    {
        const auto it = PlayerNames.find(slot);
        return it == PlayerNames.end() ? std::string() : it->second;
    }

    int GetSlotCount() const override
    {
        return 64;
    }

    int GetPlayerScore(int slot) const override
    {
        const auto it = PlayerScores.find(slot);
        return it == PlayerScores.end() ? 0 : it->second;
    }

    std::string GetCvarString(std::string_view cvarName) const override
    {
        const auto it = CvarValues.find(std::string(cvarName));
        return it == CvarValues.end() ? std::string() : it->second;
    }

    std::int64_t GetUnixTimeSeconds() const override
    {
        return CurrentTime;
    }

private:
    struct PendingRequest
    {
        bool HasResponse = false;
        portal_cod4x::HttpResponse Response;
    };
};

void Assert(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "Assertion failed: " << message << std::endl;
        std::exit(1);
    }
}

void BuildMessage_UsesPrefixAndVersion()
{
    const std::string message = portal_cod4x::BuildOnlineBroadcastMessage("^4[^1XI-BOT^4]^7", "0.1.0");

    Assert(message.find("^4[^1XI-BOT^4]^7") != std::string::npos, "Expected colorized prefix in message");
    Assert(message.find("Portal Plugin is online") != std::string::npos, "Expected startup wording in message");
    Assert(message.find("version 0.1.0") != std::string::npos, "Expected semantic version in message");
}

void BuildMessage_FallsBackWhenPrefixOrVersionMissing()
{
    const std::string message = portal_cod4x::BuildOnlineBroadcastMessage("", "");

    Assert(message.find("XI-BOT") != std::string::npos, "Expected default XI-BOT prefix when prefix is empty");
    Assert(message.find("Portal Plugin is online") != std::string::npos, "Expected startup wording in fallback message");
    Assert(message.find("0.0.0-unknown") != std::string::npos, "Expected fallback version when version is empty");
}

std::string EscapeJsonString(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size() * 2);

    for (const char c : value)
    {
        if (c == '\\' || c == '"')
        {
            escaped.push_back('\\');
        }

        escaped.push_back(c);
    }

    return escaped;
}

void Runtime_RefreshesSettingsAndReconcilesCommandPower()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.runtime.test.json";

    {
        std::ofstream configFile(configPath);
        configFile
            << "{"
            << "\"tenantId\":\"tenant-test\","
            << "\"clientId\":\"client-test\","
            << "\"clientSecret\":\"secret-test\","
            << "\"repositoryApiBaseUrl\":\"https://example.test/repository\","
            << "\"repositoryApiResource\":\"api://repository-test\","
            << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\","
            << "\"refreshIntervalSeconds\":120"
            << "}";
    }

    FakeHost host;
    host.CurrentTime = 1000;

    const std::string globalPayload =
        "{\"schemaVersion\":1,\"enabled\":true,\"commands\":{\"kick\":{\"minPower\":35},\"cmdpowerlist\":{\"minPower\":95}}}";
    const std::string serverPayload =
        "{\"schemaVersion\":1,\"commands\":{\"kick\":{\"minPower\":70},\"cmdpowerlist\":{\"minPower\":65}}}";

    host.Responses["POST https://login.microsoftonline.com/tenant-test/oauth2/v2.0/token"] = {
        200,
        "{\"access_token\":\"token-1\",\"expires_in\":3600}"};
    host.Responses["GET https://example.test/repository/v1.0/configurations/cod4xCommands"] = {
        200,
        "{\"namespace\":\"cod4xCommands\",\"configuration\":\"" + EscapeJsonString(globalPayload) + "\"}"};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/configurations/cod4xCommands"] = {
        200,
        "{\"namespace\":\"cod4xCommands\",\"configuration\":\"" + EscapeJsonString(serverPayload) + "\"}"};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/connected-players/admin-roster"] = {
        200,
        "{\"data\":{\"enabled\":false,\"defaultPower\":1,\"entries\":[]}}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    // The refresh pipeline is non-blocking and advances a single step per frame
    // (token -> global config -> server config -> reconcile), so pump several
    // frames to drive a refresh to completion. Extra frames past completion are
    // no-ops until the next refresh interval elapses.
    const auto pump = [&runtime, &host]() {
        for (int i = 0; i < 6; ++i)
        {
            runtime.Tick(host);
        }
    };

    pump();

    Assert(!host.ExecutedCommands.empty(), "Initial refresh should reconcile command powers");

    bool hasKick70 = false;
    bool hasAdminListCommands65 = false;
    for (const auto& command : host.ExecutedCommands)
    {
        if (command == "setCmdMinPower kick 70")
        {
            hasKick70 = true;
        }

        if (command == "setCmdMinPower AdminListCommands 65")
        {
            hasAdminListCommands65 = true;
        }
    }

    Assert(hasKick70, "Server override should set kick command power to 70");
    Assert(hasAdminListCommands65, "Alias command should resolve to AdminListCommands");

    const std::size_t firstApplyCount = host.ExecutedCommands.size();

    host.CurrentTime = 1100;
    pump();
    Assert(host.ExecutedCommands.size() == firstApplyCount, "Refresh before interval should not perform another reconciliation");

    const std::string changedServerPayload =
        "{\"schemaVersion\":1,\"commands\":{\"kick\":{\"minPower\":80},\"cmdpowerlist\":{\"minPower\":65}}}";
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/configurations/cod4xCommands"] = {
        200,
        "{\"namespace\":\"cod4xCommands\",\"configuration\":\"" + EscapeJsonString(changedServerPayload) + "\"}"};

    host.CurrentTime = 1121;
    pump();

    bool hasKick80 = false;
    for (const auto& command : host.ExecutedCommands)
    {
        if (command == "setCmdMinPower kick 80")
        {
            hasKick80 = true;
            break;
        }
    }

    Assert(hasKick80, "Changed server context should trigger updated command power reconciliation");
    const std::size_t secondApplyCount = host.ExecutedCommands.size();

    const std::string disabledServerPayload = "{\"schemaVersion\":1,\"enabled\":false,\"commands\":{\"kick\":{\"minPower\":55}}}";
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/configurations/cod4xCommands"] = {
        200,
        "{\"namespace\":\"cod4xCommands\",\"configuration\":\"" + EscapeJsonString(disabledServerPayload) + "\"}"};

    host.CurrentTime = 1245;
    pump();
    Assert(
        host.ExecutedCommands.size() == secondApplyCount,
        "When cod4xCommands enforcement is disabled, runtime should stop enforcing and keep existing command powers");

    const auto& context = runtime.GetServerContext();
    Assert(!context.GameServerId.empty(), "Server context should retain configured gameServerId");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_AdminRosterEnabled_LogsUnsupportedElevatedPowerOncePerPlayer()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.admin-roster.test.json";

    {
        std::ofstream configFile(configPath);
        configFile
            << "{"
            << "\"tenantId\":\"tenant-test\"," 
            << "\"clientId\":\"client-test\"," 
            << "\"clientSecret\":\"secret-test\"," 
            << "\"repositoryApiBaseUrl\":\"https://example.test/repository\"," 
            << "\"repositoryApiResource\":\"api://repository-test\"," 
            << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\"," 
            << "\"refreshIntervalSeconds\":120"
            << "}";
    }

    FakeHost host;
    host.CurrentTime = 5000;
    host.PlayerIds[2] = 76561198000000001ULL;
    host.PlayerSteamIds[2] = 76561198000000001ULL;
    host.PlayerNames[2] = "AdminPlayer";

    host.Responses["POST https://login.microsoftonline.com/tenant-test/oauth2/v2.0/token"] = {
        200,
        "{\"access_token\":\"token-1\",\"expires_in\":3600}"};
    host.Responses["GET https://example.test/repository/v1.0/configurations/cod4xCommands"] = {404, ""};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/configurations/cod4xCommands"] = {404, ""};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/connected-players/admin-roster"] = {
        200,
        "{\"data\":{\"enabled\":true,\"defaultPower\":10,\"entries\":[{\"playerGuid\":\"76561198000000001\",\"power\":70,\"tags\":[\"GameAdmin\"]}]}}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandlePlayerConnect(host, 2, "127.0.0.1");
    runtime.HandleClientAuthorized(host);

    const auto pump = [&runtime, &host]() {
        for (int i = 0; i < 6; ++i)
        {
            runtime.Tick(host);
        }
    };

    pump();

    int unsupportedWarningCount = 0;
    int reconciliationSummaryCount = 0;
    for (const auto& logMessage : host.Logs)
    {
        if (logMessage.find("cod4xPower desired level 70 for player 76561198000000001 cannot be applied") != std::string::npos)
        {
            unsupportedWarningCount++;
        }

        if (logMessage.find("cod4xPower roster reconciliation evaluated") != std::string::npos)
        {
            reconciliationSummaryCount++;
        }
    }

    Assert(unsupportedWarningCount == 1, "Expected elevated admin-power warning to log once for the player");
    Assert(reconciliationSummaryCount >= 1, "Expected admin roster reconciliation to run");

    host.CurrentTime = 5121;
    pump();

    int warningCountAfterSecondRefresh = 0;
    for (const auto& logMessage : host.Logs)
    {
        if (logMessage.find("cod4xPower desired level 70 for player 76561198000000001 cannot be applied") != std::string::npos)
        {
            warningCountAfterSecondRefresh++;
        }
    }

    Assert(warningCountAfterSecondRefresh == 1, "Expected warning dedupe for unchanged admin roster");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_EmitsAndFlushesPlayerConnectedEvent()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.ingest.test.json";

    {
        std::ofstream configFile(configPath);
        configFile
            << "{"
            << "\"tenantId\":\"tenant-test\"," 
            << "\"clientId\":\"client-test\"," 
            << "\"clientSecret\":\"secret-test\"," 
            << "\"repositoryApiBaseUrl\":\"https://example.test/repository\"," 
            << "\"repositoryApiResource\":\"api://repository-test\"," 
            << "\"ingestBaseUrl\":\"https://example.test/ingest\"," 
            << "\"ingestApiResource\":\"api://server-events-ingest\"," 
            << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\"," 
            << "\"gameType\":\"CallOfDuty4\"," 
            << "\"refreshIntervalSeconds\":120"
            << "}";
    }

    FakeHost host;
    host.CurrentTime = 2000;
    host.PlayerIds[2] = 76561198000000001ULL;
    host.PlayerSteamIds[2] = 76561198000000001ULL;
    host.PlayerNames[2] = "PlayerOne";

    host.Responses["POST https://login.microsoftonline.com/tenant-test/oauth2/v2.0/token"] = {
        200,
        "{\"access_token\":\"token-1\",\"expires_in\":3600}"};
    host.Responses["GET https://example.test/repository/v1.0/configurations/cod4xCommands"] = {404, ""};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/configurations/cod4xCommands"] = {404, ""};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/connected-players/admin-roster"] = {404, ""};
    host.Responses["POST https://example.test/ingest/events/player-connected"] = {202, ""};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandlePlayerConnect(host, 2, "192.168.0.10");
    runtime.HandlePlayerConnected(host, 2);

    for (int i = 0; i < 8; ++i)
    {
        runtime.Tick(host);
    }

    bool foundIngestPost = false;
    for (const auto& request : host.Requests)
    {
        if (request.Method == "POST" && request.Url == "https://example.test/ingest/events/player-connected")
        {
            foundIngestPost = true;
            Assert(request.Body.find("\"playerGuid\":\"76561198000000001\"") != std::string::npos, "Expected playerGuid in ingest payload");
            Assert(request.Body.find("\"steamId\":\"76561198000000001\"") != std::string::npos, "Expected steamId in ingest payload");
            Assert(request.Body.find("\"ipAddress\":\"192.168.0.10\"") != std::string::npos, "Expected ipAddress in ingest payload");
            Assert(request.Body.find("\"gameType\":\"CallOfDuty4\"") != std::string::npos, "Expected gameType in ingest payload");
            break;
        }
    }

    Assert(foundIngestPost, "Expected a player-connected ingest POST request");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_AuthorizedIdentity_AllowsDisconnectEventWhenPlayerIdUnavailableAtDisconnect()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.authorized.test.json";

    {
        std::ofstream configFile(configPath);
        configFile
            << "{"
            << "\"tenantId\":\"tenant-test\","
            << "\"clientId\":\"client-test\","
            << "\"clientSecret\":\"secret-test\","
            << "\"repositoryApiBaseUrl\":\"https://example.test/repository\","
            << "\"repositoryApiResource\":\"api://repository-test\","
            << "\"ingestBaseUrl\":\"https://example.test/ingest\","
            << "\"ingestApiResource\":\"api://server-events-ingest\","
            << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\","
            << "\"gameType\":\"CallOfDuty4\","
            << "\"refreshIntervalSeconds\":120"
            << "}";
    }

    FakeHost host;
    host.CurrentTime = 2500;
    host.PlayerIds[2] = 76561198000000001ULL;
    host.PlayerNames[2] = "PlayerOne";

    host.Responses["POST https://login.microsoftonline.com/tenant-test/oauth2/v2.0/token"] = {
        200,
        "{\"access_token\":\"token-1\",\"expires_in\":3600}"};
    host.Responses["GET https://example.test/repository/v1.0/configurations/cod4xCommands"] = {404, ""};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/configurations/cod4xCommands"] = {404, ""};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/connected-players/admin-roster"] = {404, ""};
    host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandlePlayerConnect(host, 2, "192.168.0.10");
    runtime.HandleClientAuthorized(host);

    host.PlayerIds.erase(2);
    runtime.HandlePlayerDisconnected(host, 2);

    for (int i = 0; i < 8; ++i)
    {
        runtime.Tick(host);
    }

    bool foundDisconnectPost = false;
    for (const auto& request : host.Requests)
    {
        if (request.Method == "POST" && request.Url == "https://example.test/ingest/events/player-disconnected")
        {
            foundDisconnectPost = true;
            Assert(request.Body.find("\"playerGuid\":\"76561198000000001\"") != std::string::npos, "Expected cached playerGuid in disconnect payload");
            Assert(request.Body.find("\"username\":\"PlayerOne\"") != std::string::npos, "Expected cached username in disconnect payload");
            break;
        }
    }

    Assert(foundDisconnectPost, "Expected a player-disconnected ingest POST request");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_DropsPoisonEventsAndUnblocksOtherQueues()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.poison.test.json";

    {
        std::ofstream configFile(configPath);
        configFile
            << "{"
            << "\"tenantId\":\"tenant-test\","
            << "\"clientId\":\"client-test\","
            << "\"clientSecret\":\"secret-test\","
            << "\"repositoryApiBaseUrl\":\"https://example.test/repository\","
            << "\"repositoryApiResource\":\"api://repository-test\","
            << "\"ingestBaseUrl\":\"https://example.test/ingest\","
            << "\"ingestApiResource\":\"api://server-events-ingest\","
            << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\","
            << "\"gameType\":\"CallOfDuty4\","
            << "\"refreshIntervalSeconds\":120"
            << "}";
    }

    FakeHost host;
    host.CurrentTime = 3000;
    host.PlayerIds[2] = 76561198000000001ULL;
    host.PlayerNames[2] = "PlayerOne";

    host.Responses["POST https://login.microsoftonline.com/tenant-test/oauth2/v2.0/token"] = {
        200,
        "{\"access_token\":\"token-1\",\"expires_in\":3600}"};
    host.Responses["GET https://example.test/repository/v1.0/configurations/cod4xCommands"] = {404, ""};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/configurations/cod4xCommands"] = {404, ""};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/connected-players/admin-roster"] = {404, ""};
    host.Responses["POST https://example.test/ingest/events/player-connected"] = {500, ""};
    host.Responses["POST https://example.test/ingest/events/chat-message"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandlePlayerConnect(host, 2, "192.168.0.10");
    runtime.HandlePlayerConnected(host, 2);
    runtime.HandleChatMessage(host, 2, "hello", false);

    for (int i = 0; i < 20; ++i)
    {
        for (int frame = 0; frame < 6; ++frame)
        {
            runtime.Tick(host);
        }

        host.CurrentTime += 65;
    }

    bool attemptedPoisonQueue = false;
    bool deliveredOtherQueue = false;
    for (const auto& request : host.Requests)
    {
        if (request.Method == "POST" && request.Url == "https://example.test/ingest/events/player-connected")
        {
            attemptedPoisonQueue = true;
        }

        if (request.Method == "POST" && request.Url == "https://example.test/ingest/events/chat-message")
        {
            deliveredOtherQueue = true;
        }
    }

    Assert(attemptedPoisonQueue, "Expected attempts against poison queue");
    Assert(deliveredOtherQueue, "Expected non-poison queue to be delivered after poison events are dropped");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_HandleClientCommand_SendsPrivateResponse()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;

    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandleClientCommand(host, 4, "!commands");

    Assert(host.PrivateMessages.size() == 1, "Expected one private command response");
    Assert(host.PrivateMessages.front().Slot == 4, "Private command response should target requesting slot");
    Assert(
        host.PrivateMessages.front().Message.find("Available commands") != std::string::npos,
        "Expected !commands response payload");
}

void Runtime_HandleChatMessage_AlsoExecutesCommandPath()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;

    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandleChatMessage(host, 5, "!whoami", false);

    Assert(host.PrivateMessages.size() == 1, "Expected command path to execute from chat message");
    Assert(host.PrivateMessages.front().Slot == 5, "Command response should target original chat slot");
    Assert(
        host.PrivateMessages.front().Message.find("Plugin command processing active") != std::string::npos,
        "Expected !whoami command response payload");
}

void Runtime_HandleClientCommand_DoesNotPrefixMatchLongerToken()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;

    host.CurrentTime = 42;
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandleClientCommand(host, 2, "!commandsfoo");

    Assert(host.PrivateMessages.empty(), "Unexpected prefix overmatch for non-command token");
}

void Runtime_HandleClientCommand_DedupesCrossCallbackPath()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;

    host.CurrentTime = 99;
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandleChatMessage(host, 3, "!commands", false);
    host.CurrentTime = 100;
    runtime.HandleClientCommand(host, 3, "!commands");

    Assert(host.PrivateMessages.size() == 1, "Cross-callback duplicate command response should be suppressed");

    host.CurrentTime = 101;
    runtime.HandleClientCommand(host, 3, "!commands");

    Assert(host.PrivateMessages.size() == 2, "Later command should be handled outside cross-callback dedupe window");
}

void InitializePlugin_EmitsLogAndBroadcast()
{
    FakeHost host;

    const int result = portal_cod4x::InitializePlugin(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    const std::string expectedBroadcast = "^4[^1XI-BOT^4]^7 Portal Plugin is online (version 1.2.3)";
    const std::string expectedLog = "Portal Plugin is online (version 1.2.3)";

    Assert(result == 0, "InitializePlugin should return success code");
    Assert(!host.Logs.empty(), "InitializePlugin should write startup logs");
    Assert(host.BroadcastMessages.size() == 1, "InitializePlugin should send one startup broadcast");
    Assert(host.BroadcastMessages.front() == expectedBroadcast, "Broadcast should match expected startup format");
    Assert(host.Logs.front() == expectedLog, "First startup log should match expected startup format");
}

void InitializePlugin_FallsBackWhenPrefixOrVersionMissing()
{
    FakeHost host;

    const int result = portal_cod4x::InitializePlugin(host, "", "");
    const std::string expectedBroadcast = std::string(portal_cod4x::kDefaultBotPrefix) + " Portal Plugin is online (version 0.0.0-unknown)";
    const std::string expectedLog = "Portal Plugin is online (version 0.0.0-unknown)";

    Assert(result == 0, "InitializePlugin should return success code for fallback path");
    Assert(!host.Logs.empty(), "InitializePlugin should write startup logs for fallback path");
    Assert(host.BroadcastMessages.size() == 1, "InitializePlugin should send one startup broadcast for fallback path");
    Assert(host.BroadcastMessages.front() == expectedBroadcast, "Fallback broadcast should match expected startup format");
    Assert(host.Logs.front() == expectedLog, "First fallback startup log should match expected startup format");
}
}

int main()
{
    BuildMessage_UsesPrefixAndVersion();
    BuildMessage_FallsBackWhenPrefixOrVersionMissing();
    Runtime_RefreshesSettingsAndReconcilesCommandPower();
    Runtime_AdminRosterEnabled_LogsUnsupportedElevatedPowerOncePerPlayer();
    Runtime_EmitsAndFlushesPlayerConnectedEvent();
    Runtime_AuthorizedIdentity_AllowsDisconnectEventWhenPlayerIdUnavailableAtDisconnect();
    Runtime_DropsPoisonEventsAndUnblocksOtherQueues();
    Runtime_HandleClientCommand_SendsPrivateResponse();
    Runtime_HandleChatMessage_AlsoExecutesCommandPath();
    Runtime_HandleClientCommand_DoesNotPrefixMatchLongerToken();
    Runtime_HandleClientCommand_DedupesCrossCallbackPath();
    InitializePlugin_EmitsLogAndBroadcast();
    InitializePlugin_FallsBackWhenPrefixOrVersionMissing();

    std::cout << "All plugin runtime tests passed." << std::endl;
    return 0;
}
