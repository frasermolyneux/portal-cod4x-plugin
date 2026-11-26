#include "portal_cod4x/chat_messages.h"
#include "portal_cod4x/plugin_runtime.h"
#include "rules_json.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
    std::int64_t CurrentSubsecondTicks = 0;
    bool ExecuteServerCommandSucceeds = true;
    bool HoldRulesOutcome = false;
    int OpenRequests = 0;
    std::function<portal_cod4x::HttpResponse(const RequestRecord&)> ResponseFactory;
    std::function<void(int)> OnDrop;
    std::function<void(int)> OnChat;

    void BroadcastChat(std::string_view message) override
    {
        BroadcastMessages.emplace_back(message);
    }

    void SendChat(int slot, std::string_view message) override
    {
        PrivateMessages.push_back(PrivateChatMessage{slot, std::string(message)});
        if (OnChat)
            OnChat(slot);
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
        if (OnDrop)
        {
            OnDrop(slot);
        }
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
        ++OpenRequests;
        pending->IsRulesOutcome = url == "https://example.test/ingest/rules/outcomes";

        const auto it = Responses.find(key);
        if (ResponseFactory && std::string_view(url).find("/rules/") != std::string_view::npos)
        {
            pending->HasResponse = true;
            pending->Response = ResponseFactory(Requests.back());
        }
        else if (it != Responses.end())
        {
            pending->HasResponse = true;
            pending->Response = it->second;
        }
        else if (url == "https://example.test/ingest/events/playtime-observation" ||
            url == "https://example.test/ingest/events/player-ip-resolved")
        {
            pending->HasResponse = true;
            pending->Response = {202, ""};
        }

        return static_cast<portal_cod4x::HttpRequestHandle>(pending);
    }

    portal_cod4x::HttpRequestStatus PollHttpRequest(
        portal_cod4x::HttpRequestHandle handle,
        portal_cod4x::HttpResponse& response) override
    {
        auto* pending = static_cast<PendingRequest*>(handle);
        if (pending != nullptr && pending->IsRulesOutcome && HoldRulesOutcome)
        {
            return portal_cod4x::HttpRequestStatus::Pending;
        }
        if (pending == nullptr || !pending->HasResponse)
        {
            return portal_cod4x::HttpRequestStatus::Failed;
        }

        response = pending->Response;
        return portal_cod4x::HttpRequestStatus::Completed;
    }

    void EndHttpRequest(portal_cod4x::HttpRequestHandle handle) override
    {
        --OpenRequests;
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

    std::int64_t GetUnixTime100Nanoseconds() const override
    {
        return CurrentTime * 10000000 + CurrentSubsecondTicks;
    }

private:
    struct PendingRequest
    {
        bool HasResponse = false;
        bool IsRulesOutcome = false;
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

std::filesystem::path WriteRuntimeConfig(std::string_view testName,
    std::optional<std::string_view> agentName = std::nullopt)
{
    const std::filesystem::path configPath = std::filesystem::temp_directory_path() /
        ("portal-cod4x-plugin.runtime." + std::string(testName) + ".json");
    std::ofstream configFile(configPath);
    configFile
        << "{"
        << "\"ingestBaseUrl\":\"https://example.test/ingest\","
        << "\"ingestSubscriptionKey\":\"sub-key-test\","
        << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\","
        << "\"gameType\":\"CallOfDuty4x\","
        << "\"refreshIntervalSeconds\":120";
    if (agentName)
    {
        configFile << ",\"agentName\":" << portal_cod4x::RulesJson::Quote(*agentName);
    }
    configFile << "}";
    return configPath;
}

void ConfigureTestPlayer(FakeHost& host)
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

void Runtime_AgentNameControlsStartupAndRefreshesPresentationOnly()
{
    const auto config = WriteRuntimeConfig("agent-name", "  ^1[Server Override]^7  ");
    auto nameFile = config;
    nameFile.replace_extension(".agent-name.json");
    RemoveTestConfig(nameFile);
    FakeHost host;
    ConfigureTestPlayer(host);
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.123");
    Assert(host.BroadcastMessages.front() ==
        "^1[Server Override]^7 Portal Plugin is online (version 0.3.123)",
        "Startup must use the trimmed, already-rendered agentName without wrapping it again");
    runtime.SendPrivateChat(host, 2, "first");
    Assert(host.PrivateMessages.back().Message == "^5(PM)^7 ^1[Server Override]^7 first",
        "Private messages must place the coloured PM marker before the configured prefix");

    std::ifstream input(config);
    const std::string originalJson{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    auto document = *portal_cod4x::RulesJsonReader::Parse(originalJson);
    document.Fields["agentName"].Scalar = "^4[Renamed]^7";
    document.Fields["ingestBaseUrl"].Scalar = "https://replacement.test/ingest";
    document.Fields["ingestSubscriptionKey"].Scalar = "replacement-key";
    { std::ofstream output(config); output << document.Dump(); }
    auto nameDocument = *portal_cod4x::RulesJsonReader::Parse(
        "{\"gameServerId\":\"11111111-2222-3333-4444-555555555555\",\"agentName\":\"^4[Renamed]^7\"}");
    { std::ofstream output(nameFile); output << nameDocument.Dump(); }

    host.CurrentTime = 5059;
    runtime.Tick(host);
    runtime.SendPrivateChat(host, 2, "before refresh");
    Assert(host.PrivateMessages.back().Message.starts_with("^5(PM)^7 ^1[Server Override]^7 "),
        "Config must not be read on every frame");
    host.CurrentTime = 5060;
    runtime.Tick(host);
    runtime.SendPrivateChat(host, 2, "after refresh");
    Assert(host.PrivateMessages.back().Message == "^5(PM)^7 ^4[Renamed]^7 after refresh",
        "A running plugin must adopt a valid name update at the refresh boundary");

    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 12; ++i)
        runtime.Tick(host);
    Assert(!host.Requests.empty() &&
        std::all_of(host.Requests.begin(), host.Requests.end(), [](const auto& request) {
            return request.Url.starts_with("https://example.test/ingest") &&
                request.Headers.find("sub-key-test") != std::string::npos;
        }), "Name refresh must not replace loaded request endpoints or credentials");

    for (const auto value : {"null", "\"  \""})
    {
        nameDocument.Fields["agentName"] = *portal_cod4x::RulesJsonReader::Parse(value);
        { std::ofstream output(nameFile); output << nameDocument.Dump(); }
        host.CurrentTime += 60;
        runtime.Tick(host);
        runtime.SendPrivateChat(host, 2, "fallback");
        Assert(host.PrivateMessages.back().Message ==
            "^5(PM)^7 " + std::string(portal_cod4x::kDefaultBotPrefix) + " fallback",
            "Explicit null and blank agentName must restore legacy default behavior");
    }
    Assert(host.BroadcastMessages.size() == 1, "Name refresh must not repeat the startup announcement");
    runtime.Shutdown(host);
    nameDocument.Fields["agentName"] = *portal_cod4x::RulesJsonReader::Parse("\"^2[Persisted]^7\"");
    { std::ofstream output(nameFile); output << nameDocument.Dump(); }
    portal_cod4x::PluginRuntime restarted(config.string());
    restarted.Initialize(host, "0.3.123");
    Assert(host.BroadcastMessages.back().starts_with("^2[Persisted]^7 "),
        "A valid persisted name file must override the main config at the next startup");
    restarted.Shutdown(host);
    RemoveTestConfig(nameFile);
    RemoveTestConfig(config);
}

void Runtime_AgentNameRejectsUnsafeOrIncompleteRefreshes()
{
    const auto config = WriteRuntimeConfig("agent-name-invalid", "^1[Good]^7");
    auto nameFile = config;
    nameFile.replace_extension(".agent-name.json");
    RemoveTestConfig(nameFile);
    std::ifstream input(config);
    const std::string goodJson{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    const auto good = *portal_cod4x::RulesJsonReader::Parse(goodJson);
    FakeHost host;
    host.CurrentTime = 5000;
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.123");

    auto atLimit = good;
    atLimit.Fields["agentName"].Scalar = "^2[At Limit]^7";
    std::string atLimitJson = atLimit.Dump();
    atLimitJson.append(200 * 1024 - atLimitJson.size(), ' ');
    { std::ofstream output(nameFile, std::ios::binary); output << atLimitJson; }
    host.CurrentTime += 60;
    runtime.Tick(host);
    runtime.SendPrivateChat(host, 2, "boundary");
    Assert(host.PrivateMessages.back().Message == "^5(PM)^7 ^2[At Limit]^7 boundary",
        "A valid sidecar at the 200 KiB limit must still be read and applied");
    { std::ofstream output(nameFile, std::ios::binary); output << goodJson; }
    host.CurrentTime += 60;
    runtime.Tick(host);

    std::vector<std::string> invalidJson{"{", goodJson.substr(0, goodJson.size() - 1)};
    for (const auto& name : std::vector<std::string>{
        std::string(140, 'x'), "bad\"prefix", "bad\nprefix", std::string("bad\xed\xa0\x80")})
    {
        auto document = good;
        document.Fields["agentName"].Scalar = name;
        invalidJson.push_back(document.Dump());
    }
    auto document = good;
    document.Fields["agentName"] = *portal_cod4x::RulesJsonReader::Parse("false");
    invalidJson.push_back(document.Dump());
    document = good;
    document.Fields["gameServerId"].Scalar = "another-server";
    invalidJson.push_back(document.Dump());
    document = good;
    document.Fields.erase("agentName");
    invalidJson.push_back(document.Dump());
    invalidJson.push_back(goodJson.substr(0, goodJson.size() - 1) + ",\"agentName\":\"duplicate\"}");
    invalidJson.push_back(std::string(200 * 1024 + 1, ' '));
    for (const auto& json : invalidJson)
    {
        { std::ofstream output(nameFile, std::ios::binary); output << json; }
        host.CurrentTime += 60;
        runtime.Tick(host);
        runtime.SendPrivateChat(host, 2, "retained");
        Assert(host.PrivateMessages.back().Message == "^5(PM)^7 ^1[Good]^7 retained",
            "Malformed, unsafe or wrong-server refreshes must retain the last good prefix");
    }
    const auto prefixErrorCount = [&]() {
        return std::count_if(host.Logs.begin(), host.Logs.end(), [](const auto& line) {
            return line.starts_with("Plugin chat prefix config");
        });
    };
    const auto logsBeforeRepeat = prefixErrorCount();
    host.CurrentTime += 60;
    runtime.Tick(host);
    Assert(prefixErrorCount() == logsBeforeRepeat, "Repeated name refresh errors must not flood logs");
    Assert(runtime.GetServerContext().GameServerId == "11111111-2222-3333-4444-555555555555",
        "Name refresh must not change loaded server identity");
    RemoveTestConfig(nameFile);
    host.CurrentTime += 60;
    runtime.Tick(host);
    runtime.SendPrivateChat(host, 2, "missing file");
    Assert(host.PrivateMessages.back().Message == "^5(PM)^7 ^1[Good]^7 missing file",
        "A missing replacement file must not clear a working prefix");
    runtime.Shutdown(host);

    WriteRuntimeConfig("agent-name-invalid", std::string(140, 'x'));
    portal_cod4x::PluginRuntime invalidStartup(config.string());
    invalidStartup.Initialize(host, "0.3.123");
    Assert(host.BroadcastMessages.back().starts_with(std::string(portal_cod4x::kDefaultBotPrefix) + " "),
        "An unsafe initial name must not prevent the legacy startup announcement");
    invalidStartup.Shutdown(host);
    RemoveTestConfig(config);
}

void Runtime_LocalHeadersAppearOnlyInFirstFragment()
{
    const std::string prefix = std::string(135, 'N') + "\xc3\xa9^7";
    const auto config = WriteRuntimeConfig("agent-name-fragments", prefix);
    FakeHost host;
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.123");
    std::string message;
    for (int i = 0; i < 80; ++i)
        message += "\xf0\x9f\x98\x80";
    runtime.SendPrivateChat(host, 2, message);
    const auto privateFragments = portal_cod4x::PrepareChatMessages({host.PrivateMessages.back().Message});
    const auto publicFragments = portal_cod4x::PrepareChatMessages({host.BroadcastMessages.front()});
    Assert(privateFragments && privateFragments->size() > 1 &&
        privateFragments->front() == "^5(PM)^7 " + prefix + " " &&
        privateFragments->front().size() == portal_cod4x::kChatPayloadBytes,
        "The maximum allowed prefix and PM marker must fit intact in the first fragment");
    Assert(publicFragments && publicFragments->size() > 1 && publicFragments->front().starts_with(prefix + " "),
        "Public startup must include the configured header only before splitting");
    for (const auto* fragments : {&*privateFragments, &*publicFragments})
    {
        for (std::size_t i = 0; i < fragments->size(); ++i)
        {
            Assert((*fragments)[i].size() <= portal_cod4x::kChatPayloadBytes &&
                portal_cod4x::IsValidChatText((*fragments)[i]), "Fragments must respect UTF-8 and native byte limits");
            if (i > 0)
                Assert((*fragments)[i].find(prefix) == std::string::npos &&
                    (*fragments)[i].find("(PM)") == std::string::npos,
                    "Continuation fragments must not repeat bot or private-message headers");
        }
    }
    runtime.Shutdown(host);
    RemoveTestConfig(config);
}

void Runtime_AgentNameStartupRejectsStaleOrInvalidSidecars()
{
    const auto config = WriteRuntimeConfig("agent-name-startup", "^1[Current Install]^7");
    auto nameFile = config;
    nameFile.replace_extension(".agent-name.json");
    for (const auto json : {
        "{\"gameServerId\":\"previous-server\",\"agentName\":\"^2[Stale]^7\"}",
        "{\"gameServerId\":\"11111111-2222-3333-4444-555555555555\",\"agentName\":"})
    {
        { std::ofstream output(nameFile); output << json; }
        FakeHost host;
        portal_cod4x::PluginRuntime runtime(config.string());
        runtime.Initialize(host, "0.3.123");
        Assert(host.BroadcastMessages.front().starts_with("^1[Current Install]^7 "),
            "A stale-server or incomplete sidecar must not override the installed config at startup");
        Assert(std::any_of(host.Logs.begin(), host.Logs.end(), [](const auto& line) {
            return line.starts_with("Plugin chat prefix config");
        }), "Invalid startup sidecars must emit an explicit diagnostic");
        runtime.Shutdown(host);
    }
    RemoveTestConfig(nameFile);
    WriteRuntimeConfig("agent-name-startup");
    FakeHost olderAgent;
    portal_cod4x::PluginRuntime compatible(config.string());
    compatible.Initialize(olderAgent, "0.3.123");
    Assert(olderAgent.BroadcastMessages.front().starts_with(std::string(portal_cod4x::kDefaultBotPrefix) + " "),
        "Older agents with neither optional field nor name file must retain the original startup prefix");
    compatible.Shutdown(olderAgent);
    RemoveTestConfig(config);
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

std::string JsonStringField(const std::string& payload, const std::string& field)
{
    const std::string token = "\"" + field + "\":\"";
    const auto start = payload.find(token);
    if (start == std::string::npos)
    {
        return {};
    }
    const auto valueStart = start + token.size();
    const auto end = payload.find('"', valueStart);
    return end == std::string::npos ? std::string() : payload.substr(valueStart, end - valueStart);
}

void Runtime_PlaytimeMetadataSurvivesRetryAndReconnect()
{
    const auto configPath = WriteRuntimeConfig("playtime");
    FakeHost host;
    ConfigureTestPlayer(host);
    const std::string connectUrl = "https://example.test/ingest/events/player-connected";
    host.Responses["POST " + connectUrl] = {500, ""};
    host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/map-change"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/server-connected"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);
    runtime.Tick(host);
    host.Responses["POST " + connectUrl] = {202, ""};
    host.CurrentTime += 3;
    runtime.Tick(host);
    runtime.Tick(host);

    std::vector<std::string> connectBodies;
    for (const auto& request : host.Requests)
    {
        if (request.Url == connectUrl)
        {
            connectBodies.push_back(request.Body);
        }
    }
    Assert(connectBodies.size() == 2, "Expected retry of player-connected event");
    const auto messageId = JsonStringField(connectBodies[0], "messageId");
    const auto epoch = JsonStringField(connectBodies[0], "producerEpoch");
    Assert(!messageId.empty() && JsonStringField(connectBodies[0], "observationId") == messageId,
        "Playtime observation ID must equal the broker message ID");
    Assert(JsonStringField(connectBodies[1], "observationId") == messageId, "Retry must retain observation ID");
    Assert(!epoch.empty() && JsonStringField(connectBodies[1], "producerEpoch") == epoch, "Retry must retain producer epoch");
    Assert(JsonStringField(connectBodies[0], "eventUtc") == JsonStringField(connectBodies[1], "eventUtc"),
        "Retry must retain observation time");
    Assert(JsonStringField(connectBodies[0], "eventUtc") == "1970-01-01T01:23:20Z",
        "Playtime observation must use the callback's UTC time");
    Assert(connectBodies[0].find("\"timestampQuality\":2") != std::string::npos &&
        connectBodies[0].find("\"producerSequence\":1") != std::string::npos &&
        connectBodies[0].find("\"rosterCompleteness\":0") != std::string::npos &&
        connectBodies[0].find("\"isBot\":null") != std::string::npos &&
        connectBodies[0].find("\"connectionCorrelationId\":null") != std::string::npos,
        "Player event must carry producer timestamp and non-roster metadata");

    runtime.HandlePlayerDisconnected(host, 2);
    runtime.HandlePlayerConnected(host, 2);
    runtime.HandleServerSpawned(host);
    for (int i = 0; i < 16; ++i)
    {
        runtime.Tick(host);
    }

    bool sawReconnect = false;
    bool sawDisconnect = false;
    bool sawMap = false;
    bool sawStatus = false;
    for (const auto& request : host.Requests)
    {
        if (request.Url == connectUrl && JsonStringField(request.Body, "observationId") != messageId)
        {
            sawReconnect = JsonStringField(request.Body, "producerEpoch") == epoch;
        }
        if (request.Url.find("/events/player-disconnected") != std::string::npos)
        {
            sawDisconnect = request.Body.find("\"timestampQuality\":2") != std::string::npos &&
                request.Body.find("\"isBot\":null") != std::string::npos;
        }
        if (request.Url.find("/events/map-change") != std::string::npos)
        {
            sawMap = request.Body.find("\"timestampQuality\":2") != std::string::npos;
        }
        if (request.Url.find("/events/server-status") != std::string::npos)
        {
            sawStatus = request.Body.find("\"timestampQuality\":3") != std::string::npos &&
                request.Body.find("\"rosterCompleteness\":2") != std::string::npos &&
                request.Body.find("\"isBot\":null") != std::string::npos;
        }
    }
    Assert(sawReconnect && sawDisconnect && sawMap && sawStatus, "Expected metadata on reconnect, disconnect, map and partial status");

    host.PlayerIds.clear();
    host.PlayerNames.clear();
    host.CurrentTime += 61;
    const auto requestCountBeforeEmptySnapshot = host.Requests.size();
    for (int i = 0; i < 6; ++i)
    {
        runtime.Tick(host);
    }
    const bool sawEmptyPartialStatus = std::any_of(host.Requests.begin() + requestCountBeforeEmptySnapshot, host.Requests.end(), [](const FakeHost::RequestRecord& request) {
        return request.Url.find("/events/server-status") != std::string::npos &&
            request.Body.find("\"playerCount\":0") != std::string::npos &&
            request.Body.find("\"rosterCompleteness\":2") != std::string::npos;
    });
    Assert(sawEmptyPartialStatus,
        "An apparently empty roster must remain partial");
    RemoveTestConfig(configPath);
}

void Runtime_InFlightBatchDoesNotAcknowledgeEvictedIndices()
{
    const auto configPath = WriteRuntimeConfig("batch-eviction");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/events/chat-message"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/server-connected"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/map-change"] = {202, ""};
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandleChatMessage(host, 2, "hello", false);
    runtime.Tick(host);
    for (int i = 0; i < 499; ++i)
    {
        runtime.HandleServerSpawned(host);
    }
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    const bool sentFirstServerEvent = std::any_of(host.Requests.begin(), host.Requests.end(), [](const FakeHost::RequestRecord& request) {
        return request.Url.find("/events/server-connected") != std::string::npos &&
            request.Body.find("\"sequenceId\":2,") != std::string::npos;
    });
    Assert(sentFirstServerEvent, "Eviction of an in-flight event must not acknowledge a different queued event");
    RemoveTestConfig(configPath);
}

void Runtime_RestartLosesUnsentEvents()
{
    const auto configPath = WriteRuntimeConfig("restart-loss");
    FakeHost host;
    ConfigureTestPlayer(host);
    {
        portal_cod4x::PluginRuntime runtime(configPath.string());
        runtime.Initialize(host, "0.2.0");
        runtime.HandlePlayerConnected(host, 2);
    }

    portal_cod4x::PluginRuntime restarted(configPath.string());
    restarted.Initialize(host, "0.2.0");
    for (int i = 0; i < 4; ++i)
    {
        restarted.Tick(host);
    }
    const auto connectedPosts = std::count_if(host.Requests.begin(), host.Requests.end(), [](const FakeHost::RequestRecord& request) {
        return request.Url.find("/events/player-connected") != std::string::npos;
    });
    Assert(connectedPosts == 0, "Restart must not imply replay of an unsent in-memory event");
    restarted.HandlePlayerConnected(host, 2);
    restarted.Tick(host);
    const auto postsAfterNewObservation = std::count_if(host.Requests.begin(), host.Requests.end(), [](const FakeHost::RequestRecord& request) {
        return request.Url.find("/events/player-connected") != std::string::npos;
    });
    Assert(postsAfterNewObservation == 1, "A new post-restart observation should still be emitted");
    RemoveTestConfig(configPath);
}

void Runtime_CoverageCountsCapacityLoss()
{
    const auto configPath = WriteRuntimeConfig("coverage-capacity");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandleChatMessage(host, 2, "not a playtime event", false);
    for (int i = 0; i < 251; ++i)
    {
        runtime.HandlePlayerConnected(host, 2);
        runtime.HandlePlayerDisconnected(host, 2);
    }

    host.CurrentTime += 61;
    for (int i = 0; i < 100; ++i)
    {
        runtime.Tick(host);
    }
    const auto status = std::find_if(host.Requests.begin(), host.Requests.end(), [](const FakeHost::RequestRecord& request) {
        return request.Url.find("/events/playtime-observation") != std::string::npos &&
            request.Body.find("\"kind\":4") != std::string::npos;
    });
    Assert(status != host.Requests.end(), "Expected a status after the full queue drains");
    Assert(status->Body.find("\"producerStartedUtc\":\"1970-01-01T01:23:20Z\"") != std::string::npos &&
        status->Body.find("\"droppedCapacity\":2") != std::string::npos &&
        status->Body.find("\"droppedAttempts\":0") != std::string::npos &&
        status->Body.find("\"droppedAge\":0") != std::string::npos,
        "Status must count both evicted playtime events but not the evicted chat");

    const auto oldEpoch = JsonStringField(status->Body, "producerEpoch");
    const auto oldRequestCount = host.Requests.size();
    portal_cod4x::PluginRuntime restarted(configPath.string());
    restarted.Initialize(host, "0.2.0");
    host.CurrentTime += 61;
    for (int i = 0; i < 6; ++i)
    {
        restarted.Tick(host);
    }
    const auto newStatus = std::find_if(host.Requests.begin() + oldRequestCount, host.Requests.end(), [](const FakeHost::RequestRecord& request) {
        return request.Url.find("/events/playtime-observation") != std::string::npos &&
            request.Body.find("\"kind\":4") != std::string::npos;
    });
    Assert(newStatus != host.Requests.end() && JsonStringField(newStatus->Body, "producerEpoch") != oldEpoch &&
        newStatus->Body.find("\"droppedCapacity\":0") != std::string::npos &&
        newStatus->Body.find("\"droppedAttempts\":0") != std::string::npos &&
        newStatus->Body.find("\"droppedAge\":0") != std::string::npos,
        "Restart must use a new epoch with reset counters, without implying continuity");
    RemoveTestConfig(configPath);
}

void Runtime_CoverageCountsAgeAndSimultaneousThresholds()
{
    const auto configPath = WriteRuntimeConfig("coverage-age");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
    {
        portal_cod4x::PluginRuntime runtime(configPath.string());
        runtime.Initialize(host, "0.2.0");
        runtime.HandlePlayerConnected(host, 2);
        host.CurrentTime += 901;
        for (int i = 0; i < 10; ++i)
        {
            runtime.Tick(host);
        }
        host.CurrentTime += 61;
        for (int i = 0; i < 8; ++i)
        {
            runtime.Tick(host);
        }
        const bool sawAge = std::any_of(host.Requests.begin(), host.Requests.end(), [](const FakeHost::RequestRecord& request) {
            return request.Url.find("/events/playtime-observation") != std::string::npos &&
                request.Body.find("\"kind\":4") != std::string::npos &&
                request.Body.find("\"droppedAge\":1") != std::string::npos &&
                request.Body.find("\"droppedAttempts\":0") != std::string::npos;
        });
        Assert(sawAge, "Expired playtime event must count as age loss");
    }

    FakeHost retryHost;
    ConfigureTestPlayer(retryHost);
    retryHost.Responses["POST https://example.test/ingest/events/playtime-observation"] = {500, ""};
    retryHost.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(retryHost, "0.2.0");
    runtime.HandlePlayerConnected(retryHost, 2);
    for (int i = 0; i < 600; ++i)
    {
        runtime.Tick(retryHost);
        const auto attempts = std::count_if(retryHost.Requests.begin(), retryHost.Requests.end(), [](const FakeHost::RequestRecord& request) {
            return request.Url.find("/events/playtime-observation") != std::string::npos;
        });
        if (attempts == 8)
        {
            break;
        }
        retryHost.CurrentTime += 1;
    }
    retryHost.CurrentTime += 901;
    runtime.Tick(retryHost);
    retryHost.Responses["POST https://example.test/ingest/events/playtime-observation"] = {202, ""};
    retryHost.CurrentTime += 61;
    for (int i = 0; i < 30; ++i)
    {
        runtime.Tick(retryHost);
    }
    const bool sawAttempts = std::any_of(retryHost.Requests.begin(), retryHost.Requests.end(), [](const FakeHost::RequestRecord& request) {
        return request.Url.find("/events/playtime-observation") != std::string::npos &&
            request.Body.find("\"kind\":4") != std::string::npos &&
            request.Body.find("\"droppedAttempts\":1") != std::string::npos;
    });
    Assert(sawAttempts, "An event exceeding both thresholds must be counted once as an attempt loss");
    RemoveTestConfig(configPath);
}

void Runtime_CoverageCountsInFlightEviction()
{
    const auto configPath = WriteRuntimeConfig("coverage-in-flight");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnected(host, 2);
    runtime.Tick(host);

    for (int i = 0; i < 250; ++i)
    {
        runtime.HandlePlayerDisconnected(host, 2);
        runtime.HandlePlayerConnected(host, 2);
    }

    runtime.Tick(host);
    host.CurrentTime += 61;
    for (int i = 0; i < 100; ++i)
    {
        runtime.Tick(host);
    }
    const bool sawEviction = std::any_of(host.Requests.begin(), host.Requests.end(), [](const FakeHost::RequestRecord& request) {
        return request.Url.find("/events/playtime-observation") != std::string::npos &&
            request.Body.find("\"kind\":4") != std::string::npos &&
            request.Body.find("\"droppedCapacity\":2") != std::string::npos;
    });
    Assert(sawEviction, "In-flight eviction must count local retention loss even if the request succeeds");
    RemoveTestConfig(configPath);
}

void Runtime_CoverageRetainsCountersAcrossStatusRetry()
{
    const auto configPath = WriteRuntimeConfig("coverage-retry");
    FakeHost host;
    ConfigureTestPlayer(host);
    const std::string statusUrl = "https://example.test/ingest/events/playtime-observation";
    host.Responses["POST " + statusUrl] = {500, ""};
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnected(host, 2);
    host.CurrentTime += 901;
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    host.Responses["POST " + statusUrl] = {202, ""};
    host.CurrentTime += 3;
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    host.CurrentTime += 61;
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }

    std::vector<std::string> statusBodies;
    for (const auto& request : host.Requests)
    {
        if (request.Url == statusUrl)
        {
            statusBodies.push_back(request.Body);
        }
    }
    Assert(statusBodies.size() >= 3, "Expected failed status, retry and subsequent snapshot");
    Assert(JsonStringField(statusBodies[0], "observationId") == JsonStringField(statusBodies[1], "observationId") &&
        statusBodies[0].find("\"droppedAge\":0") != std::string::npos &&
        statusBodies[1].find("\"droppedAge\":0") != std::string::npos,
        "Retry must retain the original snapshot and observation ID");
    Assert(statusBodies.back().find("\"droppedAge\":1") != std::string::npos,
        "A later snapshot must report losses discovered after the original snapshot");
    RemoveTestConfig(configPath);
}

void Runtime_CoverageUsesFirstPublicationTimeAcrossRetries()
{
    const auto configPath = WriteRuntimeConfig("coverage-publication-time");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
    const std::string url = "https://example.test/ingest/events/playtime-observation";
    host.Responses["POST " + url] = {500, ""};
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");

    host.CurrentTime += 61;
    runtime.Tick(host);
    host.CurrentTime += 5;
    runtime.Tick(host);
    runtime.Tick(host);
    runtime.Tick(host);

    const auto snapshot = std::find_if(host.Requests.begin(), host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":4") != std::string::npos;
    });
    Assert(snapshot != host.Requests.end(), "Expected a delayed coverage snapshot");
    const std::string originalBody = snapshot->Body;
    Assert(JsonStringField(originalBody, "eventUtc") == "1970-01-01T01:24:21Z" &&
        JsonStringField(originalBody, "eventPublishedUtc") == "1970-01-01T01:24:26Z" &&
        JsonStringField(originalBody, "producerStartedUtc") == "1970-01-01T01:23:20Z" &&
        !JsonStringField(originalBody, "producerEpoch").empty() &&
        originalBody.find("\"droppedCapacity\":0") != std::string::npos &&
        originalBody.find("\"droppedAttempts\":0") != std::string::npos &&
        originalBody.find("\"droppedAge\":0") != std::string::npos,
        "Coverage must carry the epoch, nonnegative counters and actual first-publication time");

    host.Responses["POST " + url] = {202, ""};
    host.CurrentTime += 3;
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    const auto retries = std::count_if(host.Requests.begin(), host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body == originalBody;
    });
    Assert(retries == 2, "Coverage retry must preserve the entire payload used for replay identity");
    RemoveTestConfig(configPath);
}

void Runtime_DedicatedPlaytimeEventsPreserveIdentityAndPrivacy()
{
    const auto configPath = WriteRuntimeConfig("dedicated-playtime");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.CvarValues["mapname"] = "mp_crash";
    host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/server-connected"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/map-change"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
    const std::string url = "https://example.test/ingest/events/playtime-observation";
    host.Responses["POST " + url] = {500, ""};
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    host.Responses["POST " + url] = {202, ""};
    host.CurrentTime += 3;
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }

    std::vector<std::string> bodies;
    for (const auto& request : host.Requests)
    {
        if (request.Url == url)
        {
            bodies.push_back(request.Body);
        }
    }
    Assert(bodies.size() == 2, "Expected dedicated observation retry");
    const auto observation = JsonStringField(bodies[0], "observationId");
    Assert(!observation.empty() && observation == JsonStringField(bodies[0], "messageId") &&
        observation == JsonStringField(bodies[1], "observationId") &&
        JsonStringField(bodies[0], "eventPublishedUtc") == JsonStringField(bodies[1], "eventPublishedUtc"),
        "Dedicated observation ID and first-publish time must survive retry");
    Assert(bodies[0].find("\"kind\":1") != std::string::npos &&
        bodies[0].find("\"eventUtc\":\"1970-01-01T01:23:20Z\"") != std::string::npos &&
        bodies[0].find("\"freshness\":1") != std::string::npos &&
        bodies[0].find("\"isBot\":null") != std::string::npos &&
        bodies[0].find("198.51.100.10") == std::string::npos &&
        bodies[0].find("\"ipAddress\"") == std::string::npos,
        "Dedicated join must use live callback time and omit network address");

    const auto requestCountBeforeReentry = host.Requests.size();
    runtime.HandleServerExited(host);
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    const bool emittedMapReentryJoin = std::any_of(host.Requests.begin() + requestCountBeforeReentry, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":1") != std::string::npos;
    });
    Assert(!emittedMapReentryJoin, "Map re-entry without a new connection must not emit a join");
    const auto requestCountBeforeLateConnect = host.Requests.size();
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    const bool emittedLateConnectJoin = std::any_of(host.Requests.begin() + requestCountBeforeLateConnect, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":1") != std::string::npos;
    });
    Assert(!emittedLateConnectJoin, "A late connect callback must not create another join");
    const auto requestCountBeforeReconnect = host.Requests.size();
    runtime.HandlePlayerDisconnected(host, 2);
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    runtime.HandleServerSpawned(host);
    for (int i = 0; i < 30; ++i)
    {
        runtime.Tick(host);
    }
    std::string joinedBodies;
    for (std::size_t i = requestCountBeforeReconnect; i < host.Requests.size(); ++i)
    {
        if (host.Requests[i].Url == url)
        {
            joinedBodies += host.Requests[i].Body;
        }
    }
    Assert(joinedBodies.find("\"kind\":1") != std::string::npos &&
        joinedBodies.find("\"kind\":2") != std::string::npos &&
        joinedBodies.find("\"kind\":3") != std::string::npos &&
        joinedBodies.find("\"kind\":4") != std::string::npos &&
        joinedBodies.find("\"rosterCompleteness\":2") != std::string::npos &&
        joinedBodies.find("198.51.100.10") == std::string::npos,
        "Dedicated reconnect after disconnect, leave, map and partial roster must be emitted without IP");
    RemoveTestConfig(configPath);
}

void Runtime_DedicatedPlaytimeAmbiguousReconnectRemainsUncertain()
{
    const auto configPath = WriteRuntimeConfig("playtime-ambiguous-reconnect");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
    const std::string url = "https://example.test/ingest/events/playtime-observation";
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }

    // A zero-ID slot could be empty, a bot, or an unresolved player.
    host.PlayerIds.erase(2);
    host.PlayerNames.erase(2);
    host.CurrentTime += 61;
    const auto beforeSnapshot = host.Requests.size();
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    const auto snapshot = std::find_if(host.Requests.begin() + beforeSnapshot, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":4") != std::string::npos;
    });
    Assert(snapshot != host.Requests.end(), "Expected dedicated snapshot for unresolved slot");
    const auto& body = snapshot->Body;
    Assert(body.front() == '[' && body.back() == ']' &&
        body.find("\"serverId\":\"11111111-2222-3333-4444-555555555555\"") != std::string::npos &&
        body.find("\"gameType\":\"CallOfDuty4x\"") != std::string::npos &&
        !JsonStringField(body, "eventGeneratedUtc").empty() &&
        !JsonStringField(body, "eventPublishedUtc").empty() &&
        !JsonStringField(body, "producerEpoch").empty() &&
        JsonStringField(body, "observationId") == JsonStringField(body, "messageId") &&
        !JsonStringField(body, "observationId").empty() &&
        body.find("\"sequenceId\":") != std::string::npos &&
        body.find("\"producerSequence\":") != std::string::npos &&
        body.find("\"eventUtc\":\"1970-01-01T01:24:21Z\"") != std::string::npos &&
        body.find("\"timestampQuality\":3") != std::string::npos &&
        body.find("\"freshness\":1") != std::string::npos &&
        body.find("\"rosterCompleteness\":2") != std::string::npos &&
        body.find("\"players\":[]") != std::string::npos &&
        body.find("\"producerStartedUtc\":\"1970-01-01T01:23:20Z\"") != std::string::npos &&
        body.find("\"droppedCapacity\":0") != std::string::npos &&
        body.find("\"droppedAttempts\":0") != std::string::npos &&
        body.find("\"droppedAge\":0") != std::string::npos &&
        body.find("198.51.100.10") == std::string::npos,
        "Unresolved roster must have the dedicated envelope, partial status and local-only coverage counters");

    host.PlayerIds[2] = 76561198000000001ULL;
    host.PlayerNames[2] = "PlayerOne";
    const auto beforeAmbiguousConnect = host.Requests.size();
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    Assert(std::none_of(host.Requests.begin() + beforeAmbiguousConnect, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":1") != std::string::npos;
    }), "An unresolved slot and same-player callback cannot prove a reconnect");

    const auto beforeKnownReconnect = host.Requests.size();
    runtime.HandlePlayerDisconnected(host, 2);
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    bool sawLeave = false;
    bool sawJoin = false;
    for (auto it = host.Requests.begin() + beforeKnownReconnect; it != host.Requests.end(); ++it)
    {
        if (it->Url == url)
        {
            sawLeave |= it->Body.find("\"kind\":2") != std::string::npos;
            sawJoin |= it->Body.find("\"kind\":1") != std::string::npos;
        }
    }
    Assert(sawLeave && sawJoin, "An observed disconnect followed by reconnect must emit both dedicated events");
    RemoveTestConfig(configPath);
}

void Runtime_DedicatedPlaytimeTracksResolvedRosterIdentity()
{
    const auto configPath = WriteRuntimeConfig("playtime-roster-identity");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};
    const std::string url = "https://example.test/ingest/events/playtime-observation";
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    host.CurrentTime += 61;
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }

    auto start = host.Requests.size();
    runtime.HandleServerExited(host);
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    Assert(std::none_of(host.Requests.begin() + start, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":1") != std::string::npos;
    }), "A roster-observed player re-entering after a map change must not emit a join");

    host.PlayerIds[2] = 76561198000000002ULL;
    host.PlayerNames[2] = "PlayerTwo";
    host.CurrentTime += 61;
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    host.PlayerIds[2] = 76561198000000001ULL;
    host.PlayerNames[2] = "PlayerOne";
    start = host.Requests.size();
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    Assert(std::any_of(host.Requests.begin() + start, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":1") != std::string::npos &&
            request.Body.find("\"playerGuid\":\"76561198000000001\"") != std::string::npos;
    }), "A resolved replacement occupant must invalidate the previous join guard");

    runtime.HandleServerExited(host);
    host.PlayerIds.erase(2);
    host.PlayerNames.erase(2);
    start = host.Requests.size();
    runtime.HandlePlayerDisconnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    Assert(std::any_of(host.Requests.begin() + start, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":2") != std::string::npos &&
            request.Body.find("\"playerGuid\":\"76561198000000001\"") != std::string::npos &&
            request.Body.find("\"username\":\"PlayerOne\"") != std::string::npos;
    }), "A disconnect after level exit must retain the last resolved playtime identity");
    RemoveTestConfig(configPath);
}

void Runtime_DedicatedPlaytimeDoesNotGuessUnresolvedDisconnect()
{
    const auto configPath = WriteRuntimeConfig("playtime-unresolved-disconnect");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
    const std::string url = "https://example.test/ingest/events/playtime-observation";
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    host.PlayerIds.erase(2);
    host.PlayerNames.erase(2);
    host.CurrentTime += 61;
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    const auto start = host.Requests.size();
    runtime.HandlePlayerDisconnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    Assert(std::none_of(host.Requests.begin() + start, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":2") != std::string::npos;
    }), "An unresolved slot must not attribute a disconnect to a stale player");

    host.PlayerIds[2] = 76561198000000001ULL;
    host.PlayerNames[2] = "PlayerOne";
    runtime.HandlePlayerConnected(host, 2);
    runtime.HandleServerExited(host);
    host.PlayerIds[2] = 76561198000000002ULL;
    host.PlayerNames[2] = "PlayerTwo";
    const auto beforeReplacementDisconnect = host.Requests.size();
    runtime.HandlePlayerDisconnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    Assert(std::none_of(host.Requests.begin() + beforeReplacementDisconnect, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":2") != std::string::npos;
    }), "A different live occupant must not be attributed to the cached player");
    RemoveTestConfig(configPath);
}

void Runtime_DedicatedPlaytimeReconcilesUnresolvedEnterWorld()
{
    const auto configPath = WriteRuntimeConfig("playtime-unresolved-enter-world");
    const std::string url = "https://example.test/ingest/events/playtime-observation";
    for (const bool replacement : {false, true})
    {
        FakeHost host;
        ConfigureTestPlayer(host);
        host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};
        portal_cod4x::PluginRuntime runtime(configPath.string());
        runtime.Initialize(host, "0.2.0");
        runtime.HandlePlayerConnected(host, 2);
        for (int i = 0; i < 10; ++i)
        {
            runtime.Tick(host);
        }

        if (replacement)
        {
            host.PlayerIds[2] = 76561198000000002ULL;
        }
        else
        {
            host.PlayerIds.erase(2);
        }
        host.PlayerNames.erase(2);
        runtime.HandlePlayerConnected(host, 2);
        host.PlayerIds.erase(2);
        host.PlayerNames[2] = "PlayerOne";
        const auto start = host.Requests.size();
        runtime.HandlePlayerDisconnected(host, 2);
        for (int i = 0; i < 10; ++i)
        {
            runtime.Tick(host);
        }
        Assert(std::none_of(host.Requests.begin() + start, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
            return request.Url == url && request.Body.find("\"kind\":2") != std::string::npos;
        }), "An unresolved enter-world callback must not attribute a later disconnect to the prior player");
    }

    FakeHost host;
    ConfigureTestPlayer(host);
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    host.PlayerIds[2] = 76561198000000002ULL;
    host.PlayerNames.erase(2);
    runtime.HandlePlayerConnected(host, 2);
    host.PlayerNames[2] = "PlayerTwo";
    const auto start = host.Requests.size();
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    Assert(std::any_of(host.Requests.begin() + start, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":1") != std::string::npos &&
            request.Body.find("\"playerGuid\":\"76561198000000002\"") != std::string::npos;
    }), "A replacement initially missing a username must emit a join when its identity resolves");
    RemoveTestConfig(configPath);
}

void Runtime_DedicatedPlaytimeDisconnectUsesResolvedIdentity()
{
    const auto configPath = WriteRuntimeConfig("playtime-disconnect-identity");
    const std::string url = "https://example.test/ingest/events/playtime-observation";
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        FakeHost host;
        ConfigureTestPlayer(host);
        host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};
        portal_cod4x::PluginRuntime runtime(configPath.string());
        runtime.Initialize(host, "0.2.0");
        if (scenario == 0)
        {
            runtime.HandlePlayerConnect(host, 2, "");
            host.PlayerIds.erase(2);
            runtime.HandlePlayerConnected(host, 2);
            host.PlayerIds[2] = 76561198000000001ULL;
            runtime.HandleClientAuthorized(host);
        }
        else if (scenario == 1)
        {
            host.PlayerNames.erase(2);
            runtime.HandlePlayerConnected(host, 2);
            host.PlayerNames[2] = "PlayerOne";
        }
        else
        {
            runtime.HandlePlayerConnected(host, 2);
            runtime.HandleServerExited(host);
            host.PlayerIds[2] = 76561198000000002ULL;
            host.PlayerNames[2] = "PlayerTwo";
            runtime.HandlePlayerConnect(host, 2, "");
            runtime.HandleClientAuthorized(host);
        }
        for (int i = 0; i < 10; ++i)
        {
            runtime.Tick(host);
        }
        if (scenario != 1)
        {
            host.PlayerIds.erase(2);
        }
        const auto start = host.Requests.size();
        runtime.HandlePlayerDisconnected(host, 2);
        for (int i = 0; i < 10; ++i)
        {
            runtime.Tick(host);
        }
        const auto leave = std::find_if(host.Requests.begin() + start, host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
            return request.Url == url && request.Body.find("\"kind\":2") != std::string::npos;
        });
        Assert(leave != host.Requests.end(), "A resolved disconnect identity must emit a leave");
        const std::string expectedGuid = scenario == 2 ? "76561198000000002" : "76561198000000001";
        Assert(leave->Body.find("\"playerGuid\":\"" + expectedGuid + "\"") != std::string::npos,
            "The leave must identify the authorized occupant, not the stale slot occupant");
    }
    FakeHost host;
    ConfigureTestPlayer(host);
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "");
    host.PlayerIds.erase(2);
    runtime.HandlePlayerConnected(host, 2);
    host.PlayerIds[2] = 76561198000000001ULL;
    runtime.HandleClientAuthorized(host);
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 10; ++i)
    {
        runtime.Tick(host);
    }
    Assert(std::any_of(host.Requests.begin(), host.Requests.end(), [&](const FakeHost::RequestRecord& request) {
        return request.Url == url && request.Body.find("\"kind\":1") != std::string::npos;
    }), "Authorization alone must not suppress the player's first enter-world join");
    RemoveTestConfig(configPath);
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

    const std::size_t tellMessageCount = host.PrivateMessages.size();
    Assert(tellMessageCount == 1, "Cross-callback duplicate command response should be suppressed");

    host.CurrentTime = 101;
    runtime.HandleClientCommand(host, 3, "!portalpluginhealth");

    const std::size_t tellMessageCountAfterSecondInvoke = host.PrivateMessages.size();
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
    for (const auto& command : host.ExecutedCommands)
    {
        if (command.rfind("consay ", 0) == 0)
        {
            hasConsay = true;
        }

    }

    Assert(hasConsay, "Expected !portalpluginhealth to emit consay console output for player invocations");
    Assert(host.PrivateMessages.size() == 1 && host.PrivateMessages.front().Slot == 7 &&
        host.PrivateMessages.front().Message.starts_with("^5(PM)^7 " + std::string(portal_cod4x::kDefaultBotPrefix) + " "),
        "Expected health guidance to use prefixed private native chat");

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
        if (message.Slot == 8 && message.Message.starts_with("^5(PM)^7 " +
            std::string(portal_cod4x::kDefaultBotPrefix) + " ") && message.Message.find("not authorized") != std::string::npos)
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
        if (message.Slot == 8 && message.Message.starts_with("^5(PM)^7 " +
            std::string(portal_cod4x::kDefaultBotPrefix) + " ") && message.Message.find("currently disabled") != std::string::npos)
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
        "{\"adminActionId\":\"22222222-2222-2222-2222-222222222222\",\"type\":\"Ban\",\"player\":{\"guid\":\"76561198000000002\"}},"
        "{\"adminActionId\":\"11111111-1111-1111-1111-111111111111\",\"type\":\"Ban\",\"player\":{\"guid\":\"76561198000000001\"}}]}}"};

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
    const auto configPath = WriteRuntimeConfig("proactive-ban-drop");
    FakeHost host;
    host.CurrentTime = 4000;
    host.PlayerIds[4] = 76561198000000004ULL;
    host.PlayerNames[4] = "BannedPlayer";
    host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] = {
        200,
        "{\"data\":{\"items\":[{\"adminActionId\":\"44444444-4444-4444-4444-444444444444\","
        "\"type\":\"Ban\",\"player\":{\"guid\":\"76561198000000004\"}}]}}"};

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
        "{\"data\":{\"items\":[{\"type\":\"Ban\",\"player\":{\"guid\":\"76561198000000009\"}}]}}"};

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
        "{\"data\":{\"items\":[{\"type\":\"Ban\",\"player\":{\"guid\":\"76561198000000051\"}}]}}"};

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

void Runtime_JsonUnescapeDecodesUnicodeEscapesInConfigStrings()
{
    const std::filesystem::path configPath =
        std::filesystem::temp_directory_path() / "portal-cod4x-plugin.jsonunescape.test.json";

    {
        std::ofstream configFile(configPath);
        // The subscription key carries a \u0027 escape (an apostrophe). Prior to the fix the decoder
        // dropped the backslash and left a literal "u0027" in the emitted request header.
        configFile
            << "{"
            << "\"ingestBaseUrl\":\"https://example.test/ingest\","
            << "\"ingestSubscriptionKey\":\"sub\\u0027key\","
            << "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\","
            << "\"gameType\":\"CallOfDuty4x\","
            << "\"refreshIntervalSeconds\":120"
            << "}";
    }

    FakeHost host;
    host.CurrentTime = 4000;
    host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] = {
        200,
        "{\"data\":{\"items\":[]}}"};

    portal_cod4x::PluginRuntime runtime(configPath.string());
    Assert(runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7") == 0, "PluginRuntime initialize should succeed");

    for (int i = 0; i < 6; ++i)
    {
        runtime.Tick(host);
    }

    bool sawDecodedKey = false;
    for (const auto& request : host.Requests)
    {
        if (request.Headers.find("Ocp-Apim-Subscription-Key: sub'key") != std::string::npos)
        {
            sawDecodedKey = true;
        }
        Assert(
            request.Headers.find("subu0027key") == std::string::npos,
            "The \\u0027 escape must be decoded to an apostrophe, not left as a literal 'u0027'");
    }

    Assert(sawDecodedKey, "Expected at least one request header to carry the \\u0027-decoded subscription key");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_ServerOriginBanResolvesLegacyIdToCanonicalPuidFromRoster()
{
    const auto configPath = WriteRuntimeConfig("legacy-ban-resolve");
    FakeHost host;
    host.CurrentTime = 4000;

    portal_cod4x::PluginRuntime runtime(configPath.string());
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    // A connected player whose canonical PUID carries non-zero low 24 bits, so recovering the full
    // id requires the live roster rather than just the legacy lower bound.
    const std::uint64_t canonicalPuid = 2310346614574440782ULL;
    const std::uint64_t legacyId = (static_cast<std::uint64_t>(4) << 40) | (canonicalPuid >> 24);

    host.PlayerIds[3] = canonicalPuid;
    host.PlayerNames[3] = "Squish";
    runtime.HandlePlayerConnect(host, 3, "203.0.113.7");
    runtime.HandlePlayerConnected(host, 3);

    // cod4x's OnPlayerAddBan delivers the legacy short id; the plugin must normalise it back to the
    // canonical PUID the portal keys bans on so dumpbanlist agrees with the agent reconcile.
    runtime.HandlePlayerBanAdded(legacyId, "Aimbot", 0, "Squish", -1);

    const std::string dump = runtime.RenderServerBanListDump();
    Assert(
        dump.find("playerid: 2310346614574440782") != std::string::npos,
        "Legacy ban id must be rendered as the canonical PUID recovered from the roster");
    Assert(
        dump.find("playerid: " + std::to_string(legacyId)) == std::string::npos,
        "Legacy short id must not leak into dumpbanlist once resolved");

    // The ban must also be enforceable under the canonical id used by OnPlayerGetBanStatus.
    std::string banMessage;
    Assert(
        runtime.TryGetPlayerBanMessage(canonicalPuid, banMessage),
        "Resolved ban must be enforced under the canonical PUID");

    std::error_code ignoreError;
    std::filesystem::remove(configPath, ignoreError);
}

void Runtime_ServerOriginBanKeepsRawIdWhenPlayerNotConnected()
{
    FakeHost host;
    portal_cod4x::PluginRuntime runtime;
    const int initializeResult = runtime.Initialize(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    Assert(initializeResult == 0, "PluginRuntime initialize should succeed");

    // With no matching connected player the plugin cannot recover the low 24 bits, so it falls back
    // to the id as delivered (no regression versus prior behaviour; the agent reconcile still maps).
    const std::uint64_t legacyId = 4535753900373ULL;
    runtime.HandlePlayerBanAdded(legacyId, "Aimbot", 0, "Squish", -1);

    Assert(
        runtime.RenderServerBanListDump().find("playerid: 4535753900373") != std::string::npos,
        "Unresolvable ban id must fall back to the raw id rather than being dropped");
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

void Runtime_IpObservationsHaveStableRetryIdsAndDistinctChangeIds()
{
    const auto configPath = WriteRuntimeConfig("ip-observation-ids");
    FakeHost host;
    ConfigureTestPlayer(host);
    const std::string ipUrl = "https://example.test/ingest/events/player-ip-resolved";
    host.Responses["POST " + ipUrl] = {503, ""};
    host.Responses["POST https://example.test/ingest/rules/evaluate"] = {200, "{\"matched\":false}"};
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 8; ++i) runtime.Tick(host);
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    for (int i = 0; i < 8; ++i) runtime.Tick(host);
    host.Responses["POST " + ipUrl] = {202, ""};
    host.CurrentTime += 3;
    for (int i = 0; i < 8; ++i) runtime.Tick(host);
    runtime.HandlePlayerConnect(host, 2, "198.51.100.11");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.11");
    for (int i = 0; i < 8; ++i) runtime.Tick(host);

    std::vector<std::string> bodies;
    for (const auto& request : host.Requests)
    {
        if (request.Url == ipUrl) bodies.push_back(request.Body);
    }
    Assert(bodies.size() == 3, "Expected one retry and one distinct changed-IP event");
    const auto messageId = [](const std::string& body) {
        const std::string prefix = "\"messageId\":\"";
        const auto start = body.find(prefix);
        return start == std::string::npos ? std::string{} : body.substr(start + prefix.size(), 36);
    };
    const auto firstId = messageId(bodies[0]);
    Assert(firstId.size() == 36 && messageId(bodies[1]) == firstId && messageId(bodies[2]).size() == 36 &&
        messageId(bodies[2]) != firstId, "Retries must preserve IDs and new observations must receive new IDs");
    Assert(bodies[2].find("\"ipAddress\":\"198.51.100.11\"") != std::string::npos,
        "Changed IP event must contain the new address");
    Assert(std::count_if(host.Requests.begin(), host.Requests.end(), [](const auto& request) {
        return request.Url == "https://example.test/ingest/rules/evaluate";
    }) == 3, "Each distinct IP must be evaluated without duplicating identical callbacks");
    RemoveTestConfig(configPath);
}

void Runtime_ConnectionIdCorrelatesEventsAndRulesAndFailsOpenAcrossLevelChange()
{
    const auto configPath = WriteRuntimeConfig("connection-correlation");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};
    host.Responses["POST https://example.test/ingest/rules/evaluate"] = {200, "{\"matched\":false}"};
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 8; ++i) runtime.Tick(host);

    const auto getConnectionId = [](const std::string& body) {
        const std::string prefix = "\"connectionId\":\"";
        const auto start = body.find(prefix);
        if (start == std::string::npos) return std::string{};
        const auto valueStart = start + prefix.size();
        return body.substr(valueStart, body.find('"', valueStart) - valueStart);
    };
    const auto connect = std::find_if(host.Requests.begin(), host.Requests.end(), [](const auto& request) {
        return request.Url == "https://example.test/ingest/events/player-connected";
    });
    const auto evaluation = std::find_if(host.Requests.begin(), host.Requests.end(), [](const auto& request) {
        return request.Url == "https://example.test/ingest/rules/evaluate";
    });
    Assert(connect != host.Requests.end() && evaluation != host.Requests.end(), "Expected connect and rules requests");
    const auto connectionId = getConnectionId(connect->Body);
    Assert(connectionId.size() == 36 && getConnectionId(evaluation->Body) == connectionId,
        "Connect and evaluation must share a UUID");

    runtime.HandleServerExited(host);
    runtime.HandlePlayerConnected(host, 2);
    runtime.HandlePlayerDisconnected(host, 2);
    for (int i = 0; i < 8; ++i) runtime.Tick(host);
    const auto disconnect = std::find_if(host.Requests.begin(), host.Requests.end(), [](const auto& request) {
        return request.Url == "https://example.test/ingest/events/player-disconnected";
    });
    Assert(disconnect != host.Requests.end() &&
        disconnect->Body.find("\"connectionId\":null") != std::string::npos,
        "Unproven map continuity must not assert the previous connection ID");
    Assert(std::find(host.Logs.begin(), host.Logs.end(),
        "Player connection continuity is unknown after level change; targeted rules effects are unavailable") != host.Logs.end(),
        "Unproven map continuity must be logged");

    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 8; ++i) runtime.Tick(host);
    const auto reconnect = std::find_if(host.Requests.rbegin(), host.Requests.rend(), [](const auto& request) {
        return request.Url == "https://example.test/ingest/events/player-connected";
    });
    Assert(reconnect != host.Requests.rend() && !getConnectionId(reconnect->Body).empty() &&
        getConnectionId(reconnect->Body) != connectionId, "Reconnect must receive a fresh connection ID");
    RemoveTestConfig(configPath);
}

void Runtime_ConnectCallbackAfterLevelExitStartsNewConnection()
{
    const auto configPath = WriteRuntimeConfig("connection-after-exit");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/rules/evaluate"] = {200, "{\"matched\":false}"};
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 8; ++i) runtime.Tick(host);

    runtime.HandleServerExited(host);
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 8; ++i) runtime.Tick(host);
    std::vector<std::string> bodies;
    for (const auto& request : host.Requests)
    {
        if (request.Url == "https://example.test/ingest/events/player-connected")
        {
            bodies.push_back(request.Body);
        }
    }
    Assert(bodies.size() == 2 && bodies[0] != bodies[1],
        "A connect callback after level exit must emit a fresh connection");
    const std::string prefix = "\"connectionId\":\"";
    const auto first = bodies[0].find(prefix);
    const auto second = bodies[1].find(prefix);
    Assert(first != std::string::npos && second != std::string::npos &&
        bodies[0].substr(first + prefix.size(), 36) != bodies[1].substr(second + prefix.size(), 36),
        "A connect callback after level exit must use a new connection ID");
    RemoveTestConfig(configPath);
}

void Runtime_ReconnectAfterLevelExitRetiresPriorState()
{
    for (int scenario = 0; scenario < 2; ++scenario)
    {
        const auto configPath = WriteRuntimeConfig("reconnect-level-" + std::to_string(scenario));
        FakeHost host;
        ConfigureTestPlayer(host);
        host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
        host.Responses["POST https://example.test/ingest/rules/evaluate"] = {200, "{\"matched\":false}"};
        portal_cod4x::PluginRuntime runtime(configPath.string());
        runtime.Initialize(host, "0.2.0");
        runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
        runtime.HandlePlayerConnected(host, 2);
        for (int i = 0; i < 8; ++i) runtime.Tick(host);
        if (scenario == 1) host.PlayerIds.erase(2);
        runtime.HandleServerExited(host);
        host.PlayerIds[2] = 76561198000000001ULL;
        if (scenario == 0)
        {
            host.CurrentTime += 61;
            runtime.Tick(host);
            runtime.HandleClientAuthorized(host);
        }
        runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
        runtime.HandlePlayerConnected(host, 2);
        for (int i = 0; i < 12; ++i) runtime.Tick(host);
        std::vector<std::string> bodies;
        for (const auto& request : host.Requests)
        {
            if (request.Url == "https://example.test/ingest/events/player-connected") bodies.push_back(request.Body);
        }
        const std::string prefix = "\"connectionId\":\"";
        Assert(bodies.size() == 2, "An observed reconnect must emit a second connect event");
        const auto first = bodies[0].find(prefix);
        const auto second = bodies[1].find(prefix);
        Assert(first != std::string::npos && second != std::string::npos &&
            bodies[0].substr(first + prefix.size(), 36) != bodies[1].substr(second + prefix.size(), 36),
            "An observed reconnect must get a fresh correlation ID");
        RemoveTestConfig(configPath);
    }
}

void Runtime_DisconnectAfterLevelExitDoesNotReuseAnotherPlayersIdentity()
{
    const auto configPath = WriteRuntimeConfig("disconnect-replaced-level");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 8; ++i) runtime.Tick(host);
    runtime.HandleServerExited(host);
    host.PlayerIds[2] = 76561198000000002ULL;
    host.PlayerNames[2] = "OtherPlayer";
    runtime.HandlePlayerDisconnected(host, 2);
    for (int i = 0; i < 8; ++i) runtime.Tick(host);
    const auto event = std::find_if(host.Requests.begin(), host.Requests.end(), [](const auto& request) {
        return request.Url == "https://example.test/ingest/events/player-disconnected";
    });
    Assert(event != host.Requests.end() && event->Body.find("\"playerGuid\":\"76561198000000002\"") != std::string::npos &&
        event->Body.find("\"username\":\"OtherPlayer\"") != std::string::npos &&
        event->Body.find("\"connectionId\":null") != std::string::npos,
        "A reused slot disconnect must not be attributed to the prior player");
    RemoveTestConfig(configPath);
}

void Runtime_UnobservedConnectionIsNotAssignedAnAdmissionId()
{
    const auto configPath = WriteRuntimeConfig("connection-unobserved");
    FakeHost host;
    ConfigureTestPlayer(host);
    portal_cod4x::PluginRuntime runtime(configPath.string());
    runtime.Initialize(host, "0.2.0");
    runtime.HandlePlayerConnected(host, 2);
    for (int i = 0; i < 8; ++i) runtime.Tick(host);
    const auto connect = std::find_if(host.Requests.begin(), host.Requests.end(), [](const auto& request) {
        return request.Url == "https://example.test/ingest/events/player-connected";
    });
    Assert(connect != host.Requests.end() && connect->Body.find("\"connectionId\":null") != std::string::npos,
        "A player already present when the plugin starts must not receive an admission ID");
    Assert(std::find(host.Logs.begin(), host.Logs.end(),
        "Player connection was not observed; targeted rules effects are unavailable") != host.Logs.end(),
        "Unobserved connections must be logged");
    RemoveTestConfig(configPath);
}

}

namespace
{
using portal_cod4x::RulesJson;
using portal_cod4x::RulesJsonReader;

std::string RulesReply(std::string commands = {}, std::string executionIds = {}, bool retryTrigger = false)
{
    return "{\"protocolVersion\":1,\"commands\":[" + commands + "],\"executionIds\":[" + executionIds +
        "],\"retryTrigger\":" + (retryTrigger ? "true" : "false") + "}";
}

RulesJson CommandForTrigger(const RulesJson& trigger, std::string_view type = "DropPlayer",
    std::string_view executionId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
{
    const auto quote = RulesJson::Quote;
    const auto correlation = "{\"triggerId\":" + quote(*trigger.String("triggerId")) +
        ",\"triggerOccurrenceId\":" + quote(*trigger.String("triggerOccurrenceId")) +
        ",\"connectionId\":" + trigger.Find("connectionId")->Dump() +
        ",\"featureId\":\"vpn-protection\",\"ruleId\":\"high-risk\"" +
        ",\"ruleSetRevision\":\"1\",\"actionId\":\"drop-player\"}";
    auto command = *RulesJsonReader::Parse(
        "{\"protocolVersion\":1,\"executionId\":" + quote(executionId) +
        ",\"actionId\":\"drop-player\",\"logicalEffectId\":" + quote(executionId) + ",\"correlation\":" + correlation +
        ",\"gameServerId\":" + trigger.Find("gameServerId")->Dump() + ",\"slotId\":" + trigger.Find("slotId")->Dump() +
        ",\"playerGuid\":" + trigger.Find("playerGuid")->Dump() +
        ",\"connectionId\":" + trigger.Find("connectionId")->Dump() + ",\"connectionGeneration\":" +
        trigger.Find("connectionGeneration")->Dump() + ",\"runtime\":" + trigger.Find("runtime")->Dump() +
        ",\"expiresAtUtc\":\"1970-01-01T01:30:00Z\",\"mode\":\"Enforce\",\"effect\":{\"type\":" + quote(type) +
        ",\"reason\":\"Rules kick\",\"message\":\"Rules message\"}}");
    if (type == "PrivateMessage")
    {
        command.Fields["deliveryIdentity"] = *RulesJsonReader::Parse(
            "{\"parentLogicalEffectId\":\"" + std::string(64, 'a') + "\",\"messageIndex\":0,\"fragmentIndex\":0}");
    }
    return command;
}

void TickFrames(portal_cod4x::PluginRuntime& runtime, FakeHost& host, int frames = 8)
{
    for (int i = 0; i < frames; ++i) runtime.Tick(host);
}

void Runtime_RulesTriggerCapabilitiesAndNativeEffects()
{
    const auto config = WriteRuntimeConfig("rules-native", "^4[Local Only]^7");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.ResponseFactory = [](const auto& request) -> portal_cod4x::HttpResponse {
        if (request.Url.ends_with("/evaluate"))
        {
            const auto trigger = *RulesJsonReader::Parse(request.Body);
            return {200, RulesReply(
                CommandForTrigger(trigger, "PublicMessage").Dump() + "," +
                CommandForTrigger(trigger, "PrivateMessage", "aaaaaaaa-bbbb-cccc-dddd-000000000001").Dump() + "," +
                CommandForTrigger(trigger, "DropPlayer", "aaaaaaaa-bbbb-cccc-dddd-000000000002").Dump())};
        }
        return {200, RulesReply()};
    };
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.123");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    TickFrames(runtime, host, 12);
    Assert(host.DroppedPlayers.size() == 1 && host.DroppedPlayers[0].Slot == 2 &&
        host.DroppedPlayers[0].Reason == "Rules kick", "Typed drop must use direct native API");
    Assert(host.BroadcastMessages.back() == "Rules message" &&
        host.PrivateMessages.size() == 1 && host.PrivateMessages[0].Slot == 2 &&
        host.PrivateMessages[0].Message == "Rules message",
        "Native rule messages must remain unchanged even when a local agentName is configured");
    Assert(host.ExecutedCommands.empty(), "Rules effects must never enqueue server command text");
    const auto evaluate = std::find_if(host.Requests.begin(), host.Requests.end(), [](const auto& request) {
        return request.Url.ends_with("/rules/evaluate");
    });
    const auto trigger = *RulesJsonReader::Parse(evaluate->Body);
    Assert(trigger.Integer("protocolVersion") == 1 && trigger.String("triggerId") == "player.connected" &&
               trigger.String("gameType") == "CallOfDuty4x" &&
               trigger.String("observationId") == trigger.String("triggerOccurrenceId") &&
               trigger.Find("runtime")->String("version") == "0.3.123" &&
               trigger.Find("runtime")->String("identityModel") == "PluginRuntimeGeneration" &&
               trigger.Find("runtime")->Find("capabilities")->Items.size() == 5,
           "Trigger must report actual version, stable correlation and only real native capabilities");
    const auto connect = std::find_if(host.Requests.begin(), host.Requests.end(), [](const auto& request) {
        return request.Url.ends_with("/events/player-connected");
    });
    Assert(connect != host.Requests.end() &&
        connect->Body.find(*trigger.String("observationId")) != std::string::npos &&
        connect->Body.find("\"rulesEvaluatedByPlugin\":true") != std::string::npos,
        "Mirrored connect event and trigger must share observation identity and ownership metadata");
    Assert(std::count_if(host.Requests.begin(), host.Requests.end(), [&](const auto& request) {
        return request.Url.ends_with("/rules/outcomes") &&
            request.Body.find("\"outcome\":\"Dispatched\"") != std::string::npos &&
            request.Body.find("\"gameServerId\":" + trigger.Find("gameServerId")->Dump()) != std::string::npos &&
            request.Body.find("\"correlation\":") != std::string::npos;
    }) == 3, "Each native action must report a correlated unconfirmed dispatch outcome");
    RemoveTestConfig(config);
}

void ChatMessages_PreserveBoundariesAndRejectInvalidBatches()
{
    using portal_cod4x::PrepareChatMessages;
    const auto exact = PrepareChatMessages({std::string(149, 'a')});
    Assert(exact && exact->size() == 1 && exact->front().size() == 149, "Exact chat budget must not split");
    const auto words = PrepareChatMessages({std::string(140, 'a') + " two words after", "last"});
    Assert(words && *words == std::vector<std::string>{std::string(140, 'a') + " two ", "words after", "last"},
           "Word splitting must preserve spaces and logical item order");
    const std::string unicode = "\xf0\x9f\x98\x80";
    const auto utf8 = PrepareChatMessages({std::string(148, 'a') + unicode});
    Assert(utf8 && *utf8 == std::vector<std::string>{std::string(148, 'a'), unicode},
           "UTF8 codepoints must never be split");
    const auto colours = PrepareChatMessages({std::string(148, 'a') + "^1" + std::string(149, 'b')});
    Assert(colours && *colours == std::vector<std::string>{std::string(148, 'a'), "^1" + std::string(147, 'b'), "^1bb"},
           "Colour codes must be atomic and carried within budget");
    const auto longWord = PrepareChatMessages({std::string(300, 'x')});
    Assert(longWord && *longWord == std::vector<std::string>{std::string(149, 'x'), std::string(149, 'x'), "xx"},
           "Unbroken text must split without loss or empty trailing fragments");
    for (const auto& invalid : std::vector<std::string>{"", "bad\ntext", "bad\"text", std::string("bad\xed\xa0\x80"),
                                                        std::string("bad\xc0\xaf"), std::string("bad\xf4\x90\x80\x80")})
    {
        Assert(!PrepareChatMessages({"valid", invalid}),
               "Whole local batch must fail before any fragments are returned");
    }
}

void Runtime_RulesRecipientMessagesSurviveTriggerDisconnect()
{
    for (int scenario = 0; scenario < 17; ++scenario)
    {
        const auto config = WriteRuntimeConfig("rules-recipient-" + std::to_string(scenario));
        FakeHost host;
        ConfigureTestPlayer(host);
        host.PlayerIds[3] = 76561198000000002ULL;
        host.PlayerNames[3] = "Recipient";
        std::optional<RulesJson> recipient;
        std::optional<RulesJson> original;
        auto recipientCommand = [&](const RulesJson& trigger) {
            auto command = CommandForTrigger(trigger,
                                             scenario == 5   ? "DropPlayer"
                                             : scenario == 6 ? "PublicMessage"
                                                             : "PrivateMessage",
                                             "aaaaaaaa-bbbb-cccc-dddd-000000000001");
            auto addressed = *recipient;
            if (scenario == 1)
                addressed.Fields["connectionGeneration"] = *RulesJsonReader::Parse("999");
            if (scenario == 2)
                addressed.Fields["playerGuid"] = *RulesJsonReader::Parse("\"123\"");
            if (scenario == 3)
                addressed.Fields["gameServerId"] = *RulesJsonReader::Parse("\"00000000-2222-3333-4444-555555555555\"");
            if (scenario == 4)
                addressed.Fields["runtime"].Fields["instanceId"] =
                    *RulesJsonReader::Parse("\"00000000-2222-3333-4444-555555555555\"");
            if (scenario == 7)
                addressed.Fields.erase("connectionGeneration");
            command.Fields["recipientTarget"] = addressed;
            command.Fields["deliveryIdentity"] =
                *RulesJsonReader::Parse("{\"parentLogicalEffectId\":\"" + std::string(64, 'a') +
                                        "\",\"messageIndex\":0,\"fragmentIndex\":0,"
                                        "\"recipientConnectionId\":" +
                                        recipient->Find("connectionId")->Dump() + "}");
            if (scenario == 10)
                command.Fields["expiresAtUtc"] = *RulesJsonReader::Parse("\"1970-01-01T01:00:00Z\"");
            if (scenario == 11)
                command.Fields["deliveryIdentity"].Fields["fragmentIndex"] = *RulesJsonReader::Parse("-1");
            if (scenario == 12)
                command.Fields["deliveryIdentity"].Fields["recipientConnectionId"] =
                    *RulesJsonReader::Parse("\"00000000-2222-3333-4444-555555555555\"");
            if (scenario == 13)
                command.Fields["slotId"] = *RulesJsonReader::Parse("3");
            if (scenario == 14)
                command.Fields["effect"].Fields["message"] =
                    *RulesJsonReader::Parse(RulesJson::Quote(std::string(150, 'x')));
            if (scenario == 15)
                command.Fields["deliveryIdentity"].Fields["parentLogicalEffectId"] =
                    *RulesJsonReader::Parse("\"invalid-parent\"");
            return command;
        };
        host.ResponseFactory = [&](const auto& request) -> portal_cod4x::HttpResponse {
            if (request.Url.ends_with("/evaluate"))
            {
                const auto trigger = *RulesJsonReader::Parse(request.Body);
                if (trigger.Integer("slotId") == 3)
                {
                    if (!recipient)
                        recipient = trigger;
                    return {200, RulesReply()};
                }
                original = trigger;
                return {200, RulesReply(CommandForTrigger(trigger).Dump(), "\"aaaaaaaa-bbbb-cccc-dddd-000000000001\"")};
            }
            if (request.Url.ends_with("/poll") && original)
            {
                Assert(RulesJsonReader::Parse(request.Body)->Find("runtime") != nullptr,
                       "Disconnected-trigger poll must include the current executor descriptor");
                auto command = recipientCommand(*original);
                std::string commands = command.Dump();
                if (scenario == 9 || scenario == 16)
                {
                    command.Fields["logicalEffectId"] = *RulesJsonReader::Parse("\"second-fragment\"");
                    command.Fields["deliveryIdentity"].Fields["fragmentIndex"] = *RulesJsonReader::Parse("1");
                    commands += "," + command.Dump();
                }
                return {200, RulesReply(commands)};
            }
            return {200, RulesReply()};
        };
        portal_cod4x::PluginRuntime runtime(config.string());
        runtime.Initialize(host, "0.3.0");
        runtime.HandlePlayerConnect(host, 3, "198.51.100.11");
        runtime.HandlePlayerConnected(host, 3);
        TickFrames(runtime, host);
        host.OnDrop = [&](int slot) {
            runtime.HandlePlayerDisconnected(host, slot);
            host.PlayerIds.erase(slot);
            if (scenario == 8)
            {
                runtime.HandlePlayerDisconnected(host, 3);
                runtime.HandlePlayerConnect(host, 3, "198.51.100.11");
                runtime.HandlePlayerConnected(host, 3);
            }
        };
        if (scenario == 9)
        {
            host.OnChat = [&](int slot) {
                runtime.HandlePlayerDisconnected(host, slot);
                host.PlayerIds.erase(slot);
            };
        }
        if (scenario == 16)
        {
            host.OnChat = [&](int) { host.CurrentTime = 5400; };
        }
        runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
        runtime.HandlePlayerConnected(host, 2);
        TickFrames(runtime, host);
        host.CurrentTime += 2;
        TickFrames(runtime, host, 20);
        const auto expected = scenario == 0 || scenario == 9 || scenario == 16 ? 1U : 0U;
        Assert(host.PrivateMessages.size() == expected && (expected == 0 || host.PrivateMessages.front().Slot == 3),
               "Recipient fragments must verify their own connection after trigger disconnect and between fragments");
        Assert(host.DroppedPlayers.size() == 1 && host.BroadcastMessages.size() == 1,
               "Recipient metadata must never retarget moderation or broadcast effects");
        RemoveTestConfig(config);
    }
}

void Runtime_RulesRejectUnsafeOrStaleCommandsIndependently()
{
    for (int scenario = 0; scenario < 26; ++scenario)
    {
        const auto config = WriteRuntimeConfig("rules-invalid-" + std::to_string(scenario));
        FakeHost host;
        ConfigureTestPlayer(host);
        host.ResponseFactory = [scenario](const auto& request) -> portal_cod4x::HttpResponse {
            if (!request.Url.ends_with("/evaluate")) return {200, RulesReply()};
            const auto trigger = *RulesJsonReader::Parse(request.Body);
            auto command = CommandForTrigger(trigger);
            auto set = [&](std::string key, std::string value) {
                command.Fields[std::move(key)] = *RulesJsonReader::Parse(value);
            };
            if (scenario == 0) set("mode", "\"DryRun\"");
            if (scenario == 1) set("mode", "\"enforce\"");
            if (scenario == 2) command.Fields.erase("mode");
            if (scenario == 3) set("gameServerId", "\"00000000-2222-3333-4444-555555555555\"");
            if (scenario == 4) set("slotId", "3");
            if (scenario == 5) set("playerGuid", "\"76561198000000002\"");
            if (scenario == 6) set("connectionGeneration", "2");
            if (scenario == 7) set("connectionId", "\"00000000-2222-3333-4444-555555555555\"");
            if (scenario == 8) set("expiresAtUtc", "\"1970-01-01T01:00:00Z\"");
            if (scenario == 9) set("expiresAtUtc", "\"2026-02-31T12:00:00Z\"");
            if (scenario == 10) set("protocolVersion", "2");
            if (scenario == 11)
                command.Fields["effect"].Fields["reason"] = *RulesJsonReader::Parse("\"bad\\u0000reason\"");
            if (scenario == 12)
                command.Fields["effect"].Fields["type"] = *RulesJsonReader::Parse("\"NativeTempBan\"");
            if (scenario == 13) set("connectionGeneration", "18446744073709551616");
            if (scenario == 14)
                command.Fields["runtime"].Fields["id"] = *RulesJsonReader::Parse("\"rcon-agent\"");
            if (scenario == 15)
                command.Fields["runtime"].Fields["instanceId"] =
                    *RulesJsonReader::Parse("\"00000000-2222-3333-4444-555555555555\"");
            if (scenario == 16) set("runtime", "\"cod4x-plugin\"");
            if (scenario == 17)
                command.Fields["correlation"].Fields["featureId"] = *RulesJsonReader::Parse("1");
            if (scenario == 18)
                command.Fields["correlation"].Fields["ruleId"] = *RulesJsonReader::Parse("\"\"");
            if (scenario == 19)
                command.Fields["correlation"].Fields["triggerOccurrenceId"] = *RulesJsonReader::Parse("\"invalid\"");
            if (scenario == 20)
                command.Fields["correlation"].Fields["siegeSessionId"] = *RulesJsonReader::Parse("\"invalid\"");
            if (scenario == 21)
                command.Fields["runtime"].Fields["identityModel"] =
                    *RulesJsonReader::Parse("\"AgentObservedConnection\"");
            if (scenario == 22)
                command.Fields["correlation"].Fields["triggerId"] = *RulesJsonReader::Parse("\"player.ip-resolved\"");
            if (scenario == 23)
                command.Fields["correlation"].Fields["triggerOccurrenceId"] =
                    *RulesJsonReader::Parse("\"00000000-2222-3333-4444-555555555555\"");
            if (scenario == 24) set("connectionGeneration", "null");
            if (scenario == 25) command.Fields.erase("runtime");
            // An unsupported or invalid action must not prevent an independent supported action.
            return {200, RulesReply(command.Dump() + "," +
                CommandForTrigger(trigger, "PrivateMessage", "aaaaaaaa-bbbb-cccc-dddd-000000000001").Dump())};
        };
        portal_cod4x::PluginRuntime runtime(config.string());
        runtime.Initialize(host, "0.3.0");
        host.CurrentSubsecondTicks = 42;
        runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
        runtime.HandlePlayerConnected(host, 2);
        TickFrames(runtime, host, 10);
        Assert(host.DroppedPlayers.empty() && host.ExecutedCommands.empty(),
            "Dry run, invalid, expired, unsupported and stale commands must never drop players");
        Assert(host.PrivateMessages.size() == 1, "Invalid command must not suppress independent supported work");
        if (scenario == 0 || scenario == 12)
        {
            const auto expected = scenario == 0 ? "\"outcome\":\"WouldExecute\"" :
                "\"outcome\":\"SkippedUnsupported\"";
            Assert(std::any_of(host.Requests.begin(), host.Requests.end(), [&](const auto& request) {
                return request.Url.ends_with("/outcomes") && request.Body.find(expected) != std::string::npos &&
                    request.Body.find("\"occurredAtUtc\":\"1970-01-01T01:23:20.0000042Z\"") != std::string::npos;
            }), "Dry run and unsupported effects must report explicit outcomes");
        }
        RemoveTestConfig(config);
    }
}

void Runtime_RulesRechecksConnectionAndSupportsReentrantDisconnect()
{
    for (int scenario = 0; scenario < 5; ++scenario)
    {
        const auto config = WriteRuntimeConfig("rules-reentrant-" + std::to_string(scenario));
        FakeHost host;
        ConfigureTestPlayer(host);
        host.ResponseFactory = [](const auto& request) -> portal_cod4x::HttpResponse {
            if (!request.Url.ends_with("/evaluate")) return {200, RulesReply()};
            const auto trigger = *RulesJsonReader::Parse(request.Body);
            return {200, RulesReply(CommandForTrigger(trigger).Dump() + "," +
                CommandForTrigger(trigger, "PrivateMessage", "aaaaaaaa-bbbb-cccc-dddd-000000000001").Dump())};
        };
        portal_cod4x::PluginRuntime runtime(config.string());
        runtime.Initialize(host, "0.3.0");
        runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
        runtime.HandlePlayerConnected(host, 2);
        runtime.Tick(host);
        if (scenario == 0) host.PlayerIds[2] = 76561198000000002ULL;
        if (scenario == 1)
        {
            runtime.HandlePlayerDisconnected(host, 2);
            runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
            runtime.HandlePlayerConnected(host, 2);
        }
        if (scenario == 2) runtime.HandleServerExited(host);
        if (scenario == 3) runtime.HandlePlayerConnect(host, 2, "198.51.100.11");
        if (scenario == 4) host.OnDrop = [&](int slot) { runtime.HandlePlayerDisconnected(host, slot); };
        runtime.Tick(host);
        Assert(host.DroppedPlayers.size() == (scenario == 4 ? 1U : 0U),
            "In-flight responses must recheck GUID, generation, map continuity and IP revision");
        Assert(host.PrivateMessages.empty(), "Synchronous disconnect must invalidate subsequent native targets safely");
        RemoveTestConfig(config);
    }
}

void Runtime_RulesOutcomesRetryAndCloudContinuationPolls()
{
    const auto config = WriteRuntimeConfig("rules-continuation");
    FakeHost host;
    ConfigureTestPlayer(host);
    RulesJson trigger;
    int outcomes = 0;
    int polls = 0;
    host.ResponseFactory = [&](const auto& request) -> portal_cod4x::HttpResponse {
        if (request.Url.ends_with("/evaluate"))
        {
            trigger = *RulesJsonReader::Parse(request.Body);
            return {200, RulesReply(CommandForTrigger(trigger, "PrivateMessage").Dump(),
                "\"bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee\"")};
        }
        if (request.Url.ends_with("/outcomes"))
        {
            ++outcomes;
            if (outcomes == 1) return {503, ""};
            return {200, RulesReply({}, "\"bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee\"")};
        }
        ++polls;
        Assert(request.Body.find("\"connectionGeneration\":1") != std::string::npos &&
            request.Body.find("\"executionIds\":[\"bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee\"]") != std::string::npos,
            "Continuation poll must retain execution identity and complete target snapshot");
        return {200, RulesReply(CommandForTrigger(trigger, "PrivateMessage").Dump() + "," +
            CommandForTrigger(trigger, "PublicMessage", "aaaaaaaa-bbbb-cccc-dddd-000000000001").Dump())};
    };
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    TickFrames(runtime, host);
    Assert(host.PrivateMessages.size() == 1 && outcomes == 1, "First native effect executes before outcome retry");
    host.CurrentTime += 5;
    TickFrames(runtime, host, 16);
    Assert(outcomes >= 2 && polls >= 1, "Outcome failure must retry while cloud continuation is polled");
    Assert(host.PrivateMessages.size() == 1 && host.BroadcastMessages.size() == 2,
        "Repeated remote command must not repeat native effect; next typed action must execute");
    RemoveTestConfig(config);
}

void RulesJson_RejectsAmbiguousExecutablePayloads()
{
    for (const std::string json : {
        "{\"commands\":[],\"commands\":[]}", "{\"commands\":[{\"mode\":\"Enforce\",\"mode\":\"DryRun\"}]}",
        "{\"n\":1.}", "{\"n\":01}", "{\"n\":1e}", "{\"a\":\"\\x00\"}", "{\"a\":\"\\ud800\"}",
        "{\"a\":\"\\udc00\"}", "[1,]", "{\"x\":true} trailing"})
        Assert(!RulesJsonReader::Parse(json), "Ambiguous or malformed JSON must be rejected before native dispatch");
    const auto unicode = RulesJsonReader::Parse("{\"a\":\"\\ud83d\\ude00\",\"b\":18446744073709551615}");
    Assert(unicode && unicode->String("a") == "\xf0\x9f\x98\x80" &&
        unicode->Unsigned("b") == UINT64_MAX, "Valid Unicode and uint64 generation must round-trip without truncation");
    const auto fraction = RulesJsonReader::Parse("{\"slotId\":1.0,\"connectionGeneration\":1e3}");
    Assert(fraction && !fraction->Integer("slotId") && !fraction->Unsigned("connectionGeneration"),
        "Numeric target fields must reject floating-point coercion even in syntactically valid JSON");
    for (const std::string bytes : {"\x80", "\xff", "\xc0\xaf", "\xc2", "\xe0\x80\x80", "\xed\xa0\x80",
        "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xe2\x28\xa1"})
        Assert(!RulesJsonReader::Parse("{\"text\":\"" + bytes + "\"}"),
            "Malformed raw UTF-8 must not reach native APIs through JSON strings");
    Assert(RulesJsonReader::Parse("{\"text\":\"\xc2\xa3\xe2\x82\xac\xf0\x9f\x98\x80\"}").has_value(),
        "Valid two-, three- and four-byte raw UTF-8 must remain accepted");
}

void Runtime_RulesOutcomeResponseContinuesAndMalformedAckRetries()
{
    for (int malformed = 0; malformed < 4; ++malformed)
    {
        const auto config = WriteRuntimeConfig("rules-outcome-response");
        FakeHost host;
        ConfigureTestPlayer(host);
        RulesJson trigger;
        int acknowledgements = 0;
        host.ResponseFactory = [&](const auto& request) -> portal_cod4x::HttpResponse {
            if (request.Url.ends_with("/evaluate"))
            {
                trigger = *RulesJsonReader::Parse(request.Body);
                return {200, RulesReply(CommandForTrigger(trigger, "PrivateMessage").Dump())};
            }
            ++acknowledgements;
            if (acknowledgements == 1 && malformed == 3)
                return {200, R"({"protocolVersion":1,"commands":[],"executionIds":[],"retryTrigger":"true"})"};
            if (acknowledgements == 1)
                return {200,
                        malformed == 0 ? "{\"commands\":[]}" : RulesReply({}, malformed == 1 ? "null" : "\"invalid\"")};
            if (acknowledgements == 2)
                return {
                    200,
                    RulesReply(
                        CommandForTrigger(trigger, "PublicMessage", "aaaaaaaa-bbbb-cccc-dddd-000000000001").Dump())};
            return {200, RulesReply()};
        };
        portal_cod4x::PluginRuntime runtime(config.string());
        runtime.Initialize(host, "0.3.0");
        runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
        runtime.HandlePlayerConnected(host, 2);
        TickFrames(runtime, host);
        Assert(host.PrivateMessages.size() == 1 && host.BroadcastMessages.size() == 1,
               "Malformed successful HTTP response must not acknowledge outcome or invoke a next action");
        host.CurrentTime += 5;
        TickFrames(runtime, host);
        Assert(acknowledgements >= 3 && host.PrivateMessages.size() == 1 && host.BroadcastMessages.size() == 2,
               "Retried outcome must advance next native action without repeating the prior effect");
        runtime.HandlePlayerConnect(host, 2, "198.51.100.11");
        runtime.Tick(host);
        Assert(host.OpenRequests > 0, "Test must leave real asynchronous requests in flight");
        runtime.Shutdown(host);
        Assert(host.OpenRequests == 0, "Unloading must release all in-flight HTTP handles before module removal");
        RemoveTestConfig(config);
    }
}

void Runtime_RulesCallbackOrderingPreservesConnectedObservation()
{
    for (int scenario = 0; scenario < 4; ++scenario)
    {
        const auto config = WriteRuntimeConfig("rules-callback-order-" + std::to_string(scenario));
        FakeHost host;
        ConfigureTestPlayer(host);
        host.ResponseFactory = [](const auto& request) -> portal_cod4x::HttpResponse {
            if (!request.Url.ends_with("/evaluate")) return {200, RulesReply()};
            const auto trigger = *RulesJsonReader::Parse(request.Body);
            return {200, RulesReply(trigger.String("triggerId") == "player.connected" ?
                CommandForTrigger(trigger, "PrivateMessage").Dump() : "")};
        };
        portal_cod4x::PluginRuntime runtime(config.string());
        runtime.Initialize(host, "0.3.0");
        runtime.HandlePlayerConnect(host, 2, scenario == 1 ? "" : "198.51.100.10");
        if (scenario == 0 || scenario == 3)
        {
            runtime.HandleClientAuthorized(host);
            runtime.Tick(host);
        }
        if (scenario == 3) runtime.HandlePlayerConnect(host, 2, "198.51.100.11");
        runtime.HandlePlayerConnected(host, 2);
        if (scenario == 1 || scenario == 2) runtime.HandlePlayerConnect(host, 2, "198.51.100.11");
        TickFrames(runtime, host, 16);
        std::string connectedObservation;
        std::string ipObservation;
        for (const auto& request : host.Requests)
        {
            if (!request.Url.ends_with("/evaluate")) continue;
            const auto trigger = *RulesJsonReader::Parse(request.Body);
            if (trigger.String("triggerId") == "player.connected")
            {
                connectedObservation = *trigger.String("observationId");
                if (scenario == 1 || scenario == 2)
                    Assert(trigger.String("playerIpAddress") == "198.51.100.11",
                        "A queued connected observation must be enriched rather than discarded on IP change");
            }
            else ipObservation = *trigger.String("observationId");
        }
        const auto connect = std::find_if(host.Requests.begin(), host.Requests.end(), [](const auto& request) {
            return request.Url.ends_with("/events/player-connected");
        });
        Assert(!connectedObservation.empty() && connect != host.Requests.end() &&
            connect->Body.find(connectedObservation) != std::string::npos,
            "Authorization and enter-world paths must share one stable connected observation");
        Assert(scenario == 0 || (!ipObservation.empty() && ipObservation != connectedObservation),
            "IP resolution and connected subscriptions require distinct observation identities");
        Assert(host.PrivateMessages.size() == (scenario == 3 ? 0U : 1U),
            "Queued connected work must run, but an in-flight pre-IP-change command must stay stale");
        RemoveTestConfig(config);
    }
}

void Runtime_RulesRejectPreviousObservationAfterIpChange()
{
    const auto config = WriteRuntimeConfig("rules-old-observation");
    FakeHost host;
    ConfigureTestPlayer(host);
    RulesJson oldCommand;
    int evaluations = 0;
    host.ResponseFactory = [&](const auto& request) -> portal_cod4x::HttpResponse {
        if (!request.Url.ends_with("/evaluate")) return {200, RulesReply()};
        const auto trigger = *RulesJsonReader::Parse(request.Body);
        if (++evaluations == 1)
        {
            oldCommand = CommandForTrigger(trigger);
            return {200, RulesReply()};
        }
        return {200, RulesReply(oldCommand.Dump() + "," +
            CommandForTrigger(trigger, "PrivateMessage", "aaaaaaaa-bbbb-cccc-dddd-000000000001").Dump())};
    };
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    TickFrames(runtime, host);
    runtime.HandlePlayerConnect(host, 2, "198.51.100.11");
    TickFrames(runtime, host, 12);
    Assert(evaluations == 2 && host.DroppedPlayers.empty() && host.PrivateMessages.size() == 1,
        "An old correlated decision must not execute through a fresh IP observation");
    RemoveTestConfig(config);
}

void Runtime_RulesRedeliveryRetainsDispatchedAfterDisconnect()
{
    const auto config = WriteRuntimeConfig("rules-redelivered-drop");
    FakeHost host;
    ConfigureTestPlayer(host);
    RulesJson trigger;
    int outcomes = 0;
    host.ResponseFactory = [&](const auto& request) -> portal_cod4x::HttpResponse {
        if (request.Url.ends_with("/evaluate"))
        {
            trigger = *RulesJsonReader::Parse(request.Body);
            return {200, RulesReply(CommandForTrigger(trigger).Dump())};
        }
        if (request.Url.ends_with("/outcomes"))
        {
            ++outcomes;
            const auto result = *RulesJsonReader::Parse(request.Body);
            Assert(result.String("outcome") == "Dispatched",
                "Redelivery must preserve the original native outcome after synchronous disconnect");
            Assert(result.String("occurredAtUtc") == "1970-01-01T01:23:20.1234567Z",
                "The actual subsecond dispatch timestamp must survive later redelivery");
            return {200, RulesReply({}, outcomes == 1 ? "\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"" : "")};
        }
        return {200, RulesReply(CommandForTrigger(trigger).Dump())};
    };
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.0");
    host.CurrentSubsecondTicks = 1234000;
    host.OnDrop = [&](int slot) {
        host.CurrentSubsecondTicks = 1234567;
        runtime.HandlePlayerDisconnected(host, slot);
    };
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    TickFrames(runtime, host);
    host.CurrentTime += 3;
    TickFrames(runtime, host);
    Assert(outcomes == 2 && host.DroppedPlayers.size() == 1,
        "An acknowledged drop may be reported again but must never execute twice");
    RemoveTestConfig(config);
}

void Runtime_RulesPartialStartsRetryOriginalTriggerWithoutReplayingEffects()
{
    const auto config = WriteRuntimeConfig("rules-partial-start");
    FakeHost host;
    ConfigureTestPlayer(host);
    std::string originalBody;
    int evaluations = 0;
    int polls = 0;
    host.ResponseFactory = [&](const auto& request) -> portal_cod4x::HttpResponse {
        if (request.Url.ends_with("/evaluate"))
        {
            ++evaluations;
            if (originalBody.empty()) originalBody = request.Body;
            Assert(request.Body == originalBody, "Partial-start retries must preserve the exact original trigger");
            if (evaluations == 2) return {503, ""};
            const auto trigger = *RulesJsonReader::Parse(request.Body);
            return {200, RulesReply(CommandForTrigger(trigger, "PrivateMessage").Dump() +
                (evaluations == 3 ? "," + CommandForTrigger(trigger, "PublicMessage",
                    "aaaaaaaa-bbbb-cccc-dddd-000000000001").Dump() : ""),
                evaluations < 3 ? "\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"" : "", evaluations < 3)};
        }
        if (request.Url.ends_with("/poll"))
        {
            ++polls;
            return {200, RulesReply({}, evaluations < 3 ? "\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"" : "")};
        }
        return {200, RulesReply({}, evaluations < 3 ? "\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"" : "")};
    };
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    TickFrames(runtime, host, 12);
    Assert(evaluations == 1 && host.PrivateMessages.size() == 1,
        "A missing provider start must not suppress healthy native work");
    host.CurrentTime += 4;
    TickFrames(runtime, host, 12);
    Assert(evaluations == 1 && polls > 0, "Healthy continuation must proceed during the trigger retry backoff");
    host.CurrentTime += 1;
    TickFrames(runtime, host, 12);
    Assert(evaluations == 2 && host.PrivateMessages.size() == 1,
        "Trigger retry must use five-second backoff and survive a transient retry failure");
    host.CurrentTime += 5;
    TickFrames(runtime, host, 20);
    Assert(evaluations == 3 && host.PrivateMessages.size() == 1 && host.BroadcastMessages.size() == 2,
        "Recovered missing work must execute without replaying the healthy effect");
    host.CurrentTime += 5;
    TickFrames(runtime, host, 12);
    Assert(evaluations == 3, "retryTrigger=false on evaluate must stop trigger retries");
    RemoveTestConfig(config);
}

void Runtime_RulesTriggerRetriesExpireAndRejectChangedConnections()
{
    for (int scenario = 0; scenario < 4; ++scenario)
    {
        const auto config = WriteRuntimeConfig("rules-retry-expiry-" + std::to_string(scenario));
        FakeHost host;
        ConfigureTestPlayer(host);
        std::string originalBody;
        int originalAttempts = 0;
        host.ResponseFactory = [&](const auto& request) -> portal_cod4x::HttpResponse {
            if (!request.Url.ends_with("/evaluate")) return {200, RulesReply()};
            if (originalBody.empty()) originalBody = request.Body;
            if (request.Body != originalBody) return {200, RulesReply()};
            ++originalAttempts;
            return {200, RulesReply({}, {}, true)};
        };
        portal_cod4x::PluginRuntime runtime(config.string());
        runtime.Initialize(host, "0.3.0");
        runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
        runtime.HandlePlayerConnected(host, 2);
        TickFrames(runtime, host);
        if (scenario == 1)
        {
            host.CurrentTime += 115;
            TickFrames(runtime, host);
        }
        if (scenario == 2) runtime.HandlePlayerConnect(host, 2, "198.51.100.11");
        if (scenario == 3)
        {
            runtime.HandlePlayerDisconnected(host, 2);
            runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
            runtime.HandlePlayerConnected(host, 2);
        }
        host.CurrentTime += scenario == 0 ? 120 : 5;
        TickFrames(runtime, host, 12);
        Assert(originalAttempts == (scenario == 1 ? 2 : 1),
            "Retry lifetime must not reset, and stale connection/IP snapshots must never be resubmitted");
        const auto expected = scenario < 2 ? "Rules trigger retry expired" : "Rules trigger retry invalidated";
        Assert(std::any_of(host.Logs.begin(), host.Logs.end(), [&](const auto& log) {
            return log.find(expected) != std::string::npos;
        }), "Abandoned trigger recovery must emit an explicit expiry or invalidation diagnostic");
        RemoveTestConfig(config);
    }
}

void Runtime_RulesShutdownDiagnosesAbandonedTriggerRecovery()
{
    for (int frames : {1, 8})
    {
        const auto config = WriteRuntimeConfig("rules-shutdown-trigger-" + std::to_string(frames));
        FakeHost host;
        ConfigureTestPlayer(host);
        host.ResponseFactory = [](const auto&) -> portal_cod4x::HttpResponse {
            return {200, RulesReply({}, {}, true)};
        };
        portal_cod4x::PluginRuntime runtime(config.string());
        runtime.Initialize(host, "0.3.0");
        runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
        runtime.HandlePlayerConnected(host, 2);
        TickFrames(runtime, host, frames);
        runtime.Shutdown(host);
        Assert(host.OpenRequests == 0 && std::any_of(host.Logs.begin(), host.Logs.end(), [](const auto& log) {
            return log.find("unresolved rules triggers/outcomes/continuations") != std::string::npos;
        }), "Shutdown must close requests and diagnose both in-flight and retained trigger recovery");
        RemoveTestConfig(config);
    }
}

void Runtime_RulesIndependentFeatureContinuationsAreNotLost()
{
    const auto config = WriteRuntimeConfig("rules-feature-continuations");
    FakeHost host;
    ConfigureTestPlayer(host);
    int polls = 0;
    host.ResponseFactory = [&](const auto& request) -> portal_cod4x::HttpResponse {
        if (request.Url.ends_with("/evaluate"))
        {
            const auto trigger = *RulesJsonReader::Parse(request.Body);
            return {200, RulesReply(CommandForTrigger(trigger, "PrivateMessage").Dump() + "," +
                CommandForTrigger(trigger, "PublicMessage", "bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee").Dump(),
                "\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\",\"bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee\"")};
        }
        if (request.Url.ends_with("/outcomes"))
        {
            const auto result = *RulesJsonReader::Parse(request.Body);
            return {200, RulesReply({}, RulesJson::Quote(*result.String("executionId")))};
        }
        ++polls;
        const auto poll = *RulesJsonReader::Parse(request.Body);
        Assert(poll.Find("executionIds")->Items.size() == 2,
            "One feature outcome must not erase the other feature's pending cloud cursor");
        return {200, RulesReply()};
    };
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    TickFrames(runtime, host);
    host.CurrentTime += 3;
    TickFrames(runtime, host);
    Assert(polls == 1, "Unfinished independent features must be resumed together without inventing local ordering");
    RemoveTestConfig(config);
}

void Runtime_RulesSnapshotsRetainRuntimeAndExactDisconnectIdentity()
{
    const auto config = WriteRuntimeConfig("rules-snapshot-binding");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["POST https://example.test/ingest/events/server-status"] = {202, ""};
    host.Responses["POST https://example.test/ingest/events/player-disconnected"] = {202, ""};
    host.ResponseFactory = [](const auto&) -> portal_cod4x::HttpResponse { return {200, RulesReply()}; };
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    TickFrames(runtime, host);
    host.CurrentTime += 61;
    TickFrames(runtime, host);
    const auto request = std::find_if(host.Requests.begin(), host.Requests.end(), [](const auto& entry) {
        return entry.Url.ends_with("/events/server-status");
    });
    Assert(request != host.Requests.end(), "Observed connection must be included in periodic status");
    const auto status = *RulesJsonReader::Parse(request->Body);
    const auto& snapshot = status.Items[0];
    const auto& player = snapshot.Find("players")->Items[0];
    Assert(snapshot.String("snapshotId") == snapshot.String("messageId") &&
        snapshot.Integer("sequence") == snapshot.Integer("sequenceId") &&
        snapshot.String("observedAtUtc").has_value() &&
        snapshot.Find("runtime")->String("instanceId").has_value() &&
        snapshot.Find("runtime")->String("startedAtUtc") == "1970-01-01T01:23:20Z" &&
        player.String("state") == "Connected" && player.Unsigned("connectionGeneration") == 1,
        "Authoritative binding metadata must reuse observed UUID, runtime start and existing sequence");
    const auto connectionId = player.String("connectionId");
    const auto instanceId = snapshot.Find("runtime")->String("instanceId");
    const auto previousSequence = snapshot.Integer("sequence");
    runtime.HandlePlayerDisconnected(host, 2);
    TickFrames(runtime, host);
    const auto disconnect = std::find_if(host.Requests.begin(), host.Requests.end(), [](const auto& entry) {
        return entry.Url.ends_with("/events/player-disconnected");
    });
    Assert(disconnect != host.Requests.end(), "Observed disconnect must be delivered");
    const auto tombstone = RulesJsonReader::Parse(disconnect->Body)->Items[0];
    Assert(tombstone.String("connectionId") == connectionId &&
        tombstone.Unsigned("connectionGeneration") == 1 &&
        tombstone.Find("runtime")->String("instanceId") == instanceId &&
        tombstone.Integer("sequence") > previousSequence,
        "Only explicit disconnect carries the exact runtime/connection/generation tombstone");
    // Hot-loaded roster identities are partial observations, never fresh admissions.
    runtime.HandlePlayerConnected(host, 2);
    host.CurrentTime += 61;
    TickFrames(runtime, host);
    const auto unknown = std::find_if(host.Requests.rbegin(), host.Requests.rend(), [](const auto& entry) {
        return entry.Url.ends_with("/events/server-status");
    });
    const auto unknownStatus = RulesJsonReader::Parse(unknown->Body)->Items[0];
    Assert(unknownStatus.Find("players")->Items[0].String("state") == "Unknown" &&
        unknownStatus.Find("players")->Items[0].Find("connectionId")->Type == RulesJson::Kind::Null,
        "Unknown continuity must never become an adoptable connection binding");
    Assert(std::count_if(host.Requests.begin(), host.Requests.end(), [](const auto& entry) {
        return entry.Url.ends_with("/rules/evaluate");
    }) == 1, "Unknown continuity must not send an invalid nonnullable normalized trigger");
    RemoveTestConfig(config);
}

void Runtime_PortalTempBansExpireOfflineWithoutAffectingPermanentBans()
{
    const auto config = WriteRuntimeConfig("portal-temp-expiry");
    FakeHost host;
    ConfigureTestPlayer(host);
    const std::string url = "GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200";
    host.Responses[url] = {200, "{\"data\":{\"items\":["
        "{\"type\":\"TempBan\",\"expires\":\"1970-01-01T01:23:30.0000000\",\"player\":{\"guid\":\"76561198000000001\"}},"
        "{\"Type\":\"Ban\",\"Expires\":null,\"Player\":{\"Guid\":\"76561198000000002\"}}]}}"};
    portal_cod4x::PluginRuntime runtime(config.string(), [&] { return host.CurrentTime; });
    runtime.Initialize(host, "0.3.0");
    TickFrames(runtime, host);
    std::string reason;
    Assert(runtime.TryGetPlayerBanMessage(76561198000000001ULL, reason) &&
        runtime.TryGetAuthenticatedPlayerBanMessage(76561198000000001ULL, reason),
        "Unexpired portal temporary ban must enforce in both admission hooks");
    Assert(host.DroppedPlayers.size() == 1, "Fresh active temporary ban proactively drops existing player");
    host.Responses[url] = {503, ""};
    host.CurrentTime = 5010;
    Assert(!runtime.TryGetPlayerBanMessage(76561198000000001ULL, reason) &&
        !runtime.TryGetAuthenticatedPlayerBanMessage(76561198000000001ULL, reason),
        "Temporary ban must stop enforcing at exact UTC expiry without any successful refresh or tick");
    Assert(runtime.TryGetPlayerBanMessage(76561198000000002ULL, reason),
        "Null expiry remains permanent during an outage");
    Assert(runtime.RenderPortalBanListDump().find("76561198000000001") == std::string::npos,
        "Expired cached portal bans must not appear as actively enforced bans");
    host.CurrentTime += 121;
    TickFrames(runtime, host);
    Assert(host.DroppedPlayers.size() == 1 && runtime.TryGetPlayerBanMessage(76561198000000002ULL, reason),
        "Offline refresh must preserve valid permanent bans without re-enforcing expired temporary bans");
    RemoveTestConfig(config);
}

void Runtime_FractionalPortalExpiryHonorsExactUtcInstant()
{
    for (int scenario = 0; scenario < 2; ++scenario)
    {
        const auto config = WriteRuntimeConfig("portal-fractional-expiry-" + std::to_string(scenario));
        FakeHost host;
        ConfigureTestPlayer(host);
        host.CurrentSubsecondTicks = scenario == 0 ? 4999999 : 5000000;
        const std::string url = "GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200";
        host.Responses[url] = {200, "{\"data\":{\"items\":["
            "{\"type\":\"TempBan\",\"expires\":\"1970-01-01T01:23:20.4000000Z\",\"player\":{\"guid\":\"76561198000000001\"}},"
            "{\"type\":\"TempBan\",\"expires\":\"1970-01-01T01:23:20.5000000\",\"player\":{\"guid\":\"76561198000000001\"}}]}}"};
        portal_cod4x::PluginRuntime runtime(config.string(), [&] { return host.CurrentTime; },
            [&] { return host.GetUnixTime100Nanoseconds(); });
        runtime.Initialize(host, "0.3.0");
        TickFrames(runtime, host);
        std::string reason;
        Assert((host.DroppedPlayers.size() == 1) == (scenario == 0),
            "Refresh must proactively enforce an unexpired fractional ban but not one at expiry");
        Assert(runtime.TryGetPlayerBanMessage(76561198000000001ULL, reason) == (scenario == 0),
            "Refresh must retain the latest fractional expiry within the same second");
        if (scenario == 0)
        {
            host.Responses[url] = {503, ""};
            Assert(runtime.TryGetAuthenticatedPlayerBanMessage(76561198000000001ULL, reason),
                "The admission hook must enforce the fractional ban one tick before expiry");
            host.CurrentSubsecondTicks = 5000000;
            Assert(!runtime.TryGetPlayerBanMessage(76561198000000001ULL, reason) &&
                !runtime.TryGetAuthenticatedPlayerBanMessage(76561198000000001ULL, reason) &&
                runtime.RenderPortalBanListDump().find("76561198000000001") == std::string::npos,
                "The exact expiry instant must stop offline admission and active-list enforcement");
        }
        RemoveTestConfig(config);
    }
}

void Runtime_FractionalCommandExpiryHonorsExactUtcInstant()
{
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        const auto config = WriteRuntimeConfig("rules-fractional-expiry-" + std::to_string(scenario));
        FakeHost host;
        ConfigureTestPlayer(host);
        host.CurrentSubsecondTicks = scenario == 1 ? 5000000 : 4999999;
        host.ResponseFactory = [scenario](const auto& request) -> portal_cod4x::HttpResponse {
            if (!request.Url.ends_with("/evaluate")) return {200, RulesReply()};
            const auto trigger = *RulesJsonReader::Parse(request.Body);
            auto command = CommandForTrigger(trigger);
            command.Fields["expiresAtUtc"] = *RulesJsonReader::Parse(scenario == 2 ?
                "\"1970-01-01T01:23:20.50000001Z\"" : "\"1970-01-01T01:23:20.5000000Z\"");
            return {200, RulesReply(command.Dump())};
        };
        portal_cod4x::PluginRuntime runtime(config.string());
        runtime.Initialize(host, "0.3.0");
        runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
        runtime.HandlePlayerConnected(host, 2);
        TickFrames(runtime, host, 12);
        Assert(host.DroppedPlayers.size() == (scenario == 0 ? 1U : 0U),
            "A native effect may dispatch one tick before expiry, never at expiry or for unrepresentable precision");
        const auto expected = scenario == 0 ? "\"outcome\":\"Dispatched\"" :
            scenario == 1 ? "\"outcome\":\"Stale\"" : "\"outcome\":\"Failed\"";
        Assert(std::any_of(host.Requests.begin(), host.Requests.end(), [&](const auto& request) {
            return request.Url.ends_with("/outcomes") && request.Body.find(expected) != std::string::npos;
        }), "Fractional command boundary must return the correct explicit outcome");
        RemoveTestConfig(config);
    }
}

void Runtime_PortalBanRefreshRejectsMalformedExpiryWithoutClearingValidCache()
{
    const std::vector<std::string> invalidExpires{
        "", ",\"expires\":null", ",\"expires\":42", ",\"expires\":\"invalid\"",
        ",\"expires\":\"2026-02-31T12:00:00Z\"", ",\"expires\":\"1970-01-01T01:30:00+02:00\"",
        ",\"expires\":\"1970-01-01T01:30:00.00000001Z\"",
        ",\"expires\":\"1970-01-01T01:30:00Z\",\"Expires\":null"};
    for (std::size_t scenario = 0; scenario < invalidExpires.size(); ++scenario)
    {
        const auto config = WriteRuntimeConfig("portal-invalid-expiry-" + std::to_string(scenario));
        FakeHost host;
        host.CurrentTime = 5000;
        const std::string url = "GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200";
        host.Responses[url] = {200, "{\"data\":{\"items\":["
            "{\"type\":\"Ban\",\"player\":{\"guid\":\"76561198000000001\"}}]}}"};
        portal_cod4x::PluginRuntime runtime(config.string(), [&] { return host.CurrentTime; });
        runtime.Initialize(host, "0.3.0");
        TickFrames(runtime, host);
        host.Responses[url] = {200, "{\"data\":{\"items\":[{\"type\":\"TempBan\"" +
            invalidExpires[scenario] + ",\"player\":{\"guid\":\"76561198000000002\"}}]}}"};
        host.CurrentTime += 121;
        TickFrames(runtime, host);
        std::string reason;
        Assert(runtime.TryGetPlayerBanMessage(76561198000000001ULL, reason) &&
            !runtime.TryGetPlayerBanMessage(76561198000000002ULL, reason),
            "Malformed temporary expiry must preserve previous cache, never fabricate a permanent ban");
        Assert(std::any_of(host.Logs.begin(), host.Logs.end(), [](const auto& log) {
            return log.find("Invalid active-ban page or expiry") != std::string::npos;
        }), "Malformed authoritative expiry must have an explicit refresh diagnostic");
        RemoveTestConfig(config);
    }
}

void Runtime_PortalBanOverlapAcrossPagesAndElapsedRefresh()
{
    for (int permanent = 0; permanent < 2; ++permanent)
    {
        const auto config = WriteRuntimeConfig("portal-overlap-" + std::to_string(permanent));
        FakeHost host;
        host.CurrentTime = 5000;
        std::string items;
        for (int i = 0; i < 200; ++i)
        {
            if (i) items += ',';
            items += "{\"type\":3,\"expires\":\"1970-01-01T01:23:30Z\",\"player\":{\"guid\":\"76561198000000001\"}}";
        }
        host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] = {
            200, "{\"data\":{\"items\":[" + items + "]}}"};
        host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=200&takeEntries=200"] = {
            200, "{\"data\":{\"items\":[" + std::string(permanent
                ? "{\"type\":4,\"expires\":null"
                : "{\"type\":3,\"expires\":\"1970-01-01T01:23:40+00:00\"") +
            ",\"player\":{\"guid\":\"76561198000000001\"}}]}}"};
        portal_cod4x::PluginRuntime runtime(config.string(), [&] { return host.CurrentTime; });
        runtime.Initialize(host, "0.3.0");
        TickFrames(runtime, host);
        host.CurrentTime = 5011;
        std::string reason;
        Assert(runtime.TryGetPlayerBanMessage(76561198000000001ULL, reason),
            "Overlapping records must retain permanent ban or latest temporary expiry across pages");
        host.CurrentTime = 5020;
        Assert(runtime.TryGetPlayerBanMessage(76561198000000001ULL, reason) == (permanent != 0),
            "Latest temporary expiry must elapse, but overlapping permanent ban must remain");
        RemoveTestConfig(config);
    }

    const auto config = WriteRuntimeConfig("portal-already-expired");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] = {
        200, "{\"data\":{\"items\":[{\"type\":\"TempBan\",\"expires\":\"1970-01-01T01:23:19Z\","
        "\"player\":{\"guid\":\"76561198000000001\"}}]}}"};
    portal_cod4x::PluginRuntime runtime(config.string(), [&] { return host.CurrentTime; });
    runtime.Initialize(host, "0.3.0");
    TickFrames(runtime, host);
    std::string reason;
    Assert(host.DroppedPlayers.empty() && !runtime.TryGetPlayerBanMessage(76561198000000001ULL, reason),
        "Already elapsed temporary bans in a refresh must never enter enforcement or proactively drop");
    RemoveTestConfig(config);
}

void Runtime_SteamBanMatchingPreservesPlayerIdOwnership()
{
    const auto config = WriteRuntimeConfig("steam-ban-ownership");
    FakeHost host;
    host.CurrentTime = 5000;
    const std::string url = "GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200";
    host.Responses[url] = {200, "{\"data\":{\"items\":["
        "{\"Type\":\"Ban\",\"Player\":{\"Guid\":\"101\",\"SteamId\":\"76561198000000001\"}},"
        "{\"type\":\"Ban\",\"player\":{\"guid\":\"102\",\"steamId\":\"76561198000000001\"}}]}}"};
    portal_cod4x::PluginRuntime runtime(config.string(), [&] { return host.CurrentTime; });
    runtime.Initialize(host, "0.3.0");
    TickFrames(runtime, host);
    std::string reason;
    Assert(runtime.TryGetPlayerBanMessage(101, reason, 999) &&
        runtime.TryGetAuthenticatedPlayerBanMessage(999, reason, 76561198000000001ULL) &&
        runtime.TryGetPlayerBanMessage(0, reason, 76561198000000001ULL),
        "Either player ID or verified Steam ID must independently match an active portal ban");
    Assert(!runtime.TryGetPlayerBanMessage(999, reason, 0) &&
        !runtime.TryGetAuthenticatedPlayerBanMessage(999, reason, 76561198000000002ULL),
        "Missing or different Steam identity must not match another profile's ban");
    runtime.HandlePlayerBanAdded(999, "Native reason", 0, "Native player", 5001);
    Assert(runtime.TryGetPlayerBanMessage(999, reason, 76561198000000001ULL) && reason == "Native reason",
        "Native player-ID ban must retain its existing reason before a separate Steam match");
    host.CurrentTime = 5001;
    Assert(runtime.TryGetPlayerBanMessage(999, reason, 76561198000000001ULL) &&
        reason == "You are banned from this server.",
        "Expired native player-ID ban must not hide an independent active portal Steam ban");
    runtime.HandlePlayerBanRemoved(999);

    runtime.HandlePlayerBanRemoved(101);
    Assert(!runtime.TryGetPlayerBanMessage(101, reason) &&
        runtime.TryGetPlayerBanMessage(101, reason, 76561198000000001ULL),
        "Player-ID unban must retain another active profile ban sharing the Steam ID");
    host.Responses[url] = {200, "{\"data\":{\"items\":["
        "{\"type\":\"Ban\",\"player\":{\"guid\":\"102\",\"steamId\":\"76561198000000001\"}}]}}"};
    TickFrames(runtime, host);
    Assert(runtime.TryGetPlayerBanMessage(999, reason, 76561198000000001ULL),
        "Refresh after a profile lift must preserve the independent shared-Steam ban");
    host.Responses[url] = {200, "{\"data\":{\"items\":[]}}"};
    host.CurrentTime += 121;
    TickFrames(runtime, host);
    Assert(!runtime.TryGetPlayerBanMessage(999, reason, 76561198000000001ULL),
        "Removing the last profile ban must remove its Steam association");
    Assert(std::find(host.ExecutedCommands.begin(), host.ExecutedCommands.end(), "unban 102") !=
        host.ExecutedCommands.end() &&
        std::none_of(host.ExecutedCommands.begin(), host.ExecutedCommands.end(), [](const auto& command) {
            return command == "unban 76561198000000001";
        }), "Reconciliation unban must remain keyed by player ID, never Steam ID");
    runtime.Shutdown(host);
    RemoveTestConfig(config);
}

void Runtime_SteamBanExpiryAndPaginationRetainIndependentAssociations()
{
    const auto config = WriteRuntimeConfig("steam-ban-expiry");
    FakeHost host;
    host.CurrentTime = 5000;
    std::string items;
    for (int i = 0; i < 200; ++i)
    {
        if (i) items += ',';
        items += "{\"type\":\"TempBan\",\"expires\":\"1970-01-01T01:23:30.5000000Z\","
            "\"player\":{\"guid\":\"101\",\"steamId\":\"76561198000000001\"}}";
    }
    const std::string url = "GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=";
    host.Responses[url + "0&takeEntries=200"] = {200, "{\"data\":{\"items\":[" + items + "]}}"};
    host.Responses[url + "200&takeEntries=200"] = {200, "{\"data\":{\"items\":["
        "{\"type\":\"Ban\",\"player\":{\"guid\":\"101\",\"steamId\":\"76561198000000002\"}},"
        "{\"type\":\"TempBan\",\"expires\":\"1970-01-01T01:23:40Z\","
        "\"player\":{\"guid\":\"102\",\"steamId\":\"76561198000000001\"}}]}}"};
    portal_cod4x::PluginRuntime runtime(config.string(), [&] { return host.CurrentTime; },
        [&] { return host.GetUnixTime100Nanoseconds(); });
    runtime.Initialize(host, "0.3.0");
    TickFrames(runtime, host);
    std::string reason;
    runtime.HandlePlayerBanRemoved(102);
    host.CurrentTime = 5010;
    host.CurrentSubsecondTicks = 4999999;
    Assert(runtime.TryGetAuthenticatedPlayerBanMessage(999, reason, 76561198000000001ULL),
        "Paginated temporary Steam association must enforce one tick before expiry");
    host.CurrentSubsecondTicks = 5000000;
    Assert(!runtime.TryGetAuthenticatedPlayerBanMessage(999, reason, 76561198000000001ULL) &&
        runtime.TryGetPlayerBanMessage(101, reason) &&
        runtime.TryGetPlayerBanMessage(999, reason, 76561198000000002ULL),
        "An expired Steam association must not inherit a different association's permanent expiry");
    runtime.HandlePlayerBanRemoved(101);
    Assert(!runtime.TryGetPlayerBanMessage(999, reason, 76561198000000002ULL),
        "Removing the profile must remove all its paginated Steam associations");
    runtime.Shutdown(host);
    RemoveTestConfig(config);
}

void Runtime_SteamBanRefreshHandlesMissingAndInvalidIdentities()
{
    const auto config = WriteRuntimeConfig("steam-ban-invalid");
    FakeHost host;
    host.CurrentTime = 5000;
    const std::string url = "GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200";
    host.Responses[url] = {200, "{\"data\":{\"items\":["
        "{\"type\":\"Ban\",\"player\":{\"guid\":\"101\",\"steamId\":null}},"
        "{\"type\":\"Ban\",\"player\":{\"guid\":\"102\",\"steamId\":\"\"}},"
        "{\"type\":\"Ban\",\"player\":{\"guid\":\"103\",\"steamId\":\"0\"}},"
        "{\"type\":\"Ban\",\"player\":{\"guid\":\"104\"}},"
        "{\"type\":\"Ban\",\"player\":{\"guid\":\"105\",\"steamId\":\"76561198000000001\"}}]}}"};
    portal_cod4x::PluginRuntime runtime(config.string(), [&] { return host.CurrentTime; });
    runtime.Initialize(host, "0.3.0");
    TickFrames(runtime, host);
    std::string reason;
    for (const auto playerId : {101ULL, 102ULL, 103ULL, 104ULL})
        Assert(runtime.TryGetPlayerBanMessage(playerId, reason), "Absent Steam ID must preserve player-ID enforcement");
    Assert(!runtime.TryGetPlayerBanMessage(999, reason) && !runtime.TryGetPlayerBanMessage(0, reason),
        "Null, blank, omitted and zero Steam IDs must not create wildcard bans");
    for (const auto* invalid : {"42", "\"-1\"", "\"76561198000000001x\"", "\"18446744073709551616\"",
        "\"76561198000000002\",\"SteamId\":\"76561198000000003\""})
    {
        host.Responses[url] = {200, "{\"data\":{\"items\":["
            "{\"type\":\"Ban\",\"player\":{\"guid\":\"106\",\"steamId\":" + std::string(invalid) + "}}]}}"};
        host.CurrentTime += 121;
        TickFrames(runtime, host);
        Assert(runtime.TryGetPlayerBanMessage(999, reason, 76561198000000001ULL) &&
            !runtime.TryGetPlayerBanMessage(106, reason),
            "Malformed Steam identity must reject the entire refresh and retain the prior cache");
    }
    Assert(std::any_of(host.Logs.begin(), host.Logs.end(), [](const auto& log) {
        return log.find("Invalid active-ban page or expiry") != std::string::npos;
    }), "Invalid Steam identity must surface a refresh error");
    runtime.Shutdown(host);
    RemoveTestConfig(config);
}

void Runtime_SteamBansEnforceFreshVerifiedSlotIdentity()
{
    const auto config = WriteRuntimeConfig("steam-ban-live");
    FakeHost host;
    ConfigureTestPlayer(host);
    host.PlayerSteamIds[2] = 76561198000000002ULL;
    host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] =
        {200, "{\"data\":{\"items\":[{\"type\":\"Ban\","
        "\"player\":{\"guid\":\"101\",\"steamId\":\"76561198000000001\"}}]}}"};
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    TickFrames(runtime, host);
    Assert(host.DroppedPlayers.empty(), "Different verified Steam identity must not proactively drop");
    host.PlayerSteamIds[2] = 76561198000000001ULL;
    runtime.HandleClientAuthorized(host);
    Assert(host.DroppedPlayers.size() == 1 && host.DroppedPlayers.back().Slot == 2,
        "Late authorization must enforce newly verified Steam identity without awaiting refresh");
    host.PlayerIds[2] = 999;
    host.PlayerSteamIds[2] = 76561198000000002ULL;
    runtime.HandleClientAuthorized(host);
    Assert(host.DroppedPlayers.size() == 1, "Reused slot must not inherit the previous occupant's cached Steam identity");
    host.PlayerSteamIds[2] = 76561198000000001ULL;
    host.OnDrop = [&](int slot) { runtime.HandlePlayerDisconnected(host, slot); };
    runtime.HandlePlayerConnected(host, 2);
    Assert(host.DroppedPlayers.size() == 2, "Enter-world must enforce Steam bans and tolerate reentrant disconnect");
    host.PlayerIds[3] = 0;
    host.PlayerSteamIds[3] = 76561198000000001ULL;
    host.CurrentTime += 121;
    TickFrames(runtime, host);
    Assert(std::any_of(host.DroppedPlayers.begin(), host.DroppedPlayers.end(), [](const auto& drop) {
        return drop.Slot == 3;
    }), "Successful refresh must enforce Steam-only identity even when player ID is zero");
    runtime.Shutdown(host);
    RemoveTestConfig(config);
}

void Runtime_LateSteamVerificationEnforcesOfflineWithoutAuthCallback()
{
    const auto config = WriteRuntimeConfig("steam-ban-late-offline");
    FakeHost host;
    ConfigureTestPlayer(host);
    const std::string url = "GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200";
    host.Responses[url] = {200, "{\"data\":{\"items\":[{\"type\":\"Ban\","
        "\"player\":{\"guid\":\"101\",\"steamId\":\"76561198000000002\"}}]}}"};
    portal_cod4x::PluginRuntime runtime(config.string());
    runtime.Initialize(host, "0.3.0");
    runtime.HandlePlayerConnect(host, 2, "198.51.100.10");
    runtime.HandlePlayerConnected(host, 2);
    TickFrames(runtime, host);
    Assert(host.DroppedPlayers.empty(), "Pending Steam verification must not cause a Steam-only drop");

    host.Responses[url] = {503, ""};
    host.CurrentTime += 121;
    TickFrames(runtime, host);
    host.PlayerSteamIds[2] = 76561198000000002ULL;
    host.OnDrop = [&](int slot) { runtime.HandlePlayerDisconnected(host, slot); };
    runtime.Tick(host);
    Assert(host.DroppedPlayers.size() == 1 && host.DroppedPlayers.front().Slot == 2,
        "The next frame must enforce newly verified Steam ID from existing cache during an API outage without auth callback");
    TickFrames(runtime, host);
    Assert(host.DroppedPlayers.size() == 1, "Unchanged identity must not repeat a drop every frame");
    host.PlayerIds[2] = 999;
    host.PlayerSteamIds[2] = 76561198000000003ULL;
    TickFrames(runtime, host);
    Assert(host.DroppedPlayers.size() == 1, "Slot reuse must not inherit a previous verified Steam identity");
    host.PlayerSteamIds[2] = 76561198000000002ULL;
    runtime.Tick(host);
    Assert(host.DroppedPlayers.size() == 2, "A subsequent occupant's newly verified identity must be checked");
    runtime.Shutdown(host);
    RemoveTestConfig(config);
}

void Runtime_EnterWorldSteamEnforcementDoesNotRequireIngestGameType()
{
    for (const auto* gameType : {"", ",\"gameType\":\"\""})
    {
        const auto config = WriteRuntimeConfig("steam-ban-no-game-type");
        {
            std::ofstream output(config);
            output << "{\"ingestBaseUrl\":\"https://example.test/ingest\","
                "\"ingestSubscriptionKey\":\"sub-key-test\","
                "\"gameServerId\":\"11111111-2222-3333-4444-555555555555\"" << gameType << "}";
        }
        FakeHost host;
        ConfigureTestPlayer(host);
        host.Responses["GET https://example.test/ingest/active-bans?gameType=CallOfDuty4x&skipEntries=0&takeEntries=200"] =
            {200, "{\"data\":{\"items\":[{\"type\":\"Ban\","
            "\"player\":{\"guid\":\"101\",\"steamId\":\"76561198000000002\"}}]}}"};
        portal_cod4x::PluginRuntime runtime(config.string());
        runtime.Initialize(host, "0.3.0");
        TickFrames(runtime, host);
        host.PlayerSteamIds[2] = 76561198000000002ULL;
        runtime.HandlePlayerConnected(host, 2);
        Assert(host.DroppedPlayers.size() == 1 && host.DroppedPlayers.front().Slot == 2,
            "Enter-world must enforce cached Steam ban when gameType is omitted or blank");
        runtime.Shutdown(host);
        RemoveTestConfig(config);
    }
}

void NativeExecutor_PreventsDuplicateAndCapacityReplay()
{
    FakeHost host;
    host.PlayerIds[2] = 76561198000000001ULL;
    host.CurrentTime = 5000;
    portal_cod4x::NativeActionTarget target{"76561198000000001",
        "11111111-2222-3333-4444-555555555555", 2, 1};
    portal_cod4x::NativeAction action{"effect-0", target, portal_cod4x::NativeEffect::Drop, "Reason", 5400LL * 10000000, true};
    portal_cod4x::NativeActionExecutor executor;
    host.OnDrop = [&](int) {
        Assert(executor.Execute(host, action, target).Outcome == "Dispatched",
            "Reentrant duplicate must return reserved result without repeating host effect");
    };
    Assert(executor.Execute(host, action, target).Outcome == "Dispatched" && host.DroppedPlayers.size() == 1,
        "Native identity must be reserved before invoking reentrant host APIs");
    host.OnDrop = {};
    for (int i = 1; i < 1000; ++i)
    {
        action.ExecutionId = "effect-" + std::to_string(i);
        Assert(executor.Execute(host, action, target).Outcome == "Dispatched", "Native capacity permits distinct effects");
    }
    action.ExecutionId = "effect-1000";
    Assert(executor.Execute(host, action, target).Outcome == "Failed", "Never evict live dedup identity at capacity");
    action.ExecutionId = "effect-0";
    executor.Execute(host, action, target);
    Assert(host.DroppedPlayers.size() == 1000, "Original effect remains protected after capacity exhaustion");
    action.Text = "Changed";
    Assert(executor.Execute(host, action, target).Outcome == "Failed", "Logical identity cannot be reused with changed effect");

    portal_cod4x::NativeActionExecutor independent;
    action.Effect = portal_cod4x::NativeEffect::Unsupported;
    for (int i = 0; i < 1001; ++i)
    {
        action.ExecutionId = "unsupported-" + std::to_string(i);
        Assert(independent.Execute(host, action, target).Outcome == "SkippedUnsupported",
            "Unsupported effects must not exhaust supported native effect capacity");
    }
    action.Effect = portal_cod4x::NativeEffect::PrivateMessage;
    action.Text = std::string(149, 'x');
    action.ExecutionId = "text-149";
    Assert(independent.Execute(host, action, target).Outcome == "Dispatched",
           "Exactly 149 UTF-8 bytes must be accepted without re-splitting prepared commands");
    action.ExecutionId = "text-150";
    action.Text += 'x';
    Assert(independent.Execute(host, action, target).Outcome == "Failed",
           "150 UTF-8 bytes must be rejected before native invocation");
    action.ExecutionId = "unicode-148";
    action.Text.clear();
    for (int i = 0; i < 74; ++i)
        action.Text += "\xc3\xa9";
    Assert(independent.Execute(host, action, target).Outcome == "Dispatched",
        "Text limit counts UTF-8 bytes, not codepoints");
    action.ExecutionId = "unicode-150";
    action.Text += "\xc3\xa9";
    Assert(independent.Execute(host, action, target).Outcome == "Failed",
        "Multibyte text above the byte bound must not reach native APIs");
}
}

int main()
{
    Runtime_SteamBanMatchingPreservesPlayerIdOwnership();
    Runtime_SteamBanExpiryAndPaginationRetainIndependentAssociations();
    Runtime_SteamBanRefreshHandlesMissingAndInvalidIdentities();
    Runtime_SteamBansEnforceFreshVerifiedSlotIdentity();
    Runtime_LateSteamVerificationEnforcesOfflineWithoutAuthCallback();
    Runtime_EnterWorldSteamEnforcementDoesNotRequireIngestGameType();
    BuildMessage_UsesPrefixAndVersion();
    BuildMessage_FallsBackWhenPrefixOrVersionMissing();
    Runtime_AgentNameControlsStartupAndRefreshesPresentationOnly();
    Runtime_AgentNameRejectsUnsafeOrIncompleteRefreshes();
    Runtime_LocalHeadersAppearOnlyInFirstFragment();
    Runtime_AgentNameStartupRejectsStaleOrInvalidSidecars();
    Runtime_LoadsConfigAndStoresServerContext();
    Runtime_EmitsAndFlushesPlayerConnectedEvent();
    Runtime_EmitsPlayerConnectedOncePerConnection();
    Runtime_ServerStatusSnapshot_ReconcilesLiveSlotsAcrossMapRotation();
    Runtime_PlaytimeMetadataSurvivesRetryAndReconnect();
    Runtime_InFlightBatchDoesNotAcknowledgeEvictedIndices();
    Runtime_RestartLosesUnsentEvents();
    Runtime_CoverageCountsCapacityLoss();
    Runtime_CoverageCountsAgeAndSimultaneousThresholds();
    Runtime_CoverageCountsInFlightEviction();
    Runtime_CoverageRetainsCountersAcrossStatusRetry();
    Runtime_CoverageUsesFirstPublicationTimeAcrossRetries();
    Runtime_DedicatedPlaytimeEventsPreserveIdentityAndPrivacy();
    Runtime_DedicatedPlaytimeAmbiguousReconnectRemainsUncertain();
    Runtime_DedicatedPlaytimeTracksResolvedRosterIdentity();
    Runtime_DedicatedPlaytimeDoesNotGuessUnresolvedDisconnect();
    Runtime_DedicatedPlaytimeReconcilesUnresolvedEnterWorld();
    Runtime_DedicatedPlaytimeDisconnectUsesResolvedIdentity();
    Runtime_EmitsEmptyIpWhenConnectAddressUnavailable();
    Runtime_AuthorizedIdentity_AllowsDisconnectEventWhenPlayerIdUnavailableAtDisconnect();
    Runtime_DropsPoisonEventsAndUnblocksOtherQueues();
    Runtime_HandleClientCommand_IgnoresPortalOwnedCommands();
    Runtime_HandleChatMessage_DoesNotInterceptPortalOwnedCommands();
    Runtime_HandleChatMessage_StripsLeadingControlByteFromPayload();
    Runtime_IpObservationsHaveStableRetryIdsAndDistinctChangeIds();
    Runtime_ConnectionIdCorrelatesEventsAndRulesAndFailsOpenAcrossLevelChange();
    Runtime_ConnectCallbackAfterLevelExitStartsNewConnection();
    Runtime_ReconnectAfterLevelExitRetiresPriorState();
    Runtime_DisconnectAfterLevelExitDoesNotReuseAnotherPlayersIdentity();
    Runtime_UnobservedConnectionIsNotAssignedAnAdmissionId();
    Runtime_RulesTriggerCapabilitiesAndNativeEffects();
    ChatMessages_PreserveBoundariesAndRejectInvalidBatches();
    Runtime_RulesRecipientMessagesSurviveTriggerDisconnect();
    Runtime_RulesRejectUnsafeOrStaleCommandsIndependently();
    Runtime_RulesRechecksConnectionAndSupportsReentrantDisconnect();
    Runtime_RulesOutcomesRetryAndCloudContinuationPolls();
    Runtime_RulesOutcomeResponseContinuesAndMalformedAckRetries();
    Runtime_RulesCallbackOrderingPreservesConnectedObservation();
    Runtime_RulesRejectPreviousObservationAfterIpChange();
    Runtime_RulesRedeliveryRetainsDispatchedAfterDisconnect();
    Runtime_RulesPartialStartsRetryOriginalTriggerWithoutReplayingEffects();
    Runtime_RulesTriggerRetriesExpireAndRejectChangedConnections();
    Runtime_RulesShutdownDiagnosesAbandonedTriggerRecovery();
    Runtime_RulesIndependentFeatureContinuationsAreNotLost();
    Runtime_RulesSnapshotsRetainRuntimeAndExactDisconnectIdentity();
    Runtime_PortalTempBansExpireOfflineWithoutAffectingPermanentBans();
    Runtime_FractionalPortalExpiryHonorsExactUtcInstant();
    Runtime_FractionalCommandExpiryHonorsExactUtcInstant();
    Runtime_PortalBanRefreshRejectsMalformedExpiryWithoutClearingValidCache();
    Runtime_PortalBanOverlapAcrossPagesAndElapsedRefresh();
    RulesJson_RejectsAmbiguousExecutablePayloads();
    NativeExecutor_PreventsDuplicateAndCapacityReplay();
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
    Runtime_ServerOriginBanResolvesLegacyIdToCanonicalPuidFromRoster();
    Runtime_ServerOriginBanKeepsRawIdWhenPlayerNotConnected();
    Runtime_JsonUnescapeDecodesUnicodeEscapesInConfigStrings();
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
