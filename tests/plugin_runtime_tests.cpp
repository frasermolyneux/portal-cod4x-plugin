#include "portal_cod4x/plugin_runtime.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
class FakeHost final : public portal_cod4x::ICod4xHost
{
public:
    std::vector<std::string> BroadcastMessages;
    std::vector<std::string> Logs;

    void BroadcastChat(std::string_view message) override
    {
        BroadcastMessages.emplace_back(message);
    }

    void Log(std::string_view message) override
    {
        Logs.emplace_back(message);
    }
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
    Assert(message.find("XI Portal Plugin is online") != std::string::npos, "Expected startup wording in message");
    Assert(message.find("version 0.1.0") != std::string::npos, "Expected semantic version in message");
}

void BuildMessage_FallsBackWhenPrefixOrVersionMissing()
{
    const std::string message = portal_cod4x::BuildOnlineBroadcastMessage("", "");

    Assert(message.find("XI-BOT") != std::string::npos, "Expected default XI-BOT prefix when prefix is empty");
    Assert(message.find("XI Portal Plugin is online") != std::string::npos, "Expected startup wording in fallback message");
    Assert(message.find("0.0.0-unknown") != std::string::npos, "Expected fallback version when version is empty");
}

void InitializePlugin_EmitsLogAndBroadcast()
{
    FakeHost host;

    const int result = portal_cod4x::InitializePlugin(host, "1.2.3", "^4[^1XI-BOT^4]^7");
    const std::string expectedBroadcast = "^4[^1XI-BOT^4]^7 XI Portal Plugin is online (version 1.2.3)";
    const std::string expectedLog = "XI Portal Plugin is online (version 1.2.3)";

    Assert(result == 0, "InitializePlugin should return success code");
    Assert(host.Logs.size() == 1, "InitializePlugin should write one startup log");
    Assert(host.BroadcastMessages.size() == 1, "InitializePlugin should send one startup broadcast");
    Assert(host.BroadcastMessages.front() == expectedBroadcast, "Broadcast should match expected startup format");
    Assert(host.Logs.front() == expectedLog, "Startup log should match expected startup format");
}

void InitializePlugin_FallsBackWhenPrefixOrVersionMissing()
{
    FakeHost host;

    const int result = portal_cod4x::InitializePlugin(host, "", "");
    const std::string expectedBroadcast = std::string(portal_cod4x::kDefaultBotPrefix) + " XI Portal Plugin is online (version 0.0.0-unknown)";
    const std::string expectedLog = "XI Portal Plugin is online (version 0.0.0-unknown)";

    Assert(result == 0, "InitializePlugin should return success code for fallback path");
    Assert(host.Logs.size() == 1, "InitializePlugin should write one startup log for fallback path");
    Assert(host.BroadcastMessages.size() == 1, "InitializePlugin should send one startup broadcast for fallback path");
    Assert(host.BroadcastMessages.front() == expectedBroadcast, "Fallback broadcast should match expected startup format");
    Assert(host.Logs.front() == expectedLog, "Fallback startup log should match expected startup format");
}
}

int main()
{
    BuildMessage_UsesPrefixAndVersion();
    BuildMessage_FallsBackWhenPrefixOrVersionMissing();
    InitializePlugin_EmitsLogAndBroadcast();
    InitializePlugin_FallsBackWhenPrefixOrVersionMissing();

    std::cout << "All plugin runtime tests passed." << std::endl;
    return 0;
}
