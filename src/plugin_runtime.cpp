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
#include <vector>

namespace portal_cod4x
{
namespace
{
constexpr std::int64_t kIngestRequestDeadlineSeconds = 20;
constexpr std::int64_t kBanSyncRequestDeadlineSeconds = 20;
constexpr std::int64_t kServerStatusIntervalSeconds = 60;
constexpr std::int64_t kMaxBufferedEventAgeSeconds = 15 * 60;
constexpr std::size_t kMaxBufferedEventAttempts = 8;
constexpr std::int64_t kDefaultBanSyncIntervalSeconds = 60;
constexpr std::string_view kDefaultBanMessage = "You are banned from this server.";

constexpr std::size_t kMaxBufferedEvents = 1000;
constexpr std::size_t kIngestMaxBatchEvents = 100;
constexpr std::size_t kIngestMaxBatchBytes = 200 * 1024;
constexpr int kActiveBanPageSize = 200;

constexpr std::string_view kQueuePlayerConnected = "player-connected";
constexpr std::string_view kQueuePlayerDisconnected = "player-disconnected";
constexpr std::string_view kQueueChatMessage = "chat-message";
constexpr std::string_view kQueueServerConnected = "server-connected";
constexpr std::string_view kQueueMapChange = "map-change";
constexpr std::string_view kQueueServerStatus = "server-status";
constexpr std::int64_t kCrossCallbackDedupWindowSeconds = 1;
constexpr std::string_view kPortalPluginHealthCommandPrefix = "!portalpluginhealth";

std::string Trim(std::string value)
{
    const auto isWhitespace = [](unsigned char c) { return std::isspace(c) != 0; };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c) { return !isWhitespace(c); }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), [&](unsigned char c) { return !isWhitespace(c); }).base(),
        value.end());

    return value;
}

std::string ToLowerInvariant(std::string_view value)
{
    std::string normalized;
    normalized.reserve(value.size());

    for (const unsigned char c : value)
    {
        normalized.push_back(static_cast<char>(std::tolower(c)));
    }

    return normalized;
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

bool IsJsonValueTerminator(const std::string& json, std::size_t index)
{
    while (index < json.size() && std::isspace(static_cast<unsigned char>(json[index])) != 0)
    {
        ++index;
    }

    return index >= json.size() || json[index] == ',' || json[index] == '}' || json[index] == ']';
}

bool StartsWithIgnoreCase(std::string_view value, std::size_t start, std::string_view token)
{
    if (start + token.size() > value.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < token.size(); ++index)
    {
        const auto left = static_cast<unsigned char>(value[start + index]);
        const auto right = static_cast<unsigned char>(token[index]);

        if (std::tolower(left) != std::tolower(right))
        {
            return false;
        }
    }

    return true;
}

std::optional<bool> ExtractJsonBoolValue(const std::string& json, const std::string& key)
{
    const std::string keyToken = "\"" + key + "\"";
    const auto keyPosition = json.find(keyToken);
    if (keyPosition == std::string::npos)
    {
        return std::nullopt;
    }

    const auto colonPosition = json.find(':', keyPosition + keyToken.size());
    if (colonPosition == std::string::npos)
    {
        return std::nullopt;
    }

    auto valueStart = colonPosition + 1;
    while (valueStart < json.size() && std::isspace(static_cast<unsigned char>(json[valueStart])) != 0)
    {
        ++valueStart;
    }

    if (valueStart >= json.size())
    {
        return std::nullopt;
    }

    if (json[valueStart] == '"')
    {
        auto valueEnd = valueStart + 1;
        bool escaped = false;

        while (valueEnd < json.size())
        {
            const auto current = json[valueEnd];
            if (escaped)
            {
                escaped = false;
                ++valueEnd;
                continue;
            }

            if (current == '\\')
            {
                escaped = true;
                ++valueEnd;
                continue;
            }

            if (current == '"')
            {
                break;
            }

            ++valueEnd;
        }

        if (valueEnd >= json.size() || json[valueEnd] != '"')
        {
            return std::nullopt;
        }

        if (!IsJsonValueTerminator(json, valueEnd + 1))
        {
            return std::nullopt;
        }

        std::string token = ToLowerInvariant(Trim(json.substr(valueStart + 1, valueEnd - valueStart - 1)));
        if (token == "true" || token == "1")
        {
            return true;
        }

        if (token == "false" || token == "0")
        {
            return false;
        }

        return std::nullopt;
    }

    if (StartsWithIgnoreCase(json, valueStart, "true") && IsJsonValueTerminator(json, valueStart + 4))
    {
        return true;
    }

    if (StartsWithIgnoreCase(json, valueStart, "false") && IsJsonValueTerminator(json, valueStart + 5))
    {
        return false;
    }

    if (json[valueStart] == '1' && IsJsonValueTerminator(json, valueStart + 1))
    {
        return true;
    }

    if (json[valueStart] == '0' && IsJsonValueTerminator(json, valueStart + 1))
    {
        return false;
    }

    return std::nullopt;
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
    auto portalPluginHealthEnabled = ExtractJsonBoolValue(configJson, "portalPluginHealthEnabled");
    auto portalPluginHealthMinPower = ExtractJsonIntValue(configJson, "portalPluginHealthMinPower");

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

    if (portalPluginHealthEnabled.has_value())
    {
        config.PortalPluginHealthEnabled = *portalPluginHealthEnabled;
    }

    if (portalPluginHealthMinPower.has_value())
    {
        config.PortalPluginHealthMinPower = std::clamp(*portalPluginHealthMinPower, 1, 100);
    }

    return config;
}

std::string BuildAuthorizationHeaders(const std::string& accessToken)
{
    return "Authorization: Bearer " + accessToken + "\r\nAccept: application/json\r\n";
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

std::string BuildPlayerGuidKey(std::uint64_t playerId)
{
    if (playerId == 0)
    {
        return {};
    }

    return std::to_string(playerId);
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

bool PluginRuntime::ShouldLog(LogLevel level) const
{
    return static_cast<int>(level) >= static_cast<int>(logLevel);
}

void PluginRuntime::LogDebug(ICod4xHost& host, std::string_view message) const
{
    if (ShouldLog(LogLevel::Debug))
    {
        host.Log(message);
    }
}

void PluginRuntime::LogInfo(ICod4xHost& host, std::string_view message) const
{
    if (ShouldLog(LogLevel::Info))
    {
        host.Log(message);
    }
}

void PluginRuntime::LogError(ICod4xHost& host, std::string_view message) const
{
    if (ShouldLog(LogLevel::Error))
    {
        host.Log(message);
    }
}

bool PluginRuntime::TrySetLogLevel(ICod4xHost& host, int levelValue, bool announce)
{
    return TrySetLogLevel(host, std::to_string(levelValue), announce);
}

bool PluginRuntime::TrySetLogLevel(ICod4xHost& host, std::string_view levelToken, bool announce)
{
    const std::string normalizedToken = ToLowerInvariant(Trim(std::string(levelToken)));
    if (normalizedToken.empty())
    {
        return false;
    }

    LogLevel parsedLogLevel = logLevel;
    if (normalizedToken == "1" || normalizedToken == "debug")
    {
        parsedLogLevel = LogLevel::Debug;
    }
    else if (normalizedToken == "2" || normalizedToken == "info")
    {
        parsedLogLevel = LogLevel::Info;
    }
    else if (normalizedToken == "3" || normalizedToken == "error")
    {
        parsedLogLevel = LogLevel::Error;
    }
    else
    {
        return false;
    }

    logLevel = parsedLogLevel;

    if (announce)
    {
        host.Log("plugin log level set to " + GetLogLevelName() + " (" + std::to_string(GetLogLevelValue()) + ")");
    }

    return true;
}

int PluginRuntime::GetLogLevelValue() const
{
    return static_cast<int>(logLevel);
}

std::string PluginRuntime::GetLogLevelName() const
{
    switch (logLevel)
    {
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Error:
            return "error";
        case LogLevel::Info:
        default:
            return "info";
    }
}

int PluginRuntime::Initialize(ICod4xHost& host, std::string_view version, std::string_view prefix)
{
    const std::string onlineMessage = BuildOnlineBroadcastMessage(prefix, version);
    const std::string normalizedVersion = version.empty() ? "0.0.0-unknown" : std::string(version);
    chatPrefix = prefix.empty() ? std::string(kDefaultBotPrefix) : std::string(prefix);
    pluginVersion = normalizedVersion;

    LogInfo(host, "Portal Plugin is online (version " + normalizedVersion + ")");
    host.BroadcastChat(onlineMessage);

    const std::int64_t nowUnixSeconds = host.GetUnixTimeSeconds();
    TryLoadConfig(host, nowUnixSeconds);
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

    if (IsIngestConfigured() && nowUnixSeconds >= nextServerStatusUnixSeconds)
    {
        FlushServerStatusSnapshot(host, nowUnixSeconds);
        nextServerStatusUnixSeconds = nowUnixSeconds + kServerStatusIntervalSeconds;
    }

    AdvanceIngest(host, nowUnixSeconds);
    AdvanceBanSync(host, nowUnixSeconds);
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

    if (!EqualsIgnoreCase(commandToken, kPortalPluginHealthCommandPrefix))
    {
        return;
    }

    markHandled();

    if (loadedConfig.has_value() && !loadedConfig->PortalPluginHealthEnabled)
    {
        SendPrivateChat(host, slot, "The !portalpluginhealth command is currently disabled by portal configuration.");
        return;
    }

    if (!host.CanPlayerUseCommand(slot, kPortalPluginHealthCommandName))
    {
        SendPrivateChat(host, slot, "You are not authorized to run !portalpluginhealth.");
        return;
    }

    HandlePortalPluginHealthCommand(host, slot);
}

void PluginRuntime::HandlePortalPluginHealthCommand(ICod4xHost& host, int invokerSlot)
{
    const std::int64_t nowUnixSeconds = host.GetUnixTimeSeconds();
    const std::vector<std::string> reportLines = BuildPortalPluginHealthReportLines(nowUnixSeconds);

    for (const auto& line : reportLines)
    {
        host.Log(line);
    }

    if (invokerSlot < 0)
    {
        return;
    }

    for (const auto& line : reportLines)
    {
        std::string commandText = line;
        std::replace(commandText.begin(), commandText.end(), '\n', ' ');
        std::replace(commandText.begin(), commandText.end(), '\r', ' ');
        host.ExecuteServerCommand("consay " + commandText);
    }

    host.ExecuteServerCommand(
        "tell " + std::to_string(invokerSlot) +
        " Portal plugin health written to console output. Open your console for full details.");
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

void PluginRuntime::HandlePlayerBanAdded(std::uint64_t playerId, std::string_view reason)
{
    const std::string playerGuid = BuildPlayerGuidKey(playerId);
    if (playerGuid.empty())
    {
        return;
    }

    std::string renderedReason = Trim(std::string(reason));
    if (renderedReason.empty())
    {
        renderedReason = std::string(kDefaultBanMessage);
    }

    {
        std::lock_guard<std::mutex> guard(activeBanCacheMutex);
        activeBanMessagesByPlayerGuid[playerGuid] = std::move(renderedReason);
    }

    nextBanSyncUnixSeconds.store(0, std::memory_order_relaxed);
}

void PluginRuntime::HandlePlayerBanRemoved(std::uint64_t playerId)
{
    const std::string playerGuid = BuildPlayerGuidKey(playerId);
    if (playerGuid.empty())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(activeBanCacheMutex);
        activeBanMessagesByPlayerGuid.erase(playerGuid);
    }

    nextBanSyncUnixSeconds.store(0, std::memory_order_relaxed);
}

bool PluginRuntime::TryGetPlayerBanMessage(std::uint64_t playerId, std::string& message) const
{
    const std::string playerGuid = BuildPlayerGuidKey(playerId);
    if (playerGuid.empty())
    {
        return false;
    }

    std::lock_guard<std::mutex> guard(activeBanCacheMutex);
    const auto it = activeBanMessagesByPlayerGuid.find(playerGuid);
    if (it == activeBanMessagesByPlayerGuid.end())
    {
        return false;
    }

    message = it->second.empty() ? std::string(kDefaultBanMessage) : it->second;
    return true;
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
            LogError(host, issue);
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
    serverContext.LastRefreshUnixSeconds = nowUnixSeconds;
    lastConfigLoadError.clear();
    nextConfigLoadAttemptUnixSeconds = nowUnixSeconds;
    nextBanSyncUnixSeconds.store(nowUnixSeconds, std::memory_order_relaxed);
    repositoryConfigWarningLogged = false;
    LogInfo(host, "plugin config loaded for gameServerId " + loadedConfig->GameServerId);
    return true;
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
            LogInfo(host, "ingest egress disabled; configure ingestBaseUrl, ingestApiResource, and gameType to enable event emission");
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
                LogDebug(
                    host,
                    "ingest batch POST succeeded for queue " + ingestBatchQueueName +
                        " eventCount=" + std::to_string(ingestBatchIndices.size()));
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
            LogError(host, "ingest batch POST returned HTTP " + std::to_string(response.StatusCode) + " for queue " + ingestBatchQueueName);

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

    LogDebug(host, "starting ingest access-token request");
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
    LogDebug(
        host,
        "starting ingest batch POST to " + requestUrl +
            " queue=" + ingestBatchQueueName +
            " eventCount=" + std::to_string(ingestBatchIndices.size()));
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
        LogError(host, std::string(reason));
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

bool PluginRuntime::IsRepositoryConfigured() const
{
    return loadedConfig.has_value() && !loadedConfig->RepositoryApiBaseUrl.empty() && !loadedConfig->RepositoryApiResource.empty() &&
        !loadedConfig->TenantId.empty() && !loadedConfig->ClientId.empty() && !loadedConfig->ClientSecret.empty();
}

bool PluginRuntime::IsRepositoryTokenValid(std::int64_t nowUnixSeconds) const
{
    return !repositoryAccessToken.empty() && nowUnixSeconds + 30 < repositoryAccessTokenExpiresAtUnixSeconds;
}

void PluginRuntime::AdvanceBanSync(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    if (!loadedConfig.has_value())
    {
        return;
    }

    if (!IsRepositoryConfigured())
    {
        if (!repositoryConfigWarningLogged)
        {
            LogInfo(host, "repository active-ban sync disabled; configure repository API auth settings to enable plugin ban cache enforcement");
            repositoryConfigWarningLogged = true;
        }

        return;
    }

    repositoryConfigWarningLogged = false;

    if (banSyncStage != BanSyncStage::Idle)
    {
        if (banSyncRequest == nullptr)
        {
            banSyncStage = BanSyncStage::Idle;
            return;
        }

        HttpResponse response;
        const HttpRequestStatus status = host.PollHttpRequest(banSyncRequest, response);
        if (status == HttpRequestStatus::Pending)
        {
            if (nowUnixSeconds - banSyncRequestStartedUnixSeconds >= kBanSyncRequestDeadlineSeconds)
            {
                AbortBanSync(host, nowUnixSeconds, "active-ban sync request timed out");
            }

            return;
        }

        host.EndHttpRequest(banSyncRequest);
        banSyncRequest = nullptr;

        if (status == HttpRequestStatus::Failed)
        {
            AbortBanSync(host, nowUnixSeconds, "active-ban sync request failed");
            return;
        }

        if (banSyncStage == BanSyncStage::AcquiringToken)
        {
            if (response.StatusCode != 200)
            {
                AbortBanSync(host, nowUnixSeconds, "failed to acquire repository API access token");
                return;
            }

            const auto parsedAccessToken = ExtractJsonStringValue(response.Body, "access_token");
            if (!parsedAccessToken.has_value() || parsedAccessToken->empty())
            {
                AbortBanSync(host, nowUnixSeconds, "repository token response missing access_token");
                return;
            }

            const int expiresInSeconds = ExtractJsonIntValue(response.Body, "expires_in").value_or(3600);
            repositoryAccessToken = *parsedAccessToken;
            repositoryAccessTokenExpiresAtUnixSeconds = nowUnixSeconds + std::max(0, expiresInSeconds - 30);
            activeBanFetchSkipEntries = 0;
            pendingActiveBanMessagesByPlayerGuid.clear();

            if (!StartActiveBanFetchRequest(host, nowUnixSeconds, activeBanFetchSkipEntries))
            {
                AbortBanSync(host, nowUnixSeconds, "failed to start repository active-ban request");
            }

            return;
        }

        if (banSyncStage == BanSyncStage::FetchingActiveBans)
        {
            if (response.StatusCode < 200 || response.StatusCode >= 300)
            {
                AbortBanSync(
                    host,
                    nowUnixSeconds,
                    "repository active-ban request returned HTTP " + std::to_string(response.StatusCode));
                return;
            }

            auto pageBanMessages = ParseActiveBanMessagesByPlayerGuid(response.Body);
            const std::size_t pageItemCount = CountActiveBanItems(response.Body);
            for (auto& [playerGuid, banMessage] : pageBanMessages)
            {
                pendingActiveBanMessagesByPlayerGuid[playerGuid] = std::move(banMessage);
            }

            if (pageItemCount >= static_cast<std::size_t>(kActiveBanPageSize))
            {
                activeBanFetchSkipEntries += kActiveBanPageSize;
                if (!StartActiveBanFetchRequest(host, nowUnixSeconds, activeBanFetchSkipEntries))
                {
                    AbortBanSync(host, nowUnixSeconds, "failed to continue repository active-ban request");
                }

                return;
            }

            {
                std::lock_guard<std::mutex> guard(activeBanCacheMutex);
                activeBanMessagesByPlayerGuid = std::move(pendingActiveBanMessagesByPlayerGuid);
            }

            pendingActiveBanMessagesByPlayerGuid.clear();
            activeBanFetchSkipEntries = 0;
            banSyncConsecutiveFailureCount = 0;

            const int configuredInterval = std::clamp(loadedConfig->RefreshIntervalSeconds, 15, 900);
            const int intervalSeconds = configuredInterval > 0 ? configuredInterval : static_cast<int>(kDefaultBanSyncIntervalSeconds);

            nextBanSyncUnixSeconds.store(nowUnixSeconds + intervalSeconds, std::memory_order_relaxed);
            LogDebug(
                host,
                "repository active-ban sync completed; cached bans=" +
                    std::to_string(activeBanMessagesByPlayerGuid.size()) +
                    " nextSyncInSeconds=" + std::to_string(intervalSeconds));
            banSyncStage = BanSyncStage::Idle;
            return;
        }
    }

    if (nowUnixSeconds < nextBanSyncUnixSeconds.load(std::memory_order_relaxed))
    {
        return;
    }

    if (!IsRepositoryTokenValid(nowUnixSeconds))
    {
        if (!StartRepositoryTokenRequest(host, nowUnixSeconds))
        {
            AbortBanSync(host, nowUnixSeconds, "failed to start repository token request");
        }

        return;
    }

    activeBanFetchSkipEntries = 0;
    pendingActiveBanMessagesByPlayerGuid.clear();

    if (!StartActiveBanFetchRequest(host, nowUnixSeconds, activeBanFetchSkipEntries))
    {
        AbortBanSync(host, nowUnixSeconds, "failed to start repository active-ban request");
    }
}

bool PluginRuntime::StartRepositoryTokenRequest(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    const std::string tokenUrl = "https://login.microsoftonline.com/" + loadedConfig->TenantId + "/oauth2/v2.0/token";
    const std::string tokenBody = "grant_type=client_credentials&client_id=" + UrlEncode(loadedConfig->ClientId) +
        "&client_secret=" + UrlEncode(loadedConfig->ClientSecret) +
        "&scope=" + UrlEncode(loadedConfig->RepositoryApiResource + "/.default");

    banSyncRequest = host.BeginHttpRequest(
        tokenUrl,
        "POST",
        tokenBody,
        "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n");

    if (banSyncRequest == nullptr)
    {
        return false;
    }

    LogDebug(host, "starting repository access-token request for active-ban sync");
    banSyncRequestStartedUnixSeconds = nowUnixSeconds;
    banSyncStage = BanSyncStage::AcquiringToken;
    return true;
}

bool PluginRuntime::StartActiveBanFetchRequest(ICod4xHost& host, std::int64_t nowUnixSeconds, int skipEntries)
{
    const std::string gameType = loadedConfig->GameType.empty() ? "CallOfDuty4x" : loadedConfig->GameType;
    const std::string requestUrl = loadedConfig->RepositoryApiBaseUrl +
        "/v1.0/admin-actions?gameType=" + UrlEncode(gameType) +
        "&filter=ActiveBans&skipEntries=" + std::to_string(skipEntries) +
        "&takeEntries=" + std::to_string(kActiveBanPageSize) +
        "&order=CreatedDesc";

    std::string headers = BuildAuthorizationHeaders(repositoryAccessToken);

    LogDebug(
        host,
        "starting repository active-ban request (skipEntries=" + std::to_string(skipEntries) + ") to " + requestUrl);
    banSyncRequest = host.BeginHttpRequest(requestUrl, "GET", "", headers);
    if (banSyncRequest == nullptr)
    {
        return false;
    }

    banSyncRequestStartedUnixSeconds = nowUnixSeconds;
    banSyncStage = BanSyncStage::FetchingActiveBans;
    return true;
}

void PluginRuntime::AbortBanSync(ICod4xHost& host, std::int64_t nowUnixSeconds, std::string_view reason)
{
    if (!reason.empty())
    {
        LogError(host, std::string(reason));
    }

    if (banSyncRequest != nullptr)
    {
        host.EndHttpRequest(banSyncRequest);
        banSyncRequest = nullptr;
    }

    banSyncStage = BanSyncStage::Idle;
    banSyncConsecutiveFailureCount++;
    activeBanFetchSkipEntries = 0;
    pendingActiveBanMessagesByPlayerGuid.clear();

    const std::int64_t backoffSeconds = static_cast<std::int64_t>(
        std::min<std::size_t>(60, static_cast<std::size_t>(1) << std::min<std::size_t>(6, banSyncConsecutiveFailureCount)));
    nextBanSyncUnixSeconds.store(nowUnixSeconds + backoffSeconds, std::memory_order_relaxed);
}

std::size_t PluginRuntime::CountActiveBanItems(const std::string& responseBody) const
{
    static const std::regex itemPattern("\\\"adminActionId\\\"\\s*:\\s*\\\"[^\\\"]+\\\"", std::regex::icase);

    return static_cast<std::size_t>(std::distance(
        std::sregex_iterator(responseBody.begin(), responseBody.end(), itemPattern),
        std::sregex_iterator()));
}

std::unordered_map<std::string, std::string> PluginRuntime::ParseActiveBanMessagesByPlayerGuid(const std::string& responseBody) const
{
    std::unordered_map<std::string, std::string> parsed;

    static const std::regex playerGuidPattern(
        "\\\"player\\\"\\s*:\\s*\\{[^\\}]*\\\"guid\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"[^\\}]*\\}",
        std::regex::icase);

    const auto begin = std::sregex_iterator(responseBody.begin(), responseBody.end(), playerGuidPattern);
    const auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it)
    {
        std::string playerGuid = Trim(JsonUnescape((*it)[1].str()));
        if (playerGuid.empty())
        {
            continue;
        }

        parsed[playerGuid] = std::string(kDefaultBanMessage);
    }

    return parsed;
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
        LogInfo(
            host,
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

std::vector<std::string> PluginRuntime::BuildPortalPluginHealthReportLines(std::int64_t nowUnixSeconds) const
{
    std::size_t activeBanCacheCount = 0;
    {
        std::lock_guard<std::mutex> guard(activeBanCacheMutex);
        activeBanCacheCount = activeBanMessagesByPlayerGuid.size();
    }

    std::string ingestStageName = "Idle";
    switch (ingestStage)
    {
        case IngestStage::AcquiringToken:
            ingestStageName = "AcquiringToken";
            break;
        case IngestStage::PostingBatch:
            ingestStageName = "PostingBatch";
            break;
        case IngestStage::Idle:
        default:
            break;
    }

    std::string banSyncStageName = "Idle";
    switch (banSyncStage)
    {
        case BanSyncStage::AcquiringToken:
            banSyncStageName = "AcquiringToken";
            break;
        case BanSyncStage::FetchingActiveBans:
            banSyncStageName = "FetchingActiveBans";
            break;
        case BanSyncStage::Idle:
        default:
            break;
    }

    std::vector<std::string> lines;
    lines.reserve(9);

    lines.emplace_back("[portal-cod4x-plugin] portalpluginhealth report");

    std::string identityLine = "pluginVersion=" + pluginVersion +
        " loadedConfig=" + std::string(loadedConfig.has_value() ? "true" : "false") +
        " gameServerId=" + serverContext.GameServerId +
        " logLevel=" + GetLogLevelName() +
        "(" + std::to_string(GetLogLevelValue()) + ")";
    if (loadedConfig.has_value() && !loadedConfig->GameType.empty())
    {
        identityLine += " gameType=" + loadedConfig->GameType;
    }

    lines.push_back(std::move(identityLine));

    lines.push_back(
        "repositoryTokenExpiresUtc=" + FormatOptionalUnixTimestamp(repositoryAccessTokenExpiresAtUnixSeconds) +
        " repositoryTokenValid=" + std::string(IsRepositoryTokenValid(nowUnixSeconds) ? "true" : "false"));

    lines.push_back(
        "ingestTokenExpiresUtc=" + FormatOptionalUnixTimestamp(ingestAccessTokenExpiresAtUnixSeconds) +
        " ingestTokenValid=" + std::string(IsIngestTokenValid(nowUnixSeconds) ? "true" : "false"));

    lines.push_back(
        "banSyncStage=" + banSyncStageName +
        " nextBanSyncUtc=" + FormatOptionalUnixTimestamp(nextBanSyncUnixSeconds.load(std::memory_order_relaxed)) +
        " activeBanCacheCount=" + std::to_string(activeBanCacheCount) +
        " pendingBanCacheCount=" + std::to_string(pendingActiveBanMessagesByPlayerGuid.size()) +
        " banSyncInFlight=" + std::string(banSyncRequest != nullptr ? "true" : "false"));

    lines.push_back(
        "ingestStage=" + ingestStageName +
        " bufferedEventCount=" + std::to_string(bufferedEvents.size()) +
        " ingestFailureCount=" + std::to_string(ingestConsecutiveFailureCount) +
        " nextIngestAttemptUtc=" + FormatOptionalUnixTimestamp(nextIngestAttemptUnixSeconds) +
        " ingestInFlight=" + std::string(ingestRequest != nullptr ? "true" : "false"));

    lines.push_back(
        "connectedPlayerCount=" + std::to_string(connectedPlayers.size()) +
        " serverContextLastRefreshUtc=" + FormatOptionalUnixTimestamp(serverContext.LastRefreshUnixSeconds));

    lines.push_back(
        "portalpluginhealthEnabled=" +
        std::string(!loadedConfig.has_value() || loadedConfig->PortalPluginHealthEnabled ? "true" : "false") +
        " portalpluginhealthMinPower=" +
        std::to_string(loadedConfig.has_value() ? loadedConfig->PortalPluginHealthMinPower : 98));

    if (!lastConfigLoadError.empty())
    {
        lines.push_back("lastConfigLoadError=" + lastConfigLoadError);
    }

    return lines;
}

std::string PluginRuntime::FormatOptionalUnixTimestamp(std::int64_t unixSeconds)
{
    if (unixSeconds <= 0)
    {
        return "n/a";
    }

    return ToIso8601Utc(unixSeconds);
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

void NotifyPortalPluginHealthCommand(ICod4xHost& host, int slot)
{
    g_runtime.HandlePortalPluginHealthCommand(host, slot);
}

bool TrySetPluginLogLevel(ICod4xHost& host, int level, bool announce)
{
    return g_runtime.TrySetLogLevel(host, level, announce);
}

bool TrySetPluginLogLevel(ICod4xHost& host, std::string_view level, bool announce)
{
    return g_runtime.TrySetLogLevel(host, level, announce);
}

int GetPluginLogLevelValue()
{
    return g_runtime.GetLogLevelValue();
}

std::string GetPluginLogLevelName()
{
    return g_runtime.GetLogLevelName();
}

void NotifyPlayerBanAdded(std::uint64_t playerId, std::string_view reason)
{
    g_runtime.HandlePlayerBanAdded(playerId, reason);
}

void NotifyPlayerBanRemoved(std::uint64_t playerId)
{
    g_runtime.HandlePlayerBanRemoved(playerId);
}

bool TryGetPlayerBanMessage(std::uint64_t playerId, std::string& message)
{
    return g_runtime.TryGetPlayerBanMessage(playerId, message);
}

const EffectiveServerContext& GetServerContext()
{
    return g_runtime.GetServerContext();
}
}
