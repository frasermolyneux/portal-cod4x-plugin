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
    Assert(message.find("version 0.1.0") != std::string::npos, "Expected semantic version in message");
}

void BuildMessage_FallsBackWhenPrefixOrVersionMissing()
{
    const std::string message = portal_cod4x::BuildOnlineBroadcastMessage("", "");

    Assert(message.find("XI-BOT") != std::string::npos, "Expected default XI-BOT prefix when prefix is empty");
    Assert(message.find("0.0.0-unknown") != std::string::npos, "Expected fallback version when version is empty");
}

void InitializePlugin_EmitsLogAndBroadcast()
{
    FakeHost host;

    const int result = portal_cod4x::InitializePlugin(host, "1.2.3", "^4[^1XI-BOT^4]^7");

    Assert(result == 0, "InitializePlugin should return success code");
    Assert(host.Logs.size() == 1, "InitializePlugin should write one startup log");
    Assert(host.BroadcastMessages.size() == 1, "InitializePlugin should send one startup broadcast");
    Assert(host.BroadcastMessages.front().find("version 1.2.3") != std::string::npos, "Broadcast should include version");
    Assert(host.Logs.front().find("version 1.2.3") != std::string::npos, "Startup log should include version");
}
}

int main()
{
    BuildMessage_UsesPrefixAndVersion();
    BuildMessage_FallsBackWhenPrefixOrVersionMissing();
    InitializePlugin_EmitsLogAndBroadcast();

    std::cout << "All plugin runtime tests passed." << std::endl;
    return 0;
}
