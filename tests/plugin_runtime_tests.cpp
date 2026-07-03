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
    std::vector<std::string> BroadcastMessages;
    std::vector<std::string> Logs;
    std::vector<std::string> ExecutedCommands;
    std::unordered_map<std::string, portal_cod4x::HttpResponse> Responses;
    std::int64_t CurrentTime = 0;

    void BroadcastChat(std::string_view message) override
    {
        BroadcastMessages.emplace_back(message);
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
        std::string_view,
        std::string_view) override
    {
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
    InitializePlugin_EmitsLogAndBroadcast();
    InitializePlugin_FallsBackWhenPrefixOrVersionMissing();

    std::cout << "All plugin runtime tests passed." << std::endl;
    return 0;
}
