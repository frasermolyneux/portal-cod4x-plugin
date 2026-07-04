#include "portal_cod4x/plugin_runtime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace portal_cod4x
{
namespace
{
// Maximum wall-clock time a single in-flight HTTP request may stay pending before
// the refresh pipeline abandons it (and frees the handle) to avoid a stalled poll
// wedging future refreshes.
constexpr std::int64_t kHttpRequestDeadlineSeconds = 30;
constexpr std::int64_t kIngestRequestDeadlineSeconds = 20;
constexpr std::int64_t kServerStatusIntervalSeconds = 60;
constexpr std::int64_t kMaxBufferedEventAgeSeconds = 15 * 60;
constexpr std::size_t kMaxBufferedEventAttempts = 8;

constexpr std::size_t kMaxBufferedEvents = 1000;
constexpr std::size_t kIngestMaxBatchEvents = 100;
constexpr std::size_t kIngestMaxBatchBytes = 200 * 1024;

constexpr std::string_view kQueuePlayerConnected = "player-connected";
constexpr std::string_view kQueuePlayerDisconnected = "player-disconnected";
constexpr std::string_view kQueueChatMessage = "chat-message";
constexpr std::string_view kQueueServerConnected = "server-connected";
constexpr std::string_view kQueueMapChange = "map-change";
constexpr std::string_view kQueueServerStatus = "server-status";
constexpr std::int64_t kCrossCallbackDedupWindowSeconds = 1;
constexpr std::string_view kCommandsCommandPrefix = "!commands";
constexpr std::string_view kWhoAmICommandPrefix = "!whoami";
constexpr std::string_view kRegisterCommandPrefix = "!register";
constexpr std::string_view kFuCommandPrefix = "!fu";
constexpr std::string_view kLikeCommandPrefix = "!like";
constexpr std::string_view kDislikeCommandPrefix = "!dislike";

struct CommandOverride
{
    std::optional<bool> Enabled;
    std::optional<int> MinPower;
};

struct Cod4xCommandDocument
{
    std::optional<bool> Enabled;
    std::unordered_map<std::string, CommandOverride> CommandOverrides;
};

std::string Trim(std::string value)
{
    const auto isWhitespace = [](unsigned char c) { return std::isspace(c) != 0; };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c) { return !isWhitespace(c); }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), [&](unsigned char c) { return !isWhitespace(c); }).base(),
        value.end());

    return value;
}

bool TryReadFile(const std::string& path, std::string& content)
{
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

std::string JsonUnescape(std::string_view escaped)
{
    std::string unescaped;
    unescaped.reserve(escaped.size());

    for (std::size_t i = 0; i < escaped.size(); ++i)
    {
        const char c = escaped[i];
        if (c != '\\' || i + 1 >= escaped.size())
        {
            unescaped.push_back(c);
            continue;
        }

        const char next = escaped[++i];
        switch (next)
        {
            case '\\':
                unescaped.push_back('\\');
                break;
            case '"':
                unescaped.push_back('"');
                break;
            case 'n':
                unescaped.push_back('\n');
                break;
            case 'r':
                unescaped.push_back('\r');
                break;
            case 't':
                unescaped.push_back('\t');
                break;
            default:
                unescaped.push_back(next);
                break;
        }
    }

    return unescaped;
}

std::optional<std::string> ExtractJsonStringValue(const std::string& json, const std::string& key)
{
    const std::regex pattern("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
    std::smatch match;
    if (!std::regex_search(json, match, pattern))
    {
        return std::nullopt;
    }

    return JsonUnescape(match[1].str());
}

std::optional<int> ExtractJsonIntValue(const std::string& json, const std::string& key)
{
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?[0-9]+)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern))
    {
        return std::nullopt;
    }

    return std::stoi(match[1].str());
}

std::optional<std::optional<bool>> ExtractJsonOptionalBool(const std::string& json, const std::string& key)
{
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false|null)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern))
    {
        return std::nullopt;
    }

    const std::string value = match[1].str();
    if (value == "null")
    {
        return std::optional<bool>{};
    }

    return std::optional<bool>{value == "true"};
}

std::size_t FindMatchingBrace(const std::string& input, std::size_t openBraceIndex)
{
    int depth = 0;
    bool inString = false;

    for (std::size_t i = openBraceIndex; i < input.size(); ++i)
    {
        const char c = input[i];

        if (c == '"' && (i == 0 || input[i - 1] != '\\'))
        {
            inString = !inString;
            continue;
        }

        if (inString)
        {
            continue;
        }

        if (c == '{')
        {
            depth++;
        }
        else if (c == '}')
        {
            depth--;
            if (depth == 0)
            {
                return i;
            }
        }
    }

    return std::string::npos;
}

std::optional<std::string> ExtractJsonObjectByKey(const std::string& json, const std::string& key)
{
    const std::string token = "\"" + key + "\"";
    const std::size_t keyPos = json.find(token);
    if (keyPos == std::string::npos)
    {
        return std::nullopt;
    }

    const std::size_t openBrace = json.find('{', keyPos + token.size());
    if (openBrace == std::string::npos)
    {
        return std::nullopt;
    }

    const std::size_t closeBrace = FindMatchingBrace(json, openBrace);
    if (closeBrace == std::string::npos)
    {
        return std::nullopt;
    }

    return json.substr(openBrace, closeBrace - openBrace + 1);
}

std::optional<std::string> ExtractJsonArrayByKey(const std::string& json, const std::string& key)
{
    const std::string token = "\"" + key + "\"";
    const std::size_t keyPos = json.find(token);
    if (keyPos == std::string::npos)
    {
        return std::nullopt;
    }

    const std::size_t openBracket = json.find('[', keyPos + token.size());
    if (openBracket == std::string::npos)
    {
        return std::nullopt;
    }

    bool inString = false;
    int depth = 0;
    for (std::size_t i = openBracket; i < json.size(); ++i)
    {
        const char c = json[i];

        if (c == '"' && (i == 0 || json[i - 1] != '\\'))
        {
            inString = !inString;
            continue;
        }

        if (inString)
        {
            continue;
        }

        if (c == '[')
        {
            depth++;
        }
        else if (c == ']')
        {
            depth--;
            if (depth == 0)
            {
                return json.substr(openBracket, i - openBracket + 1);
            }
        }
    }

    return std::nullopt;
}

std::vector<std::string> ParseJsonStringArray(const std::string& jsonArray)
{
    std::vector<std::string> output;

    if (jsonArray.size() < 2 || jsonArray.front() != '[' || jsonArray.back() != ']')
    {
        return output;
    }

    std::size_t cursor = 1;
    while (cursor < jsonArray.size() - 1)
    {
        const std::size_t stringOpen = jsonArray.find('"', cursor);
        if (stringOpen == std::string::npos)
        {
            break;
        }

        const std::size_t stringClose = jsonArray.find('"', stringOpen + 1);
        if (stringClose == std::string::npos)
        {
            break;
        }

        output.push_back(JsonUnescape(jsonArray.substr(stringOpen + 1, stringClose - stringOpen - 1)));
        cursor = stringClose + 1;
    }

    return output;
}

std::string UrlEncode(const std::string& value)
{
    static constexpr auto kHexChars = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);

    for (unsigned char c : value)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            encoded.push_back(static_cast<char>(c));
            continue;
        }

        encoded.push_back('%');
        encoded.push_back(kHexChars[(c >> 4) & 0x0F]);
        encoded.push_back(kHexChars[c & 0x0F]);
    }

    return encoded;
}

std::string NormalizeBaseUrl(std::string baseUrl)
{
    while (!baseUrl.empty() && baseUrl.back() == '/')
    {
        baseUrl.pop_back();
    }

    return baseUrl;
}

std::string ResolveCanonicalCommand(std::string command)
{
    static const std::unordered_map<std::string, std::string> kAliases{
        {"authSetAdmin", "AdminAddAdmin"},
        {"AdminAddAdminWithPassword", "AdminAddAdmin"},
        {"authChangePassword", "ChangePassword"},
        {"authListAdmins", "AdminListAdmins"},
        {"authUnsetAdmin", "AdminRemoveAdmin"},
        {"cmdpowerlist", "AdminListCommands"},
        {"setCmdMinPower", "AdminChangeCommandPower"},
        {"kickid", "kick"},
        {"clientkick", "kick"},
        {"onlykick", "kick"},
        {"unbanUser", "unban"},
        {"banUser", "permban"},
        {"banClient", "permban"},
        {"consay", "say"},
        {"contell", "tell"}};

    const auto it = kAliases.find(command);
    if (it != kAliases.end())
    {
        return it->second;
    }

    return command;
}

std::unordered_map<std::string, CommandPowerRule> BuildDefaultCommandPowerRules()
{
    static const std::array<std::pair<const char*, int>, 30> kDefaults{{
        {"cmdlist", 1},
        {"systeminfo", 1},
        {"serverinfo", 1},
        {"ministatus", 1},
        {"ChangePassword", 10},
        {"kick", 35},
        {"getmodules", 45},
        {"getss", 45},
        {"map_restart", 50},
        {"tempban", 50},
        {"dumpuser", 50},
        {"record", 50},
        {"map", 60},
        {"undercover", 60},
        {"say", 70},
        {"screensay", 70},
        {"tell", 70},
        {"screentell", 70},
        {"stoprecord", 70},
        {"gametype", 80},
        {"unban", 80},
        {"permban", 80},
        {"AdminListAdmins", 80},
        {"AdminListCommands", 95},
        {"AdminRemoveAdmin", 95},
        {"AdminAddAdmin", 95},
        {"AdminChangePassword", 95},
        {"exec", 98},
        {"set", 98},
        {"cvarlist", 98},
    }};

    std::unordered_map<std::string, CommandPowerRule> rules;
    rules.reserve(kDefaults.size() + 1);

    for (const auto& [command, minPower] : kDefaults)
    {
        rules.emplace(command, CommandPowerRule{true, minPower});
    }

    rules.emplace("AdminChangeCommandPower", CommandPowerRule{true, 98});

    return rules;
}

std::optional<Cod4xCommandDocument> ParseCod4xCommandDocument(const std::string& documentJson)
{
    Cod4xCommandDocument document;

    const auto enabledToken = ExtractJsonOptionalBool(documentJson, "enabled");
    if (enabledToken.has_value())
    {
        document.Enabled = *enabledToken;
    }

    const auto commandsObject = ExtractJsonObjectByKey(documentJson, "commands");
    if (!commandsObject.has_value())
    {
        return document;
    }

    std::size_t cursor = 1;
    while (cursor < commandsObject->size())
    {
        const std::size_t keyOpen = commandsObject->find('"', cursor);
        if (keyOpen == std::string::npos)
        {
            break;
        }

        const std::size_t keyClose = commandsObject->find('"', keyOpen + 1);
        if (keyClose == std::string::npos)
        {
            break;
        }

        const std::string commandKey = JsonUnescape(commandsObject->substr(keyOpen + 1, keyClose - keyOpen - 1));
        const std::size_t objectOpen = commandsObject->find('{', keyClose);
        if (objectOpen == std::string::npos)
        {
            break;
        }

        const std::size_t objectClose = FindMatchingBrace(*commandsObject, objectOpen);
        if (objectClose == std::string::npos)
        {
            break;
        }

        const std::string entryJson = commandsObject->substr(objectOpen, objectClose - objectOpen + 1);
        CommandOverride entry;

        const auto entryEnabled = ExtractJsonOptionalBool(entryJson, "enabled");
        if (entryEnabled.has_value())
        {
            entry.Enabled = *entryEnabled;
        }

        const auto minPower = ExtractJsonIntValue(entryJson, "minPower");
        if (minPower.has_value())
        {
            entry.MinPower = *minPower;
        }

        document.CommandOverrides[ResolveCanonicalCommand(commandKey)] = entry;
        cursor = objectClose + 1;
    }

    return document;
}

std::optional<PluginConfig> ParsePluginConfig(const std::string& configJson)
{
    auto tenantId = ExtractJsonStringValue(configJson, "tenantId");
    auto clientId = ExtractJsonStringValue(configJson, "clientId");
    auto clientSecret = ExtractJsonStringValue(configJson, "clientSecret");
    auto repositoryApiBaseUrl = ExtractJsonStringValue(configJson, "repositoryApiBaseUrl");
    auto repositoryApiResource = ExtractJsonStringValue(configJson, "repositoryApiResource");
    auto gameServerId = ExtractJsonStringValue(configJson, "gameServerId");
    auto ingestBaseUrl = ExtractJsonStringValue(configJson, "ingestBaseUrl");
    auto ingestApiResource = ExtractJsonStringValue(configJson, "ingestApiResource");
    auto gameType = ExtractJsonStringValue(configJson, "gameType");

    if (!tenantId.has_value() || !clientId.has_value() || !clientSecret.has_value() || !repositoryApiBaseUrl.has_value() ||
        !repositoryApiResource.has_value() || !gameServerId.has_value())
    {
        return std::nullopt;
    }

    PluginConfig config;
    config.TenantId = Trim(*tenantId);
    config.ClientId = Trim(*clientId);
    config.ClientSecret = Trim(*clientSecret);
    config.RepositoryApiBaseUrl = NormalizeBaseUrl(Trim(*repositoryApiBaseUrl));
    config.RepositoryApiResource = Trim(*repositoryApiResource);
    config.GameServerId = Trim(*gameServerId);

    if (ingestBaseUrl.has_value())
    {
        config.IngestBaseUrl = NormalizeBaseUrl(Trim(*ingestBaseUrl));
    }

    if (ingestApiResource.has_value())
    {
        config.IngestApiResource = Trim(*ingestApiResource);
    }

    if (gameType.has_value())
    {
        config.GameType = Trim(*gameType);
    }

    const auto refreshInterval = ExtractJsonIntValue(configJson, "refreshIntervalSeconds");
    if (refreshInterval.has_value())
    {
        config.RefreshIntervalSeconds = std::clamp(*refreshInterval, 15, 900);
    }

    return config;
}

std::optional<std::string> ExtractConfigurationPayload(const std::string& responseBody)
{
    const auto configuration = ExtractJsonStringValue(responseBody, "configuration");
    if (configuration.has_value())
    {
        return configuration;
    }

    if (responseBody.find("\"commands\"") != std::string::npos)
    {
        return responseBody;
    }

    return std::nullopt;
}


std::string BuildSnapshotHash(const EffectiveServerContext& context)
{
    std::vector<std::string> keys;
    keys.reserve(context.CommandRules.size());
    for (const auto& [key, _] : context.CommandRules)
    {
        keys.push_back(key);
    }

    std::sort(keys.begin(), keys.end());

    std::string hash = context.CommandEnforcementEnabled ? "1|" : "0|";
    hash += context.GameServerId;
    hash.push_back('|');

    for (const auto& key : keys)
    {
        const auto& rule = context.CommandRules.at(key);
        hash += key;
        hash.push_back('=');
        hash += rule.Enabled ? "1" : "0";
        hash.push_back(':');
        hash += std::to_string(rule.MinPower);
        hash.push_back(';');
    }

    return hash;
}

std::string BuildAuthorizationHeaders(const std::string& accessToken)
{
    return "Authorization: Bearer " + accessToken + "\r\nAccept: application/json\r\n";
}

bool IsSafeCommandIdentifier(std::string_view command)
{
    if (command.empty())
    {
        return false;
    }

    for (const unsigned char c : command)
    {
        if (!std::isalnum(c) && c != '_')
        {
            return false;
        }
    }

    return true;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const auto leftChar = static_cast<unsigned char>(left[index]);
        const auto rightChar = static_cast<unsigned char>(right[index]);
        if (std::tolower(leftChar) != std::tolower(rightChar))
        {
            return false;
        }
    }

    return true;
}

std::string ExtractCommandToken(std::string_view command)
{
    const std::string trimmed = Trim(std::string(command));
    if (trimmed.empty())
    {
        return {};
    }

    const std::size_t tokenEnd = trimmed.find_first_of(" \t\r\n");
    if (tokenEnd == std::string::npos)
    {
        return trimmed;
    }

    return trimmed.substr(0, tokenEnd);
}
}

std::string BuildOnlineBroadcastMessage(std::string_view prefix, std::string_view version)
{
    std::string normalizedPrefix = prefix.empty() ? std::string(kDefaultBotPrefix) : std::string(prefix);
    std::string normalizedVersion = version.empty() ? "0.0.0-unknown" : std::string(version);

    return normalizedPrefix + " Portal Plugin is online (version " + normalizedVersion + ")";
}

PluginRuntime::PluginRuntime(std::string configPath)
    : configPath(std::move(configPath))
{
}

std::string PluginRuntime::BuildPrefixedChatMessage(std::string_view message) const
{
    if (chatPrefix.empty())
    {
        return std::string(message);
    }

    std::string rendered;
    rendered.reserve(chatPrefix.size() + 1 + message.size());
    rendered += chatPrefix;
    rendered.push_back(' ');
    rendered.append(message.data(), message.size());
    return rendered;
}

void PluginRuntime::SendPrivateChat(ICod4xHost& host, int slot, std::string_view message) const
{
    if (slot < 0)
    {
        return;
    }

    host.SendChat(slot, BuildPrefixedChatMessage(message));
}

int PluginRuntime::Initialize(ICod4xHost& host, std::string_view version, std::string_view prefix)
{
    const std::string onlineMessage = BuildOnlineBroadcastMessage(prefix, version);
    const std::string normalizedVersion = version.empty() ? "0.0.0-unknown" : std::string(version);
    chatPrefix = prefix.empty() ? std::string(kDefaultBotPrefix) : std::string(prefix);
    pluginVersion = normalizedVersion;

    host.Log("Portal Plugin is online (version " + normalizedVersion + ")");
    host.BroadcastChat(onlineMessage);

    const std::int64_t nowUnixSeconds = host.GetUnixTimeSeconds();
    TryLoadConfig(host, nowUnixSeconds);
    nextRefreshUnixSeconds = 0;
    nextServerStatusUnixSeconds = nowUnixSeconds + kServerStatusIntervalSeconds;

    return 0;
}

void PluginRuntime::Tick(ICod4xHost& host)
{
    const std::int64_t nowUnixSeconds = host.GetUnixTimeSeconds();

    if (!loadedConfig.has_value())
    {
        if (nowUnixSeconds < nextConfigLoadAttemptUnixSeconds)
        {
            return;
        }

        if (!TryLoadConfig(host, nowUnixSeconds))
        {
            return;
        }

        nextServerStatusUnixSeconds = nowUnixSeconds + kServerStatusIntervalSeconds;
    }

    if (refreshStage != RefreshStage::Idle)
    {
        AdvanceRefresh(host, nowUnixSeconds);
    }
    else if (nowUnixSeconds >= nextRefreshUnixSeconds)
    {
        BeginRefresh(host, nowUnixSeconds);
    }

    if (IsIngestConfigured() && nowUnixSeconds >= nextServerStatusUnixSeconds)
    {
        FlushServerStatusSnapshot(host, nowUnixSeconds);
        nextServerStatusUnixSeconds = nowUnixSeconds + kServerStatusIntervalSeconds;
    }

    AdvanceIngest(host, nowUnixSeconds);
}

void PluginRuntime::HandlePlayerConnect(ICod4xHost& host, int slot, std::string_view ipAddress)
{
    if (!loadedConfig.has_value() || slot < 0)
    {
        return;
    }

    ConnectedPlayerState& playerState = connectedPlayers[slot];
    playerState.SlotId = slot;
    playerState.IpAddress = NormalizeIpAddress(std::string(ipAddress));
    if (playerState.ConnectedAtUnixSeconds == 0)
    {
        playerState.ConnectedAtUnixSeconds = host.GetUnixTimeSeconds();
    }

    if (playerState.IpAddress.empty())
    {
        playerState.IpAddress = "0.0.0.0";
    }

    if (playerState.Username.empty())
    {
        playerState.Username = Trim(host.GetPlayerName(slot));
    }
}

void PluginRuntime::HandlePlayerConnected(ICod4xHost& host, int slot)
{
    if (!loadedConfig.has_value() || !IsIngestConfigured() || slot < 0)
    {
        return;
    }

    const std::uint64_t playerId = host.GetPlayerId(slot);
    if (playerId == 0)
    {
        return;
    }

    const std::int64_t nowUnixSeconds = host.GetUnixTimeSeconds();
    ConnectedPlayerState& playerState = connectedPlayers[slot];
    playerState.SlotId = slot;
    playerState.PlayerGuid = std::to_string(playerId);
    playerState.SteamId = host.GetPlayerSteamId(slot);
    playerState.Username = Trim(host.GetPlayerName(slot));
    playerState.Score = host.GetPlayerScore(slot);
    playerState.ConnectedAtUnixSeconds = nowUnixSeconds;

    if (playerState.IpAddress.empty())
    {
        playerState.IpAddress = "0.0.0.0";
    }

    if (playerState.Username.empty())
    {
        return;
    }

    const std::string messageId = GenerateMessageId();
    BufferEvent(
        std::string(kQueuePlayerConnected),
        BuildPlayerConnectedPayload(
            nowUnixSeconds,
            messageId,
            playerState.PlayerGuid,
            playerState.SteamId,
            playerState.Username,
            playerState.IpAddress,
            slot),
        messageId,
        nowUnixSeconds);
}

void PluginRuntime::HandleClientAuthorized(ICod4xHost& host)
{
    if (!loadedConfig.has_value())
    {
        return;
    }

    const std::int64_t nowUnixSeconds = host.GetUnixTimeSeconds();
    for (auto& [slot, state] : connectedPlayers)
    {
        if (state.PlayerGuid.empty())
        {
            const std::uint64_t playerId = host.GetPlayerId(slot);
            if (playerId != 0)
            {
                state.PlayerGuid = std::to_string(playerId);
            }
        }

        if (state.Username.empty())
        {
            state.Username = Trim(host.GetPlayerName(slot));
        }

        if (state.SteamId == 0)
        {
            state.SteamId = host.GetPlayerSteamId(slot);
        }

        if (state.ConnectedAtUnixSeconds == 0)
        {
            state.ConnectedAtUnixSeconds = nowUnixSeconds;
        }
    }
}

void PluginRuntime::HandlePlayerDisconnected(ICod4xHost& host, int slot)
{
    if (!loadedConfig.has_value() || !IsIngestConfigured() || slot < 0)
    {
        return;
    }

    auto stateIt = connectedPlayers.find(slot);
    ConnectedPlayerState state;
    if (stateIt != connectedPlayers.end())
    {
        state = stateIt->second;
        connectedPlayers.erase(stateIt);
    }

    if (state.PlayerGuid.empty())
    {
        const std::uint64_t playerId = host.GetPlayerId(slot);
        if (playerId != 0)
        {
            state.PlayerGuid = std::to_string(playerId);
        }
    }

    if (state.Username.empty())
    {
        state.Username = Trim(host.GetPlayerName(slot));
    }

    if (state.PlayerGuid.empty() || state.Username.empty())
    {
        return;
    }

    const std::int64_t nowUnixSeconds = host.GetUnixTimeSeconds();
    const std::string messageId = GenerateMessageId();
    BufferEvent(
        std::string(kQueuePlayerDisconnected),
        BuildPlayerDisconnectedPayload(nowUnixSeconds, messageId, state.PlayerGuid, state.Username, slot),
        messageId,
        nowUnixSeconds);
}

void PluginRuntime::HandleChatMessage(ICod4xHost& host, int slot, std::string_view message, bool teamMessage)
{
    if (slot < 0)
    {
        return;
    }

    const std::string trimmedMessage = Trim(std::string(message));
    if (trimmedMessage.empty())
    {
        return;
    }

    if (trimmedMessage.front() == '!')
    {
        HandleClientCommand(host, slot, trimmedMessage, true);
    }

    if (!loadedConfig.has_value() || !IsIngestConfigured())
    {
        return;
    }

    ConnectedPlayerState& playerState = connectedPlayers[slot];
    playerState.SlotId = slot;
    if (playerState.ConnectedAtUnixSeconds == 0)
    {
        playerState.ConnectedAtUnixSeconds = host.GetUnixTimeSeconds();
    }

    if (playerState.PlayerGuid.empty())
    {
        const std::uint64_t playerId = host.GetPlayerId(slot);
        if (playerId != 0)
        {
            playerState.PlayerGuid = std::to_string(playerId);
        }
    }

    if (playerState.Username.empty())
    {
        playerState.Username = Trim(host.GetPlayerName(slot));
    }

    if (playerState.PlayerGuid.empty() || playerState.Username.empty())
    {
        return;
    }

    const std::int64_t nowUnixSeconds = host.GetUnixTimeSeconds();
    const std::string messageId = GenerateMessageId();
    BufferEvent(
        std::string(kQueueChatMessage),
        BuildChatMessagePayload(
            nowUnixSeconds,
            messageId,
            playerState.PlayerGuid,
            playerState.Username,
            slot,
            trimmedMessage,
            teamMessage),
        messageId,
        nowUnixSeconds);
}

void PluginRuntime::HandleClientCommand(ICod4xHost& host, int slot, std::string_view command, bool fromChatMessage)
{
    if (slot < 0)
    {
        return;
    }

    const std::string commandToken = ExtractCommandToken(command);
    if (commandToken.empty() || commandToken.front() != '!')
    {
        return;
    }

    const std::int64_t nowUnixSeconds = host.GetUnixTimeSeconds();
    const bool withinCrossCallbackDedupWindow =
        nowUnixSeconds >= lastHandledCommandUnixSeconds &&
        (nowUnixSeconds - lastHandledCommandUnixSeconds) <= kCrossCallbackDedupWindowSeconds;

    if (lastHandledCommandSlot == slot &&
        withinCrossCallbackDedupWindow &&
        lastHandledCommandFromChat != fromChatMessage &&
        EqualsIgnoreCase(lastHandledCommandToken, commandToken))
    {
        return;
    }

    const auto markHandled = [&]() {
        lastHandledCommandToken = commandToken;
        lastHandledCommandSlot = slot;
        lastHandledCommandUnixSeconds = nowUnixSeconds;
        lastHandledCommandFromChat = fromChatMessage;
    };

    if (EqualsIgnoreCase(commandToken, kCommandsCommandPrefix))
    {
        markHandled();
        SendPrivateChat(host, slot, "Available commands: !commands, !whoami, !register, !fu, !like, !dislike");
        return;
    }

    if (EqualsIgnoreCase(commandToken, kWhoAmICommandPrefix))
    {
        markHandled();
        SendPrivateChat(host, slot, "Plugin command processing active.");
        return;
    }

    if (EqualsIgnoreCase(commandToken, kRegisterCommandPrefix))
    {
        markHandled();
        SendPrivateChat(host, slot, "!register is not available from the plugin path yet.");
        return;
    }

    if (EqualsIgnoreCase(commandToken, kFuCommandPrefix))
    {
        markHandled();
        SendPrivateChat(host, slot, "!fu is handled by backend services only.");
        return;
    }

    if (EqualsIgnoreCase(commandToken, kLikeCommandPrefix))
    {
        markHandled();
        SendPrivateChat(host, slot, "Map vote registered: like.");
        return;
    }

    if (EqualsIgnoreCase(commandToken, kDislikeCommandPrefix))
    {
        markHandled();
        SendPrivateChat(host, slot, "Map vote registered: dislike.");
    }
}

void PluginRuntime::HandleServerSpawned(ICod4xHost& host)
{
    if (!loadedConfig.has_value() || !IsIngestConfigured())
    {
        return;
    }

    const std::int64_t nowUnixSeconds = host.GetUnixTimeSeconds();

    const std::string serverConnectedMessageId = GenerateMessageId();
    BufferEvent(
        std::string(kQueueServerConnected),
        BuildServerConnectedPayload(nowUnixSeconds, serverConnectedMessageId),
        serverConnectedMessageId,
        nowUnixSeconds);

    const std::string mapChangeMessageId = GenerateMessageId();
    BufferEvent(
        std::string(kQueueMapChange),
        BuildMapChangePayload(nowUnixSeconds, mapChangeMessageId, GetMapName(host), GetGameName(host)),
        mapChangeMessageId,
        nowUnixSeconds);

    nextServerStatusUnixSeconds = nowUnixSeconds;
}

void PluginRuntime::HandleServerExited(ICod4xHost&)
{
    connectedPlayers.clear();
}

const EffectiveServerContext& PluginRuntime::GetServerContext() const
{
    return serverContext;
}

bool PluginRuntime::TryLoadConfig(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    const auto logConfigIssue = [&](const std::string& issue) {
        if (issue != lastConfigLoadError)
        {
            host.Log(issue);
            lastConfigLoadError = issue;
        }

        nextConfigLoadAttemptUnixSeconds = nowUnixSeconds + 30;
    };

    std::string configJson;
    if (!TryReadFile(configPath, configJson))
    {
        logConfigIssue("plugin config not found at " + configPath + "; settings retrieval is disabled until config is present");
        return false;
    }

    const auto parsedConfig = ParsePluginConfig(configJson);
    if (!parsedConfig.has_value())
    {
        logConfigIssue(
            "plugin config is invalid; expected tenantId, clientId, clientSecret, repositoryApiBaseUrl, repositoryApiResource, and gameServerId");
        return false;
    }

    loadedConfig = parsedConfig;
    serverContext.GameServerId = loadedConfig->GameServerId;
    lastConfigLoadError.clear();
    nextConfigLoadAttemptUnixSeconds = nowUnixSeconds;
    host.Log("plugin config loaded for gameServerId " + loadedConfig->GameServerId);
    return true;
}

bool PluginRuntime::IsAccessTokenValid(std::int64_t nowUnixSeconds) const
{
    return !accessToken.empty() && nowUnixSeconds + 30 < accessTokenExpiresAtUnixSeconds;
}

void PluginRuntime::BeginRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    pendingHasGlobalConfig = false;
    pendingHasServerConfig = false;
    pendingGlobalConfigPayload.clear();
    pendingServerConfigPayload.clear();

    // Reuse a still-valid access token and skip straight to the config requests.
    if (IsAccessTokenValid(nowUnixSeconds))
    {
        if (!StartGlobalConfigRequest(host, nowUnixSeconds))
        {
            AbortRefresh(host, nowUnixSeconds, "failed to start global cod4xCommands request");
        }

        return;
    }

    const std::string tokenUrl = "https://login.microsoftonline.com/" + loadedConfig->TenantId + "/oauth2/v2.0/token";
    const std::string tokenBody = "grant_type=client_credentials&client_id=" + UrlEncode(loadedConfig->ClientId) +
        "&client_secret=" + UrlEncode(loadedConfig->ClientSecret) +
        "&scope=" + UrlEncode(loadedConfig->RepositoryApiResource + "/.default");

    inFlightRequest = host.BeginHttpRequest(
        tokenUrl,
        "POST",
        tokenBody,
        "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n");

    if (inFlightRequest == nullptr)
    {
        AbortRefresh(host, nowUnixSeconds, "failed to start access token request");
        return;
    }

    inFlightStartedUnixSeconds = nowUnixSeconds;
    refreshStage = RefreshStage::AcquiringToken;
}

bool PluginRuntime::StartGlobalConfigRequest(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    const std::string authHeaders = BuildAuthorizationHeaders(accessToken);
    const std::string globalUrl = loadedConfig->RepositoryApiBaseUrl + "/v1.0/configurations/cod4xCommands";

    inFlightRequest = host.BeginHttpRequest(globalUrl, "GET", "", authHeaders);
    if (inFlightRequest == nullptr)
    {
        return false;
    }

    inFlightStartedUnixSeconds = nowUnixSeconds;
    refreshStage = RefreshStage::FetchingGlobalConfig;
    return true;
}

bool PluginRuntime::StartServerConfigRequest(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    const std::string authHeaders = BuildAuthorizationHeaders(accessToken);
    const std::string serverUrl =
        loadedConfig->RepositoryApiBaseUrl + "/v1.0/game-servers/" + loadedConfig->GameServerId + "/configurations/cod4xCommands";

    inFlightRequest = host.BeginHttpRequest(serverUrl, "GET", "", authHeaders);
    if (inFlightRequest == nullptr)
    {
        return false;
    }

    inFlightStartedUnixSeconds = nowUnixSeconds;
    refreshStage = RefreshStage::FetchingServerConfig;
    return true;
}

void PluginRuntime::AdvanceRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    if (inFlightRequest == nullptr)
    {
        refreshStage = RefreshStage::Idle;
        return;
    }

    HttpResponse response;
    const HttpRequestStatus status = host.PollHttpRequest(inFlightRequest, response);
    if (status == HttpRequestStatus::Pending)
    {
        if (nowUnixSeconds - inFlightStartedUnixSeconds >= kHttpRequestDeadlineSeconds)
        {
            AbortRefresh(host, nowUnixSeconds, "cod4xCommands settings request timed out");
        }

        return;
    }

    host.EndHttpRequest(inFlightRequest);
    inFlightRequest = nullptr;

    if (status == HttpRequestStatus::Failed)
    {
        AbortRefresh(host, nowUnixSeconds, "cod4xCommands settings request failed");
        return;
    }

    switch (refreshStage)
    {
        case RefreshStage::AcquiringToken:
        {
            if (response.StatusCode != 200)
            {
                AbortRefresh(host, nowUnixSeconds, "failed to acquire access token for repository API");
                return;
            }

            const auto parsedAccessToken = ExtractJsonStringValue(response.Body, "access_token");
            if (!parsedAccessToken.has_value() || parsedAccessToken->empty())
            {
                AbortRefresh(host, nowUnixSeconds, "token response missing access_token");
                return;
            }

            const int expiresInSeconds = ExtractJsonIntValue(response.Body, "expires_in").value_or(3600);
            accessToken = *parsedAccessToken;
            accessTokenExpiresAtUnixSeconds = nowUnixSeconds + std::max(0, expiresInSeconds - 30);

            if (!StartGlobalConfigRequest(host, nowUnixSeconds))
            {
                AbortRefresh(host, nowUnixSeconds, "failed to start global cod4xCommands request");
            }

            break;
        }
        case RefreshStage::FetchingGlobalConfig:
        {
            if (response.StatusCode != 200 && response.StatusCode != 404)
            {
                AbortRefresh(host, nowUnixSeconds, "failed to retrieve global cod4xCommands settings");
                return;
            }

            if (response.StatusCode == 200)
            {
                const auto payload = ExtractConfigurationPayload(response.Body);
                if (!payload.has_value())
                {
                    AbortRefresh(host, nowUnixSeconds, "global cod4xCommands payload missing configuration");
                    return;
                }

                pendingGlobalConfigPayload = *payload;
                pendingHasGlobalConfig = true;
            }

            if (!StartServerConfigRequest(host, nowUnixSeconds))
            {
                AbortRefresh(host, nowUnixSeconds, "failed to start server cod4xCommands request");
            }

            break;
        }
        case RefreshStage::FetchingServerConfig:
        {
            if (response.StatusCode != 200 && response.StatusCode != 404)
            {
                AbortRefresh(host, nowUnixSeconds, "failed to retrieve server cod4xCommands settings");
                return;
            }

            if (response.StatusCode == 200)
            {
                const auto payload = ExtractConfigurationPayload(response.Body);
                if (!payload.has_value())
                {
                    AbortRefresh(host, nowUnixSeconds, "server cod4xCommands payload missing configuration");
                    return;
                }

                pendingServerConfigPayload = *payload;
                pendingHasServerConfig = true;
            }
            FinalizeRefresh(host, nowUnixSeconds);
            break;
        }
        case RefreshStage::Idle:
        default:
            refreshStage = RefreshStage::Idle;
            break;
    }
}

void PluginRuntime::AbortRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds, std::string_view reason)
{
    if (!reason.empty())
    {
        host.Log(std::string(reason));
    }

    if (inFlightRequest != nullptr)
    {
        host.EndHttpRequest(inFlightRequest);
        inFlightRequest = nullptr;
    }

    refreshStage = RefreshStage::Idle;
    nextRefreshUnixSeconds = nowUnixSeconds + 30;
}

void PluginRuntime::FinalizeRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    Cod4xCommandDocument globalDoc;
    Cod4xCommandDocument serverDoc;
    bool hasGlobalDoc = false;
    bool hasServerDoc = false;

    if (pendingHasGlobalConfig)
    {
        const auto parsed = ParseCod4xCommandDocument(pendingGlobalConfigPayload);
        if (!parsed.has_value())
        {
            AbortRefresh(host, nowUnixSeconds, "global cod4xCommands payload parse failed");
            return;
        }

        globalDoc = *parsed;
        hasGlobalDoc = true;
    }

    if (pendingHasServerConfig)
    {
        const auto parsed = ParseCod4xCommandDocument(pendingServerConfigPayload);
        if (!parsed.has_value())
        {
            AbortRefresh(host, nowUnixSeconds, "server cod4xCommands payload parse failed");
            return;
        }

        serverDoc = *parsed;
        hasServerDoc = true;
    }

    EffectiveServerContext nextContext;
    nextContext.GameServerId = loadedConfig->GameServerId;
    nextContext.CommandRules = BuildDefaultCommandPowerRules();

    if (hasGlobalDoc)
    {
        for (const auto& [command, overrideEntry] : globalDoc.CommandOverrides)
        {
            const std::string canonicalCommand = ResolveCanonicalCommand(command);
            const auto ruleIt = nextContext.CommandRules.find(canonicalCommand);
            if (ruleIt == nextContext.CommandRules.end())
            {
                continue;
            }

            auto& rule = ruleIt->second;

            if (overrideEntry.Enabled.has_value())
            {
                rule.Enabled = *overrideEntry.Enabled;
            }

            if (overrideEntry.MinPower.has_value())
            {
                rule.MinPower = std::clamp(*overrideEntry.MinPower, 1, 100);
            }
        }
    }

    if (hasServerDoc)
    {
        for (const auto& [command, overrideEntry] : serverDoc.CommandOverrides)
        {
            const std::string canonicalCommand = ResolveCanonicalCommand(command);
            const auto ruleIt = nextContext.CommandRules.find(canonicalCommand);
            if (ruleIt == nextContext.CommandRules.end())
            {
                continue;
            }

            auto& rule = ruleIt->second;

            if (overrideEntry.Enabled.has_value())
            {
                rule.Enabled = *overrideEntry.Enabled;
            }

            if (overrideEntry.MinPower.has_value())
            {
                rule.MinPower = std::clamp(*overrideEntry.MinPower, 1, 100);
            }
        }
    }

    const std::optional<bool> globalEnabled = hasGlobalDoc ? globalDoc.Enabled : std::optional<bool>{};
    const std::optional<bool> serverEnabled = hasServerDoc ? serverDoc.Enabled : std::optional<bool>{};
    nextContext.CommandEnforcementEnabled = serverEnabled.value_or(globalEnabled.value_or(false));
    nextContext.LastRefreshUnixSeconds = nowUnixSeconds;
    nextContext.SnapshotHash = BuildSnapshotHash(nextContext);

    const bool wasEnforced = serverContext.CommandEnforcementEnabled;

    serverContext = std::move(nextContext);

    refreshStage = RefreshStage::Idle;

    bool commandApplied = true;

    if (!serverContext.CommandEnforcementEnabled)
    {
        if (wasEnforced)
        {
            host.Log("cod4xCommands enforcement disabled for current server context; command powers left unchanged");
        }

        lastAppliedSnapshotHash.clear();
    }
    else if (serverContext.SnapshotHash != lastAppliedSnapshotHash || !wasEnforced)
    {
        commandApplied = ApplyCommandReconciliation(host);
    }

    nextRefreshUnixSeconds = commandApplied
        ? nowUnixSeconds + loadedConfig->RefreshIntervalSeconds
        : nowUnixSeconds + 30;
}

bool PluginRuntime::ApplyCommandReconciliation(ICod4xHost& host)
{
    if (!serverContext.CommandEnforcementEnabled)
    {
        return true;
    }

    std::vector<std::string> commandNames;
    commandNames.reserve(serverContext.CommandRules.size());
    for (const auto& [command, _] : serverContext.CommandRules)
    {
        commandNames.push_back(command);
    }

    std::sort(commandNames.begin(), commandNames.end());

    int appliedCount = 0;
    bool allApplied = true;
    for (const std::string& commandName : commandNames)
    {
        if (!IsSafeCommandIdentifier(commandName))
        {
            host.Log("skipping unsafe command identifier in cod4xCommands settings");
            allApplied = false;
            continue;
        }

        const auto& rule = serverContext.CommandRules.at(commandName);
        const int effectivePower = rule.Enabled ? std::clamp(rule.MinPower, 1, 100) : 100;
        const std::string command = "setCmdMinPower " + commandName + " " + std::to_string(effectivePower);

        if (host.ExecuteServerCommand(command))
        {
            appliedCount++;
        }
        else
        {
            allApplied = false;
        }
    }

    host.Log("cod4xCommands reconciliation applied " + std::to_string(appliedCount) + " command power updates");

    if (allApplied)
    {
        lastAppliedSnapshotHash = serverContext.SnapshotHash;
        return true;
    }

    return false;
}

bool PluginRuntime::IsIngestConfigured() const
{
    return loadedConfig.has_value() && !loadedConfig->IngestBaseUrl.empty() && !loadedConfig->IngestApiResource.empty() &&
        !loadedConfig->GameType.empty();
}

bool PluginRuntime::IsIngestTokenValid(std::int64_t nowUnixSeconds) const
{
    return !ingestAccessToken.empty() && nowUnixSeconds + 30 < ingestAccessTokenExpiresAtUnixSeconds;
}

void PluginRuntime::AdvanceIngest(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    if (!loadedConfig.has_value())
    {
        return;
    }

    if (!IsIngestConfigured())
    {
        if (!ingestConfigWarningLogged)
        {
            host.Log("ingest egress disabled; configure ingestBaseUrl, ingestApiResource, and gameType to enable event emission");
            ingestConfigWarningLogged = true;
        }

        return;
    }

    ingestConfigWarningLogged = false;

    if (ingestStage == IngestStage::Idle)
    {
        PruneBufferedEvents(host, nowUnixSeconds);
    }

    if (ingestStage != IngestStage::Idle)
    {
        if (ingestRequest == nullptr)
        {
            ingestStage = IngestStage::Idle;
            return;
        }

        HttpResponse response;
        const HttpRequestStatus status = host.PollHttpRequest(ingestRequest, response);
        if (status == HttpRequestStatus::Pending)
        {
            if (nowUnixSeconds - ingestRequestStartedUnixSeconds >= kIngestRequestDeadlineSeconds)
            {
                AbortIngest(host, nowUnixSeconds, "ingest request timed out");
            }

            return;
        }

        host.EndHttpRequest(ingestRequest);
        ingestRequest = nullptr;

        if (status == HttpRequestStatus::Failed)
        {
            AbortIngest(host, nowUnixSeconds, "ingest request failed");
            return;
        }

        if (ingestStage == IngestStage::AcquiringToken)
        {
            if (response.StatusCode != 200)
            {
                AbortIngest(host, nowUnixSeconds, "failed to acquire ingest access token");
                return;
            }

            const auto parsedAccessToken = ExtractJsonStringValue(response.Body, "access_token");
            if (!parsedAccessToken.has_value() || parsedAccessToken->empty())
            {
                AbortIngest(host, nowUnixSeconds, "ingest token response missing access_token");
                return;
            }

            const int expiresInSeconds = ExtractJsonIntValue(response.Body, "expires_in").value_or(3600);
            ingestAccessToken = *parsedAccessToken;
            ingestAccessTokenExpiresAtUnixSeconds = nowUnixSeconds + std::max(0, expiresInSeconds - 30);

            if (!StartIngestBatchRequest(host, nowUnixSeconds))
            {
                AbortIngest(host, nowUnixSeconds, "failed to start ingest batch request");
            }

            return;
        }

        if (ingestStage == IngestStage::PostingBatch)
        {
            if (response.StatusCode >= 200 && response.StatusCode < 300)
            {
                DropBufferedEventsByIndex(ingestBatchIndices);
                ingestBatchIndices.clear();
                ingestBatchQueueName.clear();
                ingestBatchPayload.clear();
                ingestConsecutiveFailureCount = 0;
                nextIngestAttemptUnixSeconds = nowUnixSeconds;
                ingestStage = IngestStage::Idle;
                return;
            }

            for (const std::size_t index : ingestBatchIndices)
            {
                if (index < bufferedEvents.size())
                {
                    bufferedEvents[index].AttemptCount++;
                }
            }

            PruneBufferedEvents(host, nowUnixSeconds);

            ingestConsecutiveFailureCount++;
            const std::int64_t backoffSeconds = static_cast<std::int64_t>(
                std::min<std::size_t>(60, static_cast<std::size_t>(1) << std::min<std::size_t>(6, ingestConsecutiveFailureCount)));

            nextIngestAttemptUnixSeconds = nowUnixSeconds + backoffSeconds;
            host.Log("ingest batch POST returned HTTP " + std::to_string(response.StatusCode) + " for queue " + ingestBatchQueueName);

            ingestBatchIndices.clear();
            ingestBatchQueueName.clear();
            ingestBatchPayload.clear();
            ingestStage = IngestStage::Idle;
            return;
        }
    }

    if (bufferedEvents.empty() || nowUnixSeconds < nextIngestAttemptUnixSeconds)
    {
        return;
    }

    if (!IsIngestTokenValid(nowUnixSeconds))
    {
        if (!StartIngestTokenRequest(host, nowUnixSeconds))
        {
            AbortIngest(host, nowUnixSeconds, "failed to start ingest access token request");
        }

        return;
    }

    if (!StartIngestBatchRequest(host, nowUnixSeconds))
    {
        AbortIngest(host, nowUnixSeconds, "failed to start ingest batch request");
    }
}

bool PluginRuntime::StartIngestTokenRequest(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    const std::string tokenUrl = "https://login.microsoftonline.com/" + loadedConfig->TenantId + "/oauth2/v2.0/token";
    const std::string tokenBody = "grant_type=client_credentials&client_id=" + UrlEncode(loadedConfig->ClientId) +
        "&client_secret=" + UrlEncode(loadedConfig->ClientSecret) +
        "&scope=" + UrlEncode(loadedConfig->IngestApiResource + "/.default");

    ingestRequest = host.BeginHttpRequest(
        tokenUrl,
        "POST",
        tokenBody,
        "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n");

    if (ingestRequest == nullptr)
    {
        return false;
    }

    ingestRequestStartedUnixSeconds = nowUnixSeconds;
    ingestStage = IngestStage::AcquiringToken;
    return true;
}

bool PluginRuntime::StartIngestBatchRequest(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    if (bufferedEvents.empty())
    {
        ingestStage = IngestStage::Idle;
        return true;
    }

    ingestBatchQueueName = bufferedEvents.front().QueueName;
    ingestBatchIndices = BuildBatchIndicesForQueue(ingestBatchQueueName, kIngestMaxBatchEvents, kIngestMaxBatchBytes);
    if (ingestBatchIndices.empty())
    {
        return false;
    }

    ingestBatchPayload.clear();
    ingestBatchPayload.push_back('[');
    for (std::size_t i = 0; i < ingestBatchIndices.size(); ++i)
    {
        if (i > 0)
        {
            ingestBatchPayload.push_back(',');
        }

        ingestBatchPayload += StampPublishedUtc(bufferedEvents[ingestBatchIndices[i]].PayloadJson, nowUnixSeconds);
    }

    ingestBatchPayload.push_back(']');

    std::string headers = BuildAuthorizationHeaders(ingestAccessToken);
    headers += "Content-Type: application/json\r\n";

    const std::string requestUrl = loadedConfig->IngestBaseUrl + QueueEndpointPath(ingestBatchQueueName);
    ingestRequest = host.BeginHttpRequest(requestUrl, "POST", ingestBatchPayload, headers);
    if (ingestRequest == nullptr)
    {
        return false;
    }

    ingestRequestStartedUnixSeconds = nowUnixSeconds;
    ingestStage = IngestStage::PostingBatch;
    return true;
}

void PluginRuntime::AbortIngest(ICod4xHost& host, std::int64_t nowUnixSeconds, std::string_view reason)
{
    if (!reason.empty())
    {
        host.Log(std::string(reason));
    }

    if (ingestRequest != nullptr)
    {
        host.EndHttpRequest(ingestRequest);
        ingestRequest = nullptr;
    }

    if (ingestStage == IngestStage::PostingBatch)
    {
        for (const std::size_t index : ingestBatchIndices)
        {
            if (index < bufferedEvents.size())
            {
                bufferedEvents[index].AttemptCount++;
            }
        }
    }

    ingestStage = IngestStage::Idle;
    ingestConsecutiveFailureCount++;
    const std::int64_t backoffSeconds = static_cast<std::int64_t>(
        std::min<std::size_t>(60, static_cast<std::size_t>(1) << std::min<std::size_t>(6, ingestConsecutiveFailureCount)));
    nextIngestAttemptUnixSeconds = nowUnixSeconds + backoffSeconds;
    ingestBatchIndices.clear();
    ingestBatchQueueName.clear();
    ingestBatchPayload.clear();

    PruneBufferedEvents(host, nowUnixSeconds);
}

void PluginRuntime::FlushServerStatusSnapshot(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    if (!IsIngestConfigured())
    {
        return;
    }

    for (auto it = connectedPlayers.begin(); it != connectedPlayers.end();)
    {
        if (it->second.PlayerGuid.empty())
        {
            it = connectedPlayers.erase(it);
            continue;
        }

        if (it->second.Username.empty())
        {
            it->second.Username = Trim(host.GetPlayerName(it->first));
        }

        it->second.Score = host.GetPlayerScore(it->first);
        ++it;
    }

    const std::string messageId = GenerateMessageId();
    BufferEvent(
        std::string(kQueueServerStatus),
        BuildServerStatusPayload(nowUnixSeconds, messageId, host),
        messageId,
        nowUnixSeconds);
}

std::string PluginRuntime::BuildPlayerConnectedPayload(
    std::int64_t nowUnixSeconds,
    const std::string& messageId,
    const std::string& playerGuid,
    std::uint64_t steamId,
    const std::string& username,
    const std::string& ipAddress,
    int slotId)
{
    std::string payload = BuildBaseEventPrefix(nowUnixSeconds, messageId, NextSequenceId());
    payload += ",\"playerGuid\":\"" + JsonEscape(playerGuid) + "\"";
    payload += ",\"steamId\":\"" + std::to_string(steamId) + "\"";
    payload += ",\"username\":\"" + JsonEscape(username) + "\"";
    payload += ",\"ipAddress\":\"" + JsonEscape(ipAddress) + "\"";
    payload += ",\"slotId\":" + std::to_string(slotId);
    payload += "}";
    return payload;
}

std::string PluginRuntime::BuildPlayerDisconnectedPayload(
    std::int64_t nowUnixSeconds,
    const std::string& messageId,
    const std::string& playerGuid,
    const std::string& username,
    int slotId)
{
    std::string payload = BuildBaseEventPrefix(nowUnixSeconds, messageId, NextSequenceId());
    payload += ",\"playerGuid\":\"" + JsonEscape(playerGuid) + "\"";
    payload += ",\"username\":\"" + JsonEscape(username) + "\"";
    payload += ",\"slotId\":" + std::to_string(slotId);
    payload += "}";
    return payload;
}

std::string PluginRuntime::BuildChatMessagePayload(
    std::int64_t nowUnixSeconds,
    const std::string& messageId,
    const std::string& playerGuid,
    const std::string& username,
    int slotId,
    std::string_view message,
    bool teamMessage)
{
    std::string payload = BuildBaseEventPrefix(nowUnixSeconds, messageId, NextSequenceId());
    payload += ",\"playerGuid\":\"" + JsonEscape(playerGuid) + "\"";
    payload += ",\"username\":\"" + JsonEscape(username) + "\"";
    payload += ",\"slotId\":" + std::to_string(slotId);
    payload += ",\"message\":\"" + JsonEscape(message) + "\"";
    payload += ",\"type\":\"";
    payload += teamMessage ? "Team" : "All";
    payload += "\"}";
    return payload;
}

std::string PluginRuntime::BuildServerConnectedPayload(std::int64_t nowUnixSeconds, const std::string& messageId)
{
    return BuildBaseEventPrefix(nowUnixSeconds, messageId, NextSequenceId()) + "}";
}

std::string PluginRuntime::BuildMapChangePayload(
    std::int64_t nowUnixSeconds,
    const std::string& messageId,
    const std::string& mapName,
    const std::string& gameName)
{
    std::string payload = BuildBaseEventPrefix(nowUnixSeconds, messageId, NextSequenceId());
    payload += ",\"mapName\":\"" + JsonEscape(mapName) + "\"";
    payload += ",\"gameName\":\"" + JsonEscape(gameName) + "\"";
    payload += "}";
    return payload;
}

std::string PluginRuntime::BuildServerStatusPayload(std::int64_t nowUnixSeconds, const std::string& messageId, ICod4xHost& host)
{
    std::vector<int> sortedSlots;
    sortedSlots.reserve(connectedPlayers.size());
    for (const auto& [slot, state] : connectedPlayers)
    {
        if (!state.PlayerGuid.empty() && !state.Username.empty())
        {
            sortedSlots.push_back(slot);
        }
    }

    std::sort(sortedSlots.begin(), sortedSlots.end());

    std::string payload = BuildBaseEventPrefix(nowUnixSeconds, messageId, NextSequenceId());
    payload += ",\"mapName\":\"" + JsonEscape(GetMapName(host)) + "\"";
    payload += ",\"gameName\":\"" + JsonEscape(GetGameName(host)) + "\"";
    payload += ",\"playerCount\":" + std::to_string(sortedSlots.size());
    payload += ",\"players\":[";

    for (std::size_t i = 0; i < sortedSlots.size(); ++i)
    {
        const ConnectedPlayerState& state = connectedPlayers.at(sortedSlots[i]);
        if (i > 0)
        {
            payload.push_back(',');
        }

        payload += "{\"playerGuid\":\"" + JsonEscape(state.PlayerGuid) + "\"";
        payload += ",\"username\":\"" + JsonEscape(state.Username) + "\"";
        payload += ",\"ipAddress\":\"" + JsonEscape(state.IpAddress.empty() ? "0.0.0.0" : state.IpAddress) + "\"";
        payload += ",\"slotId\":" + std::to_string(state.SlotId);
        payload += ",\"connectedAtUtc\":\"" + ToIso8601Utc(state.ConnectedAtUnixSeconds) + "\"";
        payload += ",\"score\":" + std::to_string(state.Score);
        payload += ",\"ping\":0,\"rate\":0}";
    }

    payload += "]";

    const std::string serverTitle = GetServerTitle(host);
    if (!serverTitle.empty())
    {
        payload += ",\"serverTitle\":\"" + JsonEscape(serverTitle) + "\"";
    }

    const std::string serverMod = GetServerMod(host);
    if (!serverMod.empty())
    {
        payload += ",\"serverMod\":\"" + JsonEscape(serverMod) + "\"";
    }

    payload += ",\"maxPlayers\":" + std::to_string(std::max(0, host.GetSlotCount()));
    payload += ",\"pluginVersion\":\"" + JsonEscape(pluginVersion) + "\"";
    payload += ",\"pluginHealth\":\"ok\"";
    payload += ",\"egressBufferedEventCount\":" + std::to_string(bufferedEvents.size());
    payload += "}";

    return payload;
}

std::string PluginRuntime::GetMapName(ICod4xHost& host) const
{
    const std::string mapName = Trim(host.GetCvarString("mapname"));
    return mapName.empty() ? "unknown" : mapName;
}

std::string PluginRuntime::GetGameName(ICod4xHost& host) const
{
    const std::string gameName = Trim(host.GetCvarString("gamename"));
    if (!gameName.empty())
    {
        return gameName;
    }

    const std::string gameTypeName = Trim(host.GetCvarString("g_gametype"));
    return gameTypeName.empty() ? loadedConfig->GameType : gameTypeName;
}

std::string PluginRuntime::GetServerTitle(ICod4xHost& host) const
{
    return Trim(host.GetCvarString("sv_hostname"));
}

std::string PluginRuntime::GetServerMod(ICod4xHost& host) const
{
    return Trim(host.GetCvarString("fs_game"));
}

void PluginRuntime::BufferEvent(std::string queueName, std::string payloadJson, std::string messageId, std::int64_t nowUnixSeconds)
{
    if (queueName.empty() || payloadJson.empty())
    {
        return;
    }

    if (bufferedEvents.size() >= kMaxBufferedEvents)
    {
        if (std::string_view(queueName) == kQueueChatMessage)
        {
            return;
        }

        auto chatIt = std::find_if(bufferedEvents.begin(), bufferedEvents.end(), [](const BufferedEvent& item) {
            return std::string_view(item.QueueName) == kQueueChatMessage;
        });

        if (chatIt != bufferedEvents.end())
        {
            bufferedEvents.erase(chatIt);
        }
        else if (!bufferedEvents.empty())
        {
            bufferedEvents.pop_front();
        }
    }

    bufferedEvents.push_back(BufferedEvent{
        std::move(queueName),
        std::move(payloadJson),
        std::move(messageId),
        nowUnixSeconds,
        0});

    if (nextIngestAttemptUnixSeconds > nowUnixSeconds)
    {
        nextIngestAttemptUnixSeconds = nowUnixSeconds;
    }
}

void PluginRuntime::PruneBufferedEvents(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    if (bufferedEvents.empty())
    {
        return;
    }

    std::size_t droppedByAttempts = 0;
    std::size_t droppedByAge = 0;

    for (auto it = bufferedEvents.begin(); it != bufferedEvents.end();)
    {
        const bool exceededAttempts = it->AttemptCount >= kMaxBufferedEventAttempts;
        const bool exceededAge = (nowUnixSeconds - it->CreatedUnixSeconds) >= kMaxBufferedEventAgeSeconds;

        if (!exceededAttempts && !exceededAge)
        {
            ++it;
            continue;
        }

        if (exceededAttempts)
        {
            droppedByAttempts++;
        }

        if (exceededAge)
        {
            droppedByAge++;
        }

        it = bufferedEvents.erase(it);
    }

    const std::size_t droppedTotal = droppedByAttempts + droppedByAge;
    if (droppedTotal > 0)
    {
        host.Log(
            "ingest buffer dropped " + std::to_string(droppedTotal) +
            " event(s) (attempt-threshold=" + std::to_string(droppedByAttempts) +
            ", age-threshold=" + std::to_string(droppedByAge) + ")");
    }
}

std::vector<std::size_t> PluginRuntime::BuildBatchIndicesForQueue(
    const std::string& queueName,
    std::size_t maxEvents,
    std::size_t maxBytes) const
{
    std::vector<std::size_t> indices;
    std::size_t currentBytes = 2;

    for (std::size_t i = 0; i < bufferedEvents.size(); ++i)
    {
        if (bufferedEvents[i].QueueName != queueName)
        {
            continue;
        }

        const std::size_t eventBytes = bufferedEvents[i].PayloadJson.size() + (indices.empty() ? 0 : 1);
        if (!indices.empty() && currentBytes + eventBytes > maxBytes)
        {
            break;
        }

        currentBytes += eventBytes;
        indices.push_back(i);

        if (indices.size() >= maxEvents)
        {
            break;
        }
    }

    return indices;
}

void PluginRuntime::DropBufferedEventsByIndex(const std::vector<std::size_t>& indices)
{
    if (indices.empty())
    {
        return;
    }

    std::vector<std::size_t> sortedIndices = indices;
    std::sort(sortedIndices.begin(), sortedIndices.end(), std::greater<>());

    for (const std::size_t index : sortedIndices)
    {
        if (index < bufferedEvents.size())
        {
            bufferedEvents.erase(bufferedEvents.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
}

std::string PluginRuntime::BuildBaseEventPrefix(std::int64_t nowUnixSeconds, const std::string& messageId, long long sequenceId) const
{
    std::string payload;
    payload.reserve(256);
    payload += "{\"messageId\":\"" + JsonEscape(messageId) + "\"";
    payload += ",\"eventGeneratedUtc\":\"" + ToIso8601Utc(nowUnixSeconds) + "\"";
    payload += ",\"eventPublishedUtc\":\"" + ToIso8601Utc(nowUnixSeconds) + "\"";
    payload += ",\"serverId\":\"" + JsonEscape(loadedConfig->GameServerId) + "\"";
    payload += ",\"gameType\":\"" + JsonEscape(loadedConfig->GameType) + "\"";
    payload += ",\"sequenceId\":" + std::to_string(sequenceId);
    return payload;
}

long long PluginRuntime::NextSequenceId()
{
    return nextSequenceId++;
}

std::string PluginRuntime::GenerateMessageId()
{
    static std::mt19937_64 generator(std::random_device{}());
    static constexpr char kHexChars[] = "0123456789abcdef";

    std::uniform_int_distribution<unsigned int> distribution(0, 15);

    std::string output;
    output.reserve(36);

    const std::array<int, 5> groups{8, 4, 4, 4, 12};
    for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex)
    {
        if (groupIndex > 0)
        {
            output.push_back('-');
        }

        for (int i = 0; i < groups[groupIndex]; ++i)
        {
            output.push_back(kHexChars[distribution(generator)]);
        }
    }

    return output;
}

std::string PluginRuntime::JsonEscape(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());

    for (const char c : value)
    {
        switch (c)
        {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(c);
                break;
        }
    }

    return escaped;
}

std::string PluginRuntime::ToIso8601Utc(std::int64_t unixSeconds)
{
    std::time_t unixTime = static_cast<std::time_t>(unixSeconds);
    std::tm utcTime{};

#if defined(_WIN32)
    gmtime_s(&utcTime, &unixTime);
#else
    gmtime_r(&unixTime, &utcTime);
#endif

    std::ostringstream output;
    output << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string PluginRuntime::StampPublishedUtc(std::string payloadJson, std::int64_t nowUnixSeconds)
{
    constexpr std::string_view kToken = "\"eventPublishedUtc\":\"";
    const std::size_t tokenPos = payloadJson.find(kToken);
    if (tokenPos == std::string::npos)
    {
        return payloadJson;
    }

    const std::size_t valueStart = tokenPos + kToken.size();
    const std::size_t valueEnd = payloadJson.find('"', valueStart);
    if (valueEnd == std::string::npos)
    {
        return payloadJson;
    }

    payloadJson.replace(valueStart, valueEnd - valueStart, ToIso8601Utc(nowUnixSeconds));
    return payloadJson;
}

std::string PluginRuntime::QueueEndpointPath(std::string_view queueName)
{
    return "/events/" + std::string(queueName);
}

std::string PluginRuntime::NormalizeIpAddress(std::string ipAddress)
{
    ipAddress = Trim(std::move(ipAddress));
    if (ipAddress.empty())
    {
        return ipAddress;
    }

    const std::size_t bracketPos = ipAddress.rfind("]:");
    if (!ipAddress.empty() && ipAddress.front() == '[' && bracketPos != std::string::npos)
    {
        return ipAddress.substr(1, bracketPos - 1);
    }

    const std::size_t colonPos = ipAddress.rfind(':');
    if (colonPos != std::string::npos && ipAddress.find('.') != std::string::npos)
    {
        return ipAddress.substr(0, colonPos);
    }

    return ipAddress;
}

std::string PluginRuntime::Trim(std::string value)
{
    const auto isWhitespace = [](unsigned char c) { return std::isspace(c) != 0; };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c) { return !isWhitespace(c); }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), [&](unsigned char c) { return !isWhitespace(c); }).base(),
        value.end());

    return value;
}

namespace
{
PluginRuntime g_runtime;
}

int InitializePlugin(ICod4xHost& host, std::string_view version, std::string_view prefix)
{
    return g_runtime.Initialize(host, version, prefix);
}

void TickPlugin(ICod4xHost& host)
{
    g_runtime.Tick(host);
}

void NotifyPlayerConnect(ICod4xHost& host, int slot, std::string_view ipAddress)
{
    g_runtime.HandlePlayerConnect(host, slot, ipAddress);
}

void NotifyPlayerConnected(ICod4xHost& host, int slot)
{
    g_runtime.HandlePlayerConnected(host, slot);
}

void NotifyClientAuthorized(ICod4xHost& host)
{
    g_runtime.HandleClientAuthorized(host);
}

void NotifyPlayerDisconnected(ICod4xHost& host, int slot)
{
    g_runtime.HandlePlayerDisconnected(host, slot);
}

void NotifyChatMessage(ICod4xHost& host, int slot, std::string_view message, bool teamMessage)
{
    g_runtime.HandleChatMessage(host, slot, message, teamMessage);
}

void NotifyClientCommand(ICod4xHost& host, int slot, std::string_view command)
{
    g_runtime.HandleClientCommand(host, slot, command);
}

void NotifyServerSpawned(ICod4xHost& host)
{
    g_runtime.HandleServerSpawned(host);
}

void NotifyServerExited(ICod4xHost& host)
{
    g_runtime.HandleServerExited(host);
}

const EffectiveServerContext& GetServerContext()
{
    return g_runtime.GetServerContext();
}
}
