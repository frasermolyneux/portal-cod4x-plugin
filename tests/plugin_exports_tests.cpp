#include "cod4x_abi.h"

#include "portal_cod4x/plugin_version.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
std::string g_player_name = "TestPlayer";
std::uint64_t g_player_id = 76561198000000001ULL;

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

extern "C" ftRequest_t* COD4X_CALL Plugin_HTTP_MakeHttpRequest(
    const char*,
    const char*,
    byte*,
    int,
    const char*)
{
    return nullptr;
}

extern "C" int COD4X_CALL Plugin_HTTP_SendReceiveData(ftRequest_t*)
{
    return -1;
}

extern "C" void COD4X_CALL Plugin_HTTP_FreeObj(ftRequest_t*)
{
}

extern "C" int COD4X_CALL Plugin_GetSlotCount()
{
    return 64;
}

extern "C" char* COD4X_CALL Plugin_GetPlayerName(int)
{
    return g_player_name.data();
}

extern "C" std::uint64_t COD4X_CALL Plugin_GetPlayerID(unsigned int)
{
    return g_player_id;
}

extern "C" unsigned int COD4X_CALL Plugin_GetClientNumForClient(client_t*)
{
    return 0;
}

extern "C" clientScoreboard_t COD4X_CALL Plugin_GetClientScoreboard(int)
{
    return clientScoreboard_t{10, 0, 0, 0};
}

extern "C" const char* COD4X_CALL Plugin_NET_AdrToStringShortMT(netadr_t*, char* buf, int len)
{
    if (buf != nullptr && len > 0)
    {
        std::snprintf(buf, static_cast<std::size_t>(len), "%s", "127.0.0.1:28960");
        return buf;
    }

    return "127.0.0.1:28960";
}

extern "C" void COD4X_CALL Plugin_Cvar_VariableStringBuffer(const char* cvarname, char* buff, size_t size)
{
    if (buff == nullptr || size == 0)
    {
        return;
    }

    const char* value = "";
    if (cvarname != nullptr)
    {
        if (std::strcmp(cvarname, "mapname") == 0)
        {
            value = "mp_crash";
        }
        else if (std::strcmp(cvarname, "gamename") == 0)
        {
            value = "Call of Duty 4";
        }
        else if (std::strcmp(cvarname, "sv_hostname") == 0)
        {
            value = "XI Test Server";
        }
        else if (std::strcmp(cvarname, "fs_game") == 0)
        {
            value = "mods/xtreme";
        }
    }

    std::snprintf(buff, size, "%s", value);
}

extern "C"
{
int OnInit();
void OnFrame();
void OnClientAuthorized();
void OnMessageSent(char* message, int slot, qboolean* show, int mode);
void OnSpawnServer();
void OnExitLevel();
void OnPlayerConnect(int clientnum, netadr_t* netaddress, char* pbguid, char* userinfo, int authstatus, char* deniedmsg, int deniedmsgbufmaxlen);
void OnClientEnterWorld(client_t* client);
void OnPlayerDC(client_t* client, const char* reason);
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

    qboolean show = qtrue;
    char chatMessage[] = "Hello world";
    netadr_t playerAddress{};
    playerAddress.type = NA_IP;
    playerAddress.address.ip[0] = 127;
    playerAddress.address.ip[1] = 0;
    playerAddress.address.ip[2] = 0;
    playerAddress.address.ip[3] = 1;
    client_t fakeClient{};

    OnMessageSent(chatMessage, 0, &show, 0);
    OnSpawnServer();
    OnExitLevel();
    OnPlayerConnect(0, &playerAddress, nullptr, nullptr, 0, nullptr, 0);
    OnClientEnterWorld(&fakeClient);
    OnPlayerDC(&fakeClient, "quit");

    AssertTrue(g_logs.size() >= logCountAfterInit, "OnFrame and OnClientAuthorized should be safe to invoke.");

    std::cout << "All plugin export tests passed.\n";
    return 0;
}
