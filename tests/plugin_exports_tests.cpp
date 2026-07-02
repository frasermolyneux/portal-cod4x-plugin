#include "cod4x_abi.h"

#include "portal_cod4x/plugin_version.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
struct ChatMessage
{
    int slot;
    std::string message;
};

std::vector<std::string> g_logs;
std::vector<ChatMessage> g_chats;
std::vector<std::string> g_commands;

void AssertTrue(bool condition, const char* failureMessage)
{
    if (!condition)
    {
        std::cerr << "Assertion failed: " << failureMessage << '\n';
        std::exit(1);
    }
}

std::string FormatMessage(const char* fmt, va_list args)
{
    char buffer[2048] = {};
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    return std::string(buffer);
}
}

extern "C" void COD4X_CALL Plugin_Printf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    g_logs.emplace_back(FormatMessage(fmt, args));
    va_end(args);
}

extern "C" void COD4X_CALL Plugin_ChatPrintf(int slot, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    g_chats.push_back({slot, FormatMessage(fmt, args)});
    va_end(args);
}

extern "C" void COD4X_CALL Plugin_Cbuf_AddText(const char* text)
{
    g_commands.emplace_back(text == nullptr ? "" : text);
}

extern "C" ftRequest_t* COD4X_CALL Plugin_HTTP_Request(
    const char*,
    const char*,
    byte*,
    int,
    const char*)
{
    return nullptr;
}

extern "C" void COD4X_CALL Plugin_HTTP_FreeObj(ftRequest_t*)
{
}

extern "C"
{
int OnInit();
void OnFrame();
void OnClientAuthorized();
void OnInfoRequest(pluginInfo_t* info);
}

int main()
{
    pluginInfo_t info = {};
    OnInfoRequest(&info);

    AssertTrue(info.handlerVersion.major == PLUGIN_HANDLER_VERSION_MAJOR, "Handler major version should match ABI definition.");
    AssertTrue(info.handlerVersion.minor == PLUGIN_HANDLER_VERSION_MINOR, "Handler minor version should match ABI definition.");
    AssertTrue(info.pluginVersion.major == portal_cod4x::kPluginVersionMajor, "Plugin major version should match generated version metadata.");
    AssertTrue(info.pluginVersion.minor == portal_cod4x::kPluginVersionMinor, "Plugin minor version should match generated version metadata.");
    AssertTrue(std::string(info.fullName).find("portal-cod4x-plugin") != std::string::npos, "Plugin full name should be populated.");
    AssertTrue(
        std::string(info.shortDescription).find(std::string(portal_cod4x::kPluginSemanticVersion)) != std::string::npos,
        "Plugin short description should include the semantic version.");

    const int initResult = OnInit();
    AssertTrue(initResult == 0, "OnInit should return success code.");
    AssertTrue(!g_chats.empty(), "OnInit should send at least one chat message.");
    AssertTrue(g_chats.front().slot == -1, "Startup broadcast should target all players with slot -1.");
    AssertTrue(g_chats.front().message.find("version") != std::string::npos, "Startup broadcast should contain a version string.");
    AssertTrue(!g_logs.empty(), "OnInit should emit at least one log message.");

    const std::size_t logCountAfterInit = g_logs.size();

    OnFrame();
    OnClientAuthorized();

    AssertTrue(g_logs.size() >= logCountAfterInit, "OnFrame and OnClientAuthorized should be safe to invoke.");

    std::cout << "All plugin export tests passed.\n";
    return 0;
}
