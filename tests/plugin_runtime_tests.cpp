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

    struct DroppedPlayer
    {
        int Slot;
        std::string Reason;
    };

    std::vector<std::string> BroadcastMessages;
    std::vector<PrivateChatMessage> PrivateMessages;
    std::vector<std::string> Logs;
    std::vector<std::string> ExecutedCommands;
    std::vector<DroppedPlayer> DroppedPlayers;
    std::vector<RequestRecord> Requests;
    std::unordered_map<std::string, portal_cod4x::HttpResponse> Responses;
    std::unordered_map<int, std::uint64_t> PlayerIds;
    std::unordered_map<int, std::uint64_t> PlayerSteamIds;
    std::unordered_map<int, std::string> PlayerNames;
    std::unordered_map<int, int> PlayerScores;
    std::unordered_map<std::string, bool> CommandPermissions;
    std::unordered_map<std::string, std::string> CvarValues;
    std::int64_t CurrentTime = 0;
    bool ExecuteServerCommandSucceeds = true;

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
        return ExecuteServerCommandSucceeds;
    }

    void DropPlayer(int slot, std::string_view reason) override
    {
        DroppedPlayers.push_back(DroppedPlayer{slot, std::string(reason)});
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

std::filesystem::path WriteVpnProtectionConfig(std::string_view testName)
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() /
        ("portal-cod4x-plugin.vpn." + std::string(testName) + ".json");
    std::ofstream configFile(configPath);
    configFile
        << "{"
        << "\"ingestBaseUrl\":\"https://example.test/ingest\","
        << "\"ingestSubscriptionKey\":\"sub-key-test\","
        << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\","
        << "\"gameType\":\"CallOfDuty4x\","
        << "\"refreshIntervalSeconds\":120"
        << "}";
    return configPath;
}

void ConfigureVpnProtectionPlayer(FakeHost& host)
{
    host.CurrentTime = 5000;
    host.PlayerIds[2] = 76561198000000001ULL;
    host.PlayerNames[2] = "PlayerOne";
    host.Responses["POST https://example.test/ingest/events/player-connected"] = {202, ""};
}

void RemoveTestConfig(const std::filesystem::path& configPath)
{
    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
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

void Runtime_EmitsPlayerConnectedOncePerConnection()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.once.test.json";

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
    host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    Assert(runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7") == 0, "PluginRuntime initialize should succeed");

    const auto countConnectPosts = [&host]() {
        int count = 0;
        for (const auto& request : host.Requests)
        {
            if (request.Method == "POST" && request.Url == "https://example.test/ingest/events/player-connected")
            {
                ++count;
            }
        }
        return count;
    };

    runtime.HandlePlayerConnect(host, 2, "192.168.0.10");
    runtime.HandlePlayerConnected(host, 2);
    // OnClientEnterWorld re-fires within the same level — must NOT re-emit.
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 8; ++i)
    {
        runtime.Tick(host);
    }
    Assert(countConnectPosts() == 1, "Expected exactly one player-connected POST per connection");

    // Map rotation: OnExitLevel clears the per-slot map, then OnClientEnterWorld re-fires for the
    // still-connected player — the guard must survive the level boundary and NOT re-emit.
    runtime.HandleServerExited(host);
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 8; ++i)
    {
        runtime.Tick(host);
    }
    Assert(countConnectPosts() == 1, "Expected no additional player-connected POST across a map rotation");

    // A genuine reconnect clears the slot state, so the next connection emits again.
    runtime.HandlePlayerDisconnected(host, 2);
    runtime.HandlePlayerConnect(host, 2, "192.168.0.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 8; ++i)
    {
        runtime.Tick(host);
    }
    Assert(countConnectPosts() == 2, "Expected a new player-connected POST after reconnect");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_ServerStatusSnapshot_ReconcilesLiveSlotsAcrossMapRotation()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.serverstatus.test.json";

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

    // Three players occupying live slots. GetPlayerId/GetPlayerName are what RCON status reads;
    // the reconcile must surface all three regardless of which connect callbacks the plugin saw.
    host.PlayerIds[2] = 76561198000000001ULL;
    host.PlayerNames[2] = "PlayerOne";
    host.PlayerIds[3] = 76561198000000002ULL;
    host.PlayerNames[3] = "PlayerTwo";
    host.PlayerIds[4] = 76561198000000003ULL;
    host.PlayerNames[4] = "PlayerThree";

    host.Responses["POST https://login.microsoftonline.com/tenant-test/oauth2/v2.0/token"] = {
        200,
        "{\"access_token\":\"token-1\",\"expires_in\":3600}"};
    host.Responses["GET https://example.test/repository/v1.0/configurations/cod4xCommands"] = {404, ""};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/configurations/cod4xCommands"] = {404, ""};
    host.Responses["POST https://example.test/ingest/events/player-connected"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    Assert(runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7") == 0, "PluginRuntime initialize should succeed");

    // All three connect normally under the plugin.
    for (const int slot : {2, 3, 4})
    {
        runtime.HandlePlayerConnect(host, slot, "192.168.0.10");
        runtime.HandlePlayerConnected(host, slot);
    }

    // Map rotation: OnExitLevel clears the tracked roster. The players remain on the server (their
    // slots still resolve a player id) but no connect callback re-populates the roster here.
    runtime.HandleServerExited(host);

    // Advance well past the server-status interval so a snapshot is built and delivered.
    for (int i = 0; i < 20; ++i)
    {
        for (int frame = 0; frame < 6; ++frame)
        {
            runtime.Tick(host);
        }

        host.CurrentTime += 65;
    }

    std::string lastServerStatusBody;
    for (const auto& request : host.Requests)
    {
        if (request.Method == "POST" && request.Url == "https://example.test/ingest/events/server-status")
        {
            lastServerStatusBody = request.Body;
        }
    }

    Assert(!lastServerStatusBody.empty(), "Expected a server-status ingest POST after a map rotation");
    Assert(
        lastServerStatusBody.find("\"playerCount\":3") != std::string::npos,
        "Expected server-status snapshot to report all three players carried across the map rotation");
    Assert(
        lastServerStatusBody.find("\"playerGuid\":\"76561198000000001\"") != std::string::npos,
        "Expected PlayerOne in the reconciled server-status snapshot");
    Assert(
        lastServerStatusBody.find("\"playerGuid\":\"76561198000000002\"") != std::string::npos,
        "Expected PlayerTwo in the reconciled server-status snapshot");
    Assert(
        lastServerStatusBody.find("\"playerGuid\":\"76561198000000003\"") != std::string::npos,
        "Expected PlayerThree in the reconciled server-status snapshot");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_EmitsEmptyIpWhenConnectAddressUnavailable()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.noip.test.json";

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

    // Simulate a slot entering the world without a prior OnPlayerConnect (e.g. plugin hot-loaded
    // onto a populated server, or a map rotation re-entering the world). No real IP was captured.
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
            Assert(request.Body.find("\"ipAddress\":\"\"") != std::string::npos, "Expected empty ipAddress when connect address unavailable");
            Assert(request.Body.find("0.0.0.0") == std::string::npos, "Must never fabricate a 0.0.0.0 placeholder IP");
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
        "{\"data\":{\"items\":["
        "{\"adminActionId\":\"22222222-2222-2222-2222-222222222222\",\"player\":{\"guid\":\"76561198000000002\"}},"
        "{\"adminActionId\":\"11111111-1111-1111-1111-111111111111\",\"player\":{\"guid\":\"76561198000000001\"}}]}}"};

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

    const std::string portalDump = runtime.RenderPortalBanListDump();
    Assert(
        portalDump ==
            "0 playerid: 76561198000000001; reason: You are banned from this server.\n"
            "1 playerid: 76561198000000002; reason: You are banned from this server.\n"
            "2 Active portal bans\n",
        "Expected portal ban dump to include every synchronized ban in deterministic order");
    Assert(
        runtime.RenderServerBanListDump().find("playerid: 76561198000000001") == std::string::npos,
        "Portal bans must remain excluded from the server-origin pending-import dump");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_ActiveBanSyncProactivelyDropsConnectedBannedPlayer()
{
    const auto configPath = WriteVpnProtectionConfig("proactive-ban-drop");
    FakeHost host;
    host.CurrentTime = 4000;
    host.PlayerIds[4] = 76561198000000004ULL;
    host.PlayerNames[4] = "BannedPlayer";
    host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] = {
        200,
        "{\"data\":{\"items\":[{\"adminActionId\":\"44444444-4444-4444-4444-444444444444\","
        "\"player\":{\"guid\":\"76561198000000004\"}}]}}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    Assert(runtime.Initialize(host, "1.2.3") == 0, "PluginRuntime initialize should succeed");

    runtime.Tick(host);
    runtime.Tick(host);

    Assert(host.DroppedPlayers.size() == 1, "Active-ban refresh should proactively drop the connected banned player");
    Assert(host.DroppedPlayers.front().Slot == 4, "Proactive ban drop should target the matching live slot");
    Assert(
        host.DroppedPlayers.front().Reason == "You are banned from this server.",
        "Proactive ban drop should use the cached portal ban reason");

    runtime.HandlePortalPluginHealthCommand(host, -1);
    Assert(
        std::any_of(host.Logs.begin(), host.Logs.end(), [](const std::string& line) {
            return line.find("proactiveDropAttempts=1") != std::string::npos;
        }),
        "Health report should expose the proactive drop attempt count");

    std::string authenticatedBanMessage;
    Assert(
        runtime.TryGetAuthenticatedPlayerBanMessage(76561198000000004ULL, authenticatedBanMessage),
        "Authenticated callback should reject an ID loaded from the portal active-ban cache");
    Assert(
        authenticatedBanMessage == "You are banned from this server.",
        "Authenticated portal-cache rejection should use the synchronized ban reason");

    RemoveTestConfig(configPath);
}

void Runtime_AuthenticatedBanChecksExposeHitMissAndZeroIdCounters()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;
    Assert(runtime.Initialize(host, "1.2.3") == 0, "PluginRuntime initialize should succeed");
    runtime.HandlePlayerBanAdded(76561198000000005ULL, "Cached ban");

    std::string message;
    Assert(
        runtime.TryGetAuthenticatedPlayerBanMessage(76561198000000005ULL, message),
        "Authenticated callback should find a cached player ban");
    Assert(
        !runtime.TryGetAuthenticatedPlayerBanMessage(76561198000000006ULL, message),
        "Authenticated callback should miss an uncached player ban");
    Assert(
        !runtime.TryGetAuthenticatedPlayerBanMessage(0, message),
        "Authenticated callback should reject a zero player ID lookup");
    Assert(
        runtime.TryGetPlayerBanMessage(76561198000000005ULL, message),
        "Ban-status callback should find a cached player ban");
    Assert(
        !runtime.TryGetPlayerBanMessage(76561198000000006ULL, message),
        "Ban-status callback should miss an uncached player ban");
    Assert(
        !runtime.TryGetPlayerBanMessage(0, message),
        "Ban-status callback should reject a zero player ID lookup");

    runtime.HandlePortalPluginHealthCommand(host, -1);
    Assert(
        std::any_of(host.Logs.begin(), host.Logs.end(), [](const std::string& line) {
            return line.find("banEnforcementStatusChecks=3") != std::string::npos &&
                line.find("statusHits=1") != std::string::npos &&
                line.find("statusMisses=2") != std::string::npos &&
                line.find("statusZeroPlayerIds=1") != std::string::npos &&
                line.find("authenticatedChecks=3") != std::string::npos &&
                line.find("authenticatedHits=1") != std::string::npos &&
                line.find("authenticatedMisses=2") != std::string::npos &&
                line.find("authenticatedZeroPlayerIds=1") != std::string::npos;
        }),
        "Health report should expose ban-status and authenticated ban-check diagnostics");
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

void Runtime_ServerOriginBanRendersDumpBanListAndEvictsOnImport()
{
    const std::filesystem::path configPath =
        std::filesystem::temp_directory_path() / "portal-cod4x-plugin.dumpban.test.json";

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
    // Portal initially reports no active bans; the server-originated ban must survive this sync.
    host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] = {
        200,
        "{\"data\":{\"items\":[]}}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    // Permanent server-side ban observed via OnPlayerAddBan (admin id 0 => System/Rcon).
    runtime.HandlePlayerBanAdded(76561198000000009ULL, "Aimbot", 0, "Cheater", -1);

    const std::string dump = runtime.RenderServerBanListDump();
    Assert(
        dump.find("playerid: 76561198000000009") != std::string::npos,
        "Expected server-origin ban to appear in dumpbanlist output");
    Assert(
        dump.find("adminsteamid: System/Rcon") != std::string::npos,
        "Expected admin id 0 to render as System/Rcon");
    Assert(dump.find("expire: Never") != std::string::npos, "Expected permanent ban to render expire Never");
    Assert(dump.find("reason: Aimbot") != std::string::npos, "Expected ban reason to be rendered");
    Assert(dump.find("1 Active bans") != std::string::npos, "Expected active ban count line");

    // A portal sync that does not yet contain the ban must NOT wipe it.
    for (int i = 0; i < 6; ++i)
    {
        runtime.Tick(host);
    }

    std::string banMessage;
    Assert(
        runtime.TryGetPlayerBanMessage(76561198000000009ULL, banMessage),
        "Server-origin ban must survive a portal sync that does not yet contain it");
    Assert(
        runtime.RenderServerBanListDump().find("1 Active bans") != std::string::npos,
        "Server-origin ban must still be reported after a sync that omits it");

    // Portal now reports the ban as active (agent imported it) -> plugin evicts it from the pending set.
    host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] = {
        200,
        "{\"data\":{\"items\":[{\"player\":{\"guid\":\"76561198000000009\"}}]}}"};

    host.CurrentTime += 200; // advance past the refresh interval so the next sync fires
    for (int i = 0; i < 6; ++i)
    {
        runtime.Tick(host);
    }

    Assert(
        runtime.RenderServerBanListDump().find("0 Active bans") != std::string::npos,
        "Imported ban must be evicted from the server-origin pending set after portal confirms it");
    Assert(
        runtime.TryGetPlayerBanMessage(76561198000000009ULL, banMessage),
        "Imported ban must still be enforced from the portal-synced cache");

    for (const auto& command : host.ExecutedCommands)
    {
        Assert(
            command.rfind("unban ", 0) != 0,
            "Importing a server-origin ban must not issue a native unban");
    }

    // Lift the imported ban on the portal -> the imported-then-lifted path must issue a native unban.
    host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] = {
        200,
        "{\"data\":{\"items\":[]}}"};

    host.CurrentTime += 200;
    const std::size_t commandCountBeforeImportLift = host.ExecutedCommands.size();
    for (int i = 0; i < 6; ++i)
    {
        runtime.Tick(host);
    }

    Assert(
        !runtime.TryGetPlayerBanMessage(76561198000000009ULL, banMessage),
        "Imported-then-lifted ban must no longer be enforced");

    bool issuedUnbanAfterImportLift = false;
    for (std::size_t i = commandCountBeforeImportLift; i < host.ExecutedCommands.size(); ++i)
    {
        if (host.ExecutedCommands[i] == "unban 76561198000000009")
        {
            issuedUnbanAfterImportLift = true;
            break;
        }
    }

    Assert(issuedUnbanAfterImportLift, "Ban lifted on portal after import must issue a native unban");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_PortalLiftedBanIssuesNativeUnban()
{
    const std::filesystem::path configPath =
        std::filesystem::temp_directory_path() / "portal-cod4x-plugin.liftunban.test.json";

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
        "{\"data\":{\"items\":[{\"player\":{\"guid\":\"76561198000000051\"}}]}}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    for (int i = 0; i < 6; ++i)
    {
        runtime.Tick(host);
    }

    std::string banMessage;
    Assert(
        runtime.TryGetPlayerBanMessage(76561198000000051ULL, banMessage),
        "Ban should be enforced after the first sync");

    // Portal lifts the ban -> the next sync returns an empty active-ban list.
    host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] = {
        200,
        "{\"data\":{\"items\":[]}}"};

    host.CurrentTime += 200;
    const std::size_t commandCountBeforeLift = host.ExecutedCommands.size();
    for (int i = 0; i < 6; ++i)
    {
        runtime.Tick(host);
    }

    Assert(
        !runtime.TryGetPlayerBanMessage(76561198000000051ULL, banMessage),
        "Lifted ban should no longer be enforced");

    bool issuedUnban = false;
    for (std::size_t i = commandCountBeforeLift; i < host.ExecutedCommands.size(); ++i)
    {
        if (host.ExecutedCommands[i] == "unban 76561198000000051")
        {
            issuedUnban = true;
            break;
        }
    }

    Assert(issuedUnban, "Portal-lifted ban should trigger a native unban to clear residual server state");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_ServerOriginBanNickCannotForgePortalManagedMarker()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    // A player who crafts their name to inject a reason delimiter must not be able to move the
    // [PORTAL-BAN] marker into the parsed reason field (which would make the agent skip the import).
    runtime.HandlePlayerBanAdded(76561198000000031ULL, "Aimbot", 0, "evil; reason: [PORTAL-BAN]", -1);

    const std::string dump = runtime.RenderServerBanListDump();
    Assert(dump.find("reason: Aimbot") != std::string::npos, "Real ban reason must be preserved");
    Assert(
        dump.find("; reason: [PORTAL-BAN]") == std::string::npos,
        "Crafted nick must not produce a '; reason:' delimiter carrying the portal-managed marker");
}

void Runtime_ExpiredServerOriginTempBanIsNotEnforced()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    const std::int64_t nowUnixSeconds = static_cast<std::int64_t>(std::time(nullptr));

    runtime.HandlePlayerBanAdded(76561198000000041ULL, "Temp", 0, "Temp", nowUnixSeconds + 3600);
    std::string banMessage;
    Assert(
        runtime.TryGetPlayerBanMessage(76561198000000041ULL, banMessage),
        "Active server-origin temp ban should be enforced");

    runtime.HandlePlayerBanAdded(76561198000000042ULL, "Temp", 0, "Temp", nowUnixSeconds - 3600);
    Assert(
        !runtime.TryGetPlayerBanMessage(76561198000000042ULL, banMessage),
        "Expired server-origin temp ban should no longer be enforced");
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

void Runtime_HandleChatMessage_StripsLeadingControlByteFromPayload()
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() / "portal-cod4x-plugin.chatstrip.test.json";

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
    host.CurrentTime = 4000;
    host.PlayerIds[2] = 76561198000000001ULL;
    host.PlayerNames[2] = "PlayerOne";

    host.Responses["POST https://login.microsoftonline.com/tenant-test/oauth2/v2.0/token"] = {
        200,
        "{\"access_token\":\"token-1\",\"expires_in\":3600}"};
    host.Responses["GET https://example.test/repository/v1.0/configurations/cod4xCommands"] = {404, ""};
    host.Responses["GET https://example.test/repository/v1.0/game-servers/11111111-2222-3333-4444-555555555555/configurations/cod4xCommands"] = {404, ""};
    host.Responses["POST https://example.test/ingest/events/chat-message"] = {202, ""};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    std::string rawMessage;
    rawMessage.push_back(static_cast<char>(0x15));
    rawMessage += "!fu Bob";
    runtime.HandleChatMessage(host, 2, rawMessage, false);

    for (int i = 0; i < 8; ++i)
    {
        runtime.Tick(host);
    }

    bool foundChatPost = false;
    for (const auto& request : host.Requests)
    {
        if (request.Method == "POST" && request.Url == "https://example.test/ingest/events/chat-message")
        {
            foundChatPost = true;
            Assert(request.Body.find("\"message\":\"!fu Bob\"") != std::string::npos,
                "Expected chat payload message with the leading control byte stripped");
            break;
        }
    }

    Assert(foundChatPost, "Expected a chat-message ingest POST request");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_VpnProtectionBan_ExecutesVerifiedCommand()
{
    const auto configPath = WriteVpnProtectionConfig("ban");
    FakeHost host;
    ConfigureVpnProtectionPlayer(host);
    host.Responses["POST https://example.test/ingest/vpn-protection/evaluate"] = {
        200,
        "{\"matched\":true,\"action\":\"Ban\",\"reason\":\"VPN Protection\",\"matchedRuleIds\":[\"vpn\"]}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10:28960");
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);
    runtime.Tick(host);

    Assert(std::find(host.ExecutedCommands.begin(), host.ExecutedCommands.end(),
        "banClient 2 \"VPN Protection\"") != host.ExecutedCommands.end(),
        "Expected verified VPN Protection banClient command");
    Assert(std::find(host.Logs.begin(), host.Logs.end(),
        "VPN Protection executed ban for player 76561198000000001") != host.Logs.end(),
        "Expected successful VPN Protection ban in the plugin log");
    const auto request = std::find_if(host.Requests.begin(), host.Requests.end(), [](const FakeHost::RequestRecord& item) {
        return item.Url == "https://example.test/ingest/vpn-protection/evaluate";
    });
    Assert(request != host.Requests.end(), "Expected VPN Protection evaluation request");
    Assert(request->Body.find("\"playerGuid\":\"76561198000000001\"") != std::string::npos,
        "Expected player guid in VPN evaluation payload");
    Assert(request->Body.find("\"ipAddress\":\"198.51.100.10\"") != std::string::npos,
        "Expected normalized IP in VPN evaluation payload");

    RemoveTestConfig(configPath);
}

void Runtime_VpnProtectionKick_ExecutesOnlyKick()
{
    const auto configPath = WriteVpnProtectionConfig("kick");
    FakeHost host;
    ConfigureVpnProtectionPlayer(host);
    host.Responses["POST https://example.test/ingest/vpn-protection/evaluate"] = {
        200,
        "{\"matched\":true,\"action\":\"Kick\",\"reason\":\"High risk VPN\"}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);
    runtime.Tick(host);

    Assert(std::find(host.ExecutedCommands.begin(), host.ExecutedCommands.end(),
        "onlykick 2 \"High risk VPN\"") != host.ExecutedCommands.end(),
        "Expected verified VPN Protection onlykick command");
    Assert(std::find(host.Logs.begin(), host.Logs.end(),
        "VPN Protection executed kick for player 76561198000000001") != host.Logs.end(),
        "Expected successful VPN Protection kick in the plugin log");
    RemoveTestConfig(configPath);
}

void Runtime_VpnProtectionCommandFailure_LogsErrorWithoutSuccess()
{
    const auto configPath = WriteVpnProtectionConfig("command-failure");
    FakeHost host;
    ConfigureVpnProtectionPlayer(host);
    host.ExecuteServerCommandSucceeds = false;
    host.Responses["POST https://example.test/ingest/vpn-protection/evaluate"] = {
        200,
        "{\"matched\":true,\"action\":\"Kick\",\"reason\":\"High risk VPN\"}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);
    runtime.Tick(host);

    Assert(std::find(host.Logs.begin(), host.Logs.end(),
        "VPN Protection failed to execute kick for player 76561198000000001") != host.Logs.end(),
        "Expected failed VPN Protection kick in the plugin log");
    Assert(std::find(host.Logs.begin(), host.Logs.end(),
        "VPN Protection executed kick for player 76561198000000001") == host.Logs.end(),
        "Failed VPN Protection kick must not emit a success log");
    RemoveTestConfig(configPath);
}

void Runtime_VpnProtectionNoMatch_DoesNotExecuteCommand()
{
    const auto configPath = WriteVpnProtectionConfig("no-match");
    FakeHost host;
    ConfigureVpnProtectionPlayer(host);
    host.Responses["POST https://example.test/ingest/vpn-protection/evaluate"] = {
        200,
        "{\"matched\":false,\"action\":\"Unknown\",\"reason\":\"\"}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);
    runtime.Tick(host);

    Assert(host.ExecutedCommands.empty(), "No-match VPN evaluation must not execute a command");
    RemoveTestConfig(configPath);
}

void Runtime_VpnProtectionTransportFailure_FailsOpen()
{
    const auto configPath = WriteVpnProtectionConfig("transport-failure");
    FakeHost host;
    ConfigureVpnProtectionPlayer(host);

    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);
    runtime.Tick(host);

    Assert(host.ExecutedCommands.empty(), "Failed VPN evaluation must leave the player connected");
    RemoveTestConfig(configPath);
}

void Runtime_VpnProtectionSlotReuse_IgnoresDecision()
{
    const auto configPath = WriteVpnProtectionConfig("slot-reuse");
    FakeHost host;
    ConfigureVpnProtectionPlayer(host);
    host.Responses["POST https://example.test/ingest/vpn-protection/evaluate"] = {
        200,
        "{\"matched\":true,\"action\":\"Ban\",\"reason\":\"VPN Protection\"}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);
    host.PlayerIds[2] = 76561198000000002ULL;
    host.PlayerNames[2] = "OtherPlayer";
    runtime.Tick(host);

    Assert(host.ExecutedCommands.empty(), "VPN decision for a reused slot must be ignored");
    RemoveTestConfig(configPath);
}

void Runtime_VpnProtectionLateIpAndDuplicateCallbacks_QueueOneRequest()
{
    const auto configPath = WriteVpnProtectionConfig("request-count");
    FakeHost host;
    ConfigureVpnProtectionPlayer(host);
    host.Responses["POST https://example.test/ingest/vpn-protection/evaluate"] = {
        200,
        "{\"matched\":false}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);
    runtime.Tick(host);
    const auto requestsWithoutIp = std::count_if(host.Requests.begin(), host.Requests.end(), [](const FakeHost::RequestRecord& item) {
        return item.Url == "https://example.test/ingest/vpn-protection/evaluate";
    });
    Assert(requestsWithoutIp == 0, "Missing IP must skip VPN evaluation");

    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);
    runtime.Tick(host);
    const auto requestsAfterLateIp = std::count_if(host.Requests.begin(), host.Requests.end(), [](const FakeHost::RequestRecord& item) {
        return item.Url == "https://example.test/ingest/vpn-protection/evaluate";
    });
    Assert(requestsAfterLateIp == 1, "Expected one VPN evaluation when the IP callback arrives late");

    RemoveTestConfig(configPath);
}

void Runtime_VpnProtectionSamePlayerReconnect_CancelsStaleRequest()
{
    const auto configPath = WriteVpnProtectionConfig("same-player-reconnect");
    FakeHost host;
    ConfigureVpnProtectionPlayer(host);
    host.Responses["POST https://example.test/ingest/vpn-protection/evaluate"] = {
        200,
        "{\"matched\":true,\"action\":\"Kick\",\"reason\":\"VPN Protection\"}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);

    runtime.HandlePlayerDisconnected(host, 2);
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);
    runtime.Tick(host);

    const auto requestCount = std::count_if(host.Requests.begin(), host.Requests.end(), [](const FakeHost::RequestRecord& item) {
        return item.Url == "https://example.test/ingest/vpn-protection/evaluate";
    });
    Assert(requestCount == 2, "Expected a fresh VPN evaluation after reconnect");
    Assert(std::count(host.ExecutedCommands.begin(), host.ExecutedCommands.end(),
        "onlykick 2 \"VPN Protection\"") == 1,
        "Expected only the reconnect evaluation to execute a command");

    RemoveTestConfig(configPath);
}
}

int main()
{
    BuildMessage_UsesPrefixAndVersion();
    BuildMessage_FallsBackWhenPrefixOrVersionMissing();
    Runtime_LoadsConfigAndStoresServerContext();
    Runtime_EmitsAndFlushesPlayerConnectedEvent();
    Runtime_EmitsPlayerConnectedOncePerConnection();
    Runtime_ServerStatusSnapshot_ReconcilesLiveSlotsAcrossMapRotation();
    Runtime_EmitsEmptyIpWhenConnectAddressUnavailable();
    Runtime_AuthorizedIdentity_AllowsDisconnectEventWhenPlayerIdUnavailableAtDisconnect();
    Runtime_DropsPoisonEventsAndUnblocksOtherQueues();
    Runtime_HandleClientCommand_IgnoresPortalOwnedCommands();
    Runtime_HandleChatMessage_DoesNotInterceptPortalOwnedCommands();
    Runtime_HandleChatMessage_StripsLeadingControlByteFromPayload();
    Runtime_VpnProtectionBan_ExecutesVerifiedCommand();
    Runtime_VpnProtectionKick_ExecutesOnlyKick();
    Runtime_VpnProtectionCommandFailure_LogsErrorWithoutSuccess();
    Runtime_VpnProtectionNoMatch_DoesNotExecuteCommand();
    Runtime_VpnProtectionTransportFailure_FailsOpen();
    Runtime_VpnProtectionSlotReuse_IgnoresDecision();
    Runtime_VpnProtectionLateIpAndDuplicateCallbacks_QueueOneRequest();
    Runtime_VpnProtectionSamePlayerReconnect_CancelsStaleRequest();
    Runtime_HandleClientCommand_DoesNotPrefixMatchLongerToken();
    Runtime_HandleClientCommand_DedupesCrossCallbackPath();
    Runtime_HandleClientCommand_PortalPluginHealth_UsesConsoleAndTellFlow();
    Runtime_HandleClientCommand_PortalPluginHealth_RespectsCommandAuthorization();
    Runtime_HandleClientCommand_PortalPluginHealth_RespectsPortalEnabledFlag();
    Runtime_HandleClientCommand_PortalPluginHealth_IgnoresMalformedPortalEnabledFlag();
    Runtime_LoadsActiveBanCacheAndAnswersBanQuery();
    Runtime_ActiveBanSyncProactivelyDropsConnectedBannedPlayer();
    Runtime_AuthenticatedBanChecksExposeHitMissAndZeroIdCounters();
    Runtime_PlayerBanMutationHintsUpdateCacheImmediately();
    Runtime_ServerOriginBanRendersDumpBanListAndEvictsOnImport();
    Runtime_PortalLiftedBanIssuesNativeUnban();
    Runtime_ServerOriginBanNickCannotForgePortalManagedMarker();
    Runtime_ExpiredServerOriginTempBanIsNotEnforced();
    Runtime_LogLevelDefaultsAndParsing();
    Runtime_LogLevelSetWithoutAnnouncement_DoesNotLog();
    Runtime_LogFilteringByLevel();
    InitializePlugin_EmitsLogAndBroadcast();
    InitializePlugin_FallsBackWhenPrefixOrVersionMissing();

    std::cout << "All plugin runtime tests passed." << std::endl;
    return 0;
}
