#include "portal_cod4x/plugin_runtime.h"

#include <algorithm>
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
    std::unordered_map<std::string, bool> CommandPermissions;
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

    bool CanPlayerUseCommand(int, std::string_view commandName) const override
    {
        const auto it = CommandPermissions.find(std::string(commandName));
        return it == CommandPermissions.end() ? true : it->second;
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

void Runtime_LoadsConfigAndStoresServerContext()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.runtime.test.json";

    {
        std::ofstream configFile(configPath);
        configFile
            << "{"
            << "\"ingestBaseUrl\":\"https://example.test/ingest\","
            << "\"ingestSubscriptionKey\":\"sub-key-test\","
            << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\","
            << "\"refreshIntervalSeconds\":120"
            << "}";
    }

    FakeHost host;
    host.CurrentTime = 1000;

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.Tick(host);
    Assert(host.ExecutedCommands.empty(), "Plugin runtime should not apply cod4x command-power reconciliation");

    bool requestedCod4xCommandSettings = false;
    for (const auto& request : host.Requests)
    {
        if (request.Url.find("/configurations/cod4xCommands") != std::string::npos)
        {
            requestedCod4xCommandSettings = true;
            break;
        }
    }

    Assert(!requestedCod4xCommandSettings, "Plugin runtime should not poll cod4xCommands settings");

    const auto& context = runtime.GetServerContext();
    Assert(
        context.GameServerId == "11111111-2222-3333-4444-555555555555",
        "Server context should retain configured gameServerId");

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
            << "\"ingestBaseUrl\":\"https://example.test/ingest\","
            << "\"ingestSubscriptionKey\":\"sub-key-test\","
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
            << "\"ingestBaseUrl\":\"https://example.test/ingest\","
            << "\"ingestSubscriptionKey\":\"sub-key-test\","
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
            << "\"ingestBaseUrl\":\"https://example.test/ingest\","
            << "\"ingestSubscriptionKey\":\"sub-key-test\","
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

void Runtime_HandleClientCommand_IgnoresPortalOwnedCommands()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;

    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandleClientCommand(host, 4, "!commands");

    Assert(host.PrivateMessages.empty(), "Plugin should ignore portal-owned commands like !commands.");
    Assert(host.ExecutedCommands.empty(), "Plugin should not emit server commands for portal-owned command input.");
}

void Runtime_HandleChatMessage_DoesNotInterceptPortalOwnedCommands()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;

    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandleChatMessage(host, 5, "!whoami", false);

    Assert(host.PrivateMessages.empty(), "Plugin should not respond to portal-owned chat commands.");
    Assert(host.ExecutedCommands.empty(), "Plugin should not run server commands for portal-owned chat commands.");
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
    host.CommandPermissions[std::string(portal_cod4x::kPortalPluginHealthCommandName)] = true;

    host.CurrentTime = 99;
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandleChatMessage(host, 3, "!portalpluginhealth", false);
    host.CurrentTime = 100;
    runtime.HandleClientCommand(host, 3, "!portalpluginhealth");

    const std::size_t tellMessageCount = std::count_if(
        host.ExecutedCommands.begin(),
        host.ExecutedCommands.end(),
        [](const std::string& command) { return command.find("tell 3 ") != std::string::npos; });
    Assert(tellMessageCount == 1, "Cross-callback duplicate command response should be suppressed");

    host.CurrentTime = 101;
    runtime.HandleClientCommand(host, 3, "!portalpluginhealth");

    const std::size_t tellMessageCountAfterSecondInvoke = std::count_if(
        host.ExecutedCommands.begin(),
        host.ExecutedCommands.end(),
        [](const std::string& command) { return command.find("tell 3 ") != std::string::npos; });
    Assert(tellMessageCountAfterSecondInvoke == 2, "Later command should be handled outside cross-callback dedupe window");
}

void Runtime_HandleClientCommand_PortalPluginHealth_UsesConsoleAndTellFlow()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;
    host.CommandPermissions[std::string(portal_cod4x::kPortalPluginHealthCommandName)] = true;

    host.CurrentTime = 111;
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandleClientCommand(host, 7, "!portalpluginhealth");

    bool hasConsay = false;
    bool hasTell = false;
    for (const auto& command : host.ExecutedCommands)
    {
        if (command.rfind("consay ", 0) == 0)
        {
            hasConsay = true;
        }

        if (command.find("tell 7 ") != std::string::npos)
        {
            hasTell = true;
        }
    }

    Assert(hasConsay, "Expected !portalpluginhealth to emit consay console output for player invocations");
    Assert(hasTell, "Expected !portalpluginhealth to send tell guidance to the requesting player");

    bool hasHealthLogLine = false;
    for (const auto& line : host.Logs)
    {
        if (line.find("portalpluginhealth report") != std::string::npos)
        {
            hasHealthLogLine = true;
            break;
        }
    }

    Assert(hasHealthLogLine, "Expected !portalpluginhealth to log console health report lines");
}

void Runtime_HandleClientCommand_PortalPluginHealth_RespectsCommandAuthorization()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;
    host.CommandPermissions[std::string(portal_cod4x::kPortalPluginHealthCommandName)] = false;

    host.CurrentTime = 112;
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandleClientCommand(host, 8, "!portalpluginhealth");

    bool hasDenyMessage = false;
    for (const auto& message : host.PrivateMessages)
    {
        if (message.Slot == 8 && message.Message.find("not authorized") != std::string::npos)
        {
            hasDenyMessage = true;
            break;
        }
    }

    Assert(hasDenyMessage, "Expected unauthorized response when player lacks portalpluginhealth command power.");
    Assert(host.ExecutedCommands.empty(), "Unauthorized health command should not emit server commands.");
}

void Runtime_HandleClientCommand_PortalPluginHealth_RespectsPortalEnabledFlag()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.portalpluginhealth-disabled.test.json";

    {
        std::ofstream configFile(configPath);
        configFile
            << "{"
            << "\"ingestBaseUrl\":\"https://example.test/ingest\","
            << "\"ingestSubscriptionKey\":\"sub-key-test\","
            << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\","
            << "\"portalPluginHealthEnabled\":false"
            << "}";
    }

    FakeHost host;
    host.CommandPermissions[std::string(portal_cod4x::kPortalPluginHealthCommandName)] = true;
    host.CurrentTime = 113;

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandleClientCommand(host, 8, "!portalpluginhealth");

    bool hasDisabledMessage = false;
    for (const auto& message : host.PrivateMessages)
    {
        if (message.Slot == 8 && message.Message.find("currently disabled") != std::string::npos)
        {
            hasDisabledMessage = true;
            break;
        }
    }

    Assert(hasDisabledMessage, "Expected disabled response when portalpluginhealth is disabled in runtime config.");
    Assert(host.ExecutedCommands.empty(), "Disabled health command should not emit server commands.");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_HandleClientCommand_PortalPluginHealth_IgnoresMalformedPortalEnabledFlag()
{
    const std::filesystem::path configPath =
        std::filesystem::temp_directory_path() / "portal-cod4x-plugin.portalpluginhealth-malformed-enabled.test.json";

    {
        std::ofstream configFile(configPath);
        configFile
            << "{"
            << "\"ingestBaseUrl\":\"https://example.test/ingest\","
            << "\"ingestSubscriptionKey\":\"sub-key-test\","
            << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\","
            << "\"portalPluginHealthEnabled\":00"
            << "}";
    }

    FakeHost host;
    host.CommandPermissions[std::string(portal_cod4x::kPortalPluginHealthCommandName)] = true;
    host.CurrentTime = 114;

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandleClientCommand(host, 9, "!portalpluginhealth");

    bool hasDisabledMessage = false;
    for (const auto& message : host.PrivateMessages)
    {
        if (message.Slot == 9 && message.Message.find("currently disabled") != std::string::npos)
        {
            hasDisabledMessage = true;
            break;
        }
    }

    Assert(!hasDisabledMessage, "Malformed portalPluginHealthEnabled should not disable !portalpluginhealth.");
    Assert(!host.ExecutedCommands.empty(), "Malformed portalPluginHealthEnabled should preserve default command behavior.");

    bool hasHealthLogLine = false;
    for (const auto& line : host.Logs)
    {
        if (line.find("portalpluginhealthEnabled=true") != std::string::npos)
        {
            hasHealthLogLine = true;
            break;
        }
    }

    Assert(hasHealthLogLine, "Expected malformed flag to fall back to enabled=true in health report output.");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_LoadsActiveBanCacheAndAnswersBanQuery()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.bansync.test.json";

    {
        std::ofstream configFile(configPath);
        configFile
            << "{"
            << "\"ingestBaseUrl\":\"https://example.test/ingest\","
            << "\"ingestSubscriptionKey\":\"sub-key-test\","
            << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\","
            << "\"gameType\":\"CallOfDuty4x\","
            << "\"refreshIntervalSeconds\":120"
            << "}";
    }

    FakeHost host;
    host.CurrentTime = 4000;

    host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] = {
        200,
        "{\"data\":{\"items\":[{\"player\":{\"guid\":\"76561198000000001\"}}]}}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    for (int i = 0; i < 6; ++i)
    {
        runtime.Tick(host);
    }

    std::string banMessage;
    const bool isBanned = runtime.TryGetPlayerBanMessage(76561198000000001ULL, banMessage);

    Assert(isBanned, "Expected player to be banned after active-ban cache sync");
    Assert(
        banMessage.find("banned") != std::string::npos,
        "Expected returned ban message to contain ban wording");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_PlayerBanMutationHintsUpdateCacheImmediately()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;

    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    runtime.HandlePlayerBanAdded(76561198000000002ULL, "Manual server ban");

    std::string banMessage;
    bool isBanned = runtime.TryGetPlayerBanMessage(76561198000000002ULL, banMessage);

    Assert(isBanned, "Expected add-ban hint to populate local ban cache");
    Assert(
        banMessage.find("Manual server ban") != std::string::npos,
        "Expected local ban cache to preserve callback-provided reason");

    runtime.HandlePlayerBanRemoved(76561198000000002ULL);

    isBanned = runtime.TryGetPlayerBanMessage(76561198000000002ULL, banMessage);
    Assert(!isBanned, "Expected remove-ban hint to evict local ban cache entry");
}

void Runtime_LogLevelDefaultsAndParsing()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;

    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    host.Logs.clear();

    Assert(runtime.GetLogLevelValue() == 2, "Expected default plugin log level to be info (2)");
    Assert(runtime.GetLogLevelName() == "info", "Expected default plugin log level name to be info");

    const bool setDebugResult = runtime.TrySetLogLevel(host, "debug");
    Assert(setDebugResult, "Expected debug log level token to be accepted");
    Assert(runtime.GetLogLevelValue() == 1, "Expected debug token to map to level 1");
    Assert(runtime.GetLogLevelName() == "debug", "Expected debug token to map to debug name");
    Assert(!host.Logs.empty(), "Expected log-level change announcement when announce=true");
    Assert(
        host.Logs.back().find("plugin log level set to debug (1)") != std::string::npos,
        "Expected debug change announcement to include debug level");

    host.Logs.clear();
    const bool setErrorResult = runtime.TrySetLogLevel(host, 3);
    Assert(setErrorResult, "Expected numeric log level token to be accepted");
    Assert(runtime.GetLogLevelValue() == 3, "Expected numeric level 3 to map to error");
    Assert(runtime.GetLogLevelName() == "error", "Expected numeric level 3 to map to error name");
    Assert(
        !host.Logs.empty() && host.Logs.back().find("plugin log level set to error (3)") != std::string::npos,
        "Expected numeric level change announcement to include error level");

    host.Logs.clear();
    const bool invalidSetResult = runtime.TrySetLogLevel(host, "verbose");
    Assert(!invalidSetResult, "Expected unknown log level token to be rejected");
    Assert(runtime.GetLogLevelValue() == 3, "Rejected token should preserve previous log level");
    Assert(host.Logs.empty(), "Rejected token should not emit a change announcement");
}

void Runtime_LogLevelSetWithoutAnnouncement_DoesNotLog()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;

    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    host.Logs.clear();

    const bool setInfoWithoutAnnounce = runtime.TrySetLogLevel(host, "info", false);
    Assert(setInfoWithoutAnnounce, "Expected info token to be accepted with announce disabled");
    Assert(runtime.GetLogLevelValue() == 2, "Expected info token to keep level at 2");
    Assert(host.Logs.empty(), "No log entry should be emitted when announce=false");
}

void Runtime_LogFilteringByLevel()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.log-filtering.test.json";

    {
        std::ofstream configFile(configPath);
        configFile
            << "{"
            << "\"ingestBaseUrl\":\"https://example.test/ingest\","
            << "\"ingestSubscriptionKey\":\"sub-key-test\","
            << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\","
            << "\"gameType\":\"CallOfDuty4\","
            << "\"refreshIntervalSeconds\":120"
            << "}";
    }

    FakeHost host;
    host.CurrentTime = 5000;

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");
    runtime.HandleServerSpawned(host);

    host.Logs.clear();
    const bool setErrorResult = runtime.TrySetLogLevel(host, "error", false);
    Assert(setErrorResult, "Expected error log level token to be accepted");

    runtime.Tick(host);
    runtime.Tick(host);

    bool sawErrorMessage = false;
    bool sawDebugMessageAtErrorLevel = false;
    for (const auto& line : host.Logs)
    {
        if (line.find("ingest request failed") != std::string::npos)
        {
            sawErrorMessage = true;
        }

        if (line.find("starting ingest batch POST") != std::string::npos)
        {
            sawDebugMessageAtErrorLevel = true;
        }
    }

    Assert(sawErrorMessage, "Expected error-level logs to remain visible at error log level");
    Assert(!sawDebugMessageAtErrorLevel, "Expected debug logs to be suppressed at error log level");

    host.Logs.clear();
    const bool setDebugResult = runtime.TrySetLogLevel(host, "debug", false);
    Assert(setDebugResult, "Expected debug log level token to be accepted");

    host.CurrentTime += 30;
    runtime.Tick(host);

    bool sawDebugMessageAtDebugLevel = false;
    for (const auto& line : host.Logs)
    {
        if (line.find("starting ingest batch POST") != std::string::npos)
        {
            sawDebugMessageAtDebugLevel = true;
            break;
        }
    }

    Assert(sawDebugMessageAtDebugLevel, "Expected debug logs to be visible at debug log level");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
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
    Runtime_LoadsConfigAndStoresServerContext();
    Runtime_EmitsAndFlushesPlayerConnectedEvent();
    Runtime_AuthorizedIdentity_AllowsDisconnectEventWhenPlayerIdUnavailableAtDisconnect();
    Runtime_DropsPoisonEventsAndUnblocksOtherQueues();
    Runtime_HandleClientCommand_IgnoresPortalOwnedCommands();
    Runtime_HandleChatMessage_DoesNotInterceptPortalOwnedCommands();
    Runtime_HandleClientCommand_DoesNotPrefixMatchLongerToken();
    Runtime_HandleClientCommand_DedupesCrossCallbackPath();
    Runtime_HandleClientCommand_PortalPluginHealth_UsesConsoleAndTellFlow();
    Runtime_HandleClientCommand_PortalPluginHealth_RespectsCommandAuthorization();
    Runtime_HandleClientCommand_PortalPluginHealth_RespectsPortalEnabledFlag();
    Runtime_HandleClientCommand_PortalPluginHealth_IgnoresMalformedPortalEnabledFlag();
    Runtime_LoadsActiveBanCacheAndAnswersBanQuery();
    Runtime_PlayerBanMutationHintsUpdateCacheImmediately();
    Runtime_LogLevelDefaultsAndParsing();
    Runtime_LogLevelSetWithoutAnnouncement_DoesNotLog();
    Runtime_LogFilteringByLevel();
    InitializePlugin_EmitsLogAndBroadcast();
    InitializePlugin_FallsBackWhenPrefixOrVersionMissing();

    std::cout << "All plugin runtime tests passed." << std::endl;
    return 0;
}
