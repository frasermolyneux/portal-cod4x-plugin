#include "portal_cod4x/plugin_runtime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
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
constexpr std::int64_t kIngestRequestDeadlineSeconds = 60;
constexpr std::int64_t kBanSyncRequestDeadlineSeconds = 20;
constexpr std::int64_t kVpnEvaluationRequestDeadlineSeconds = 10;
constexpr std::int64_t kServerStatusIntervalSeconds = 60;
constexpr std::int64_t kMaxBufferedEventAgeSeconds = 15 * 60;
constexpr std::size_t kMaxBufferedEventAttempts = 8;
constexpr std::int64_t kDefaultBanSyncIntervalSeconds = 60;
constexpr std::string_view kDefaultBanMessage = "You are banned from this server.";

constexpr std::size_t kMaxBufferedEvents = 1000;
constexpr std::size_t kIngestMaxBatchEvents = 100;
constexpr std::size_t kIngestMaxBatchBytes = 200 * 1024;
constexpr int kActiveBanPageSize = 200;
constexpr std::size_t kMaxPendingVpnEvaluations = 64;

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

std::string SanitiseBanField(std::string value)
{
    // Newlines/carriage returns would split a single dumpbanlist entry across lines and break the
    // agent's line-based parse; other control bytes are stripped to keep the RCON output clean.
    for (char& c : value)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc == '\r' || uc == '\n' || uc < 0x20)
        {
            c = ' ';
        }
    }

    return Trim(std::move(value));
}

std::string StripLeadingControlCharacters(std::string value)
{
    // CoD4x's OnMessageSent hook delivers chat with a leading 0x15 control byte (the engine's say
    // marker; reference plugins such as adminplugin/sourcebansplugin strip it the same way). Remove
    // leading C0/DEL control bytes so command detection and the emitted chat-message payload are clean.
    const auto isControl = [](unsigned char c) { return c < 0x20 || c == 0x7F; };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c) { return !isControl(c); }));

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

// Appends a Unicode code point to the buffer encoded as UTF-8.
void AppendUtf8CodePoint(std::string& out, std::uint32_t codePoint)
{
    if (codePoint <= 0x7F)
    {
        out.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint <= 0x7FF)
    {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint <= 0xFFFF)
    {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// Parses exactly four hex digits at escaped[pos..pos+4). Returns false (leaving value untouched)
// when four hex digits are not available so the caller can fall back to literal output.
bool TryParseUnicodeEscape(std::string_view escaped, std::size_t pos, std::uint16_t& value)
{
    if (pos + 4 > escaped.size())
    {
        return false;
    }

    std::uint16_t parsed = 0;
    for (std::size_t i = 0; i < 4; ++i)
    {
        const char c = escaped[pos + i];
        std::uint16_t digit;
        if (c >= '0' && c <= '9')
        {
            digit = static_cast<std::uint16_t>(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            digit = static_cast<std::uint16_t>(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            digit = static_cast<std::uint16_t>(c - 'A' + 10);
        }
        else
        {
            return false;
        }

        parsed = static_cast<std::uint16_t>((parsed << 4) | digit);
    }

    value = parsed;
    return true;
}

// Parses a non-negative decimal player id string into a uint64. Returns false on empty/non-digit
// input or on overflow; player GUIDs in this plugin are always numeric and within uint64 range.
bool TryParseUint64(const std::string& value, std::uint64_t& parsed)
{
    if (value.empty())
    {
        return false;
    }

    std::uint64_t result = 0;
    for (const char c : value)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }

        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        // Reject overflow so a malformed/over-long id can never wrap into a plausible-looking
        // value that produces a false roster match.
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
        {
            return false;
        }

        result = (result * 10) + digit;
    }

    parsed = result;
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
            case 'u':
            {
                // Decode a \uXXXX escape (e.g. \u0027 -> '). Without this the previous default
                // branch dropped the backslash and left a literal "u0027" in ban reasons and other
                // portal-sourced strings.
                std::uint16_t code = 0;
                if (!TryParseUnicodeEscape(escaped, i + 1, code))
                {
                    // Malformed escape; preserve prior behaviour and emit the 'u' literally.
                    unescaped.push_back(next);
                    break;
                }

                i += 4; // consume the four hex digits

                std::uint32_t codePoint = code;
                // Combine a UTF-16 surrogate pair: \uD800-\uDBFF followed by \uDC00-\uDFFF.
                if (code >= 0xD800 && code <= 0xDBFF && i + 2 < escaped.size() &&
                    escaped[i + 1] == '\\' && escaped[i + 2] == 'u')
                {
                    std::uint16_t low = 0;
                    if (TryParseUnicodeEscape(escaped, i + 3, low) && low >= 0xDC00 && low <= 0xDFFF)
                    {
                        codePoint = 0x10000u +
                            ((static_cast<std::uint32_t>(code - 0xD800) << 10) |
                             static_cast<std::uint32_t>(low - 0xDC00));
                        i += 6; // consume "\uXXXX" of the low surrogate
                    }
                }

                AppendUtf8CodePoint(unescaped, codePoint);
                break;
            }
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
    auto gameServerId = ExtractJsonStringValue(configJson, "gameServerId");
    auto ingestBaseUrl = ExtractJsonStringValue(configJson, "ingestBaseUrl");
    auto ingestSubscriptionKey = ExtractJsonStringValue(configJson, "ingestSubscriptionKey");
    auto gameType = ExtractJsonStringValue(configJson, "gameType");
    auto portalPluginHealthEnabled = ExtractJsonBoolValue(configJson, "portalPluginHealthEnabled");
    auto portalPluginHealthMinPower = ExtractJsonIntValue(configJson, "portalPluginHealthMinPower");

    if (!gameServerId.has_value() || !ingestBaseUrl.has_value() || !ingestSubscriptionKey.has_value())
    {
        return std::nullopt;
    }

    PluginConfig config;
    config.GameServerId = Trim(*gameServerId);
    config.IngestBaseUrl = NormalizeBaseUrl(Trim(*ingestBaseUrl));
    config.IngestSubscriptionKey = Trim(*ingestSubscriptionKey);

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

std::string BuildSubscriptionKeyHeaders(const std::string& subscriptionKey)
{
    return "Ocp-Apim-Subscription-Key: " + subscriptionKey + "\r\nAccept: application/json\r\n";
}

bool IsSafeCommandReason(std::string_view reason)
{
    return !reason.empty() && reason.size() <= 256 &&
        reason.find_first_of(";\r\n") == std::string_view::npos;
}

std::string QuoteCommandReason(std::string reason)
{
    std::replace(reason.begin(), reason.end(), '"', '\'');
    return "\"" + reason + "\"";
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

// A legacy CoD4x player id encodes the canonical PUID as (4 << 40) | (canonicalPuid >> 24); the
// low 24 bits of the canonical id are not recoverable from the legacy id alone, which is why the
// resolver relies on the live roster to recover the full canonical id.
constexpr std::uint64_t kLegacyCoD4xIdentifierPrefix = 4;
constexpr std::uint64_t kLegacyCoD4xPayloadMask = (static_cast<std::uint64_t>(1) << 40) - 1;
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
    lastTickUnixSeconds = nowUnixSeconds;

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
    AdvanceVpnEvaluation(host, nowUnixSeconds);
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
    if (playerState.ConnectionGeneration == 0)
    {
        playerState.ConnectionGeneration = ++nextConnectionGeneration;
    }

    // Only overwrite a previously captured IP when we have a real address for this slot.
    // Never fabricate a placeholder (e.g. "0.0.0.0") — an unknown IP must stay empty so the
    // downstream processors skip persistence rather than polluting the player record.
    const std::string normalizedIp = NormalizeIpAddress(std::string(ipAddress));
    if (!normalizedIp.empty())
    {
        playerState.IpAddress = normalizedIp;
    }

    if (playerState.ConnectedAtUnixSeconds == 0)
    {
        playerState.ConnectedAtUnixSeconds = host.GetUnixTimeSeconds();
    }

    if (playerState.Username.empty())
    {
        playerState.Username = Trim(host.GetPlayerName(slot));
    }

    QueueVpnEvaluation(playerState);
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
    const std::string currentGuid = std::to_string(playerId);

    // Always keep the roster state in sync with the live slot, even for a player who carried across
    // a map rotation. OnClientEnterWorld re-fires for every player on each map rotation (OnExitLevel
    // clears the per-slot map) with no intervening OnPlayerDC; populating connectedPlayers here (and
    // not only for freshly connecting GUIDs) ensures carried-over players remain in the periodic
    // server-status snapshot rather than silently dropping out until they next chat.
    ConnectedPlayerState& playerState = connectedPlayers[slot];
    if (!playerState.PlayerGuid.empty() && playerState.PlayerGuid != currentGuid)
    {
        playerState = ConnectedPlayerState{};
    }

    playerState.SlotId = slot;
    if (playerState.ConnectionGeneration == 0)
    {
        playerState.ConnectionGeneration = ++nextConnectionGeneration;
    }
    playerState.PlayerGuid = currentGuid;
    playerState.SteamId = host.GetPlayerSteamId(slot);
    playerState.Username = Trim(host.GetPlayerName(slot));
    playerState.Score = host.GetPlayerScore(slot);
    if (playerState.ConnectedAtUnixSeconds == 0)
    {
        playerState.ConnectedAtUnixSeconds = nowUnixSeconds;
    }

    // playerState.IpAddress may be empty when this slot never fired OnPlayerConnect under the
    // plugin (e.g. hot-load onto a populated server, or a slot re-entering the world after a map
    // rotation). Leave it empty rather than fabricating "0.0.0.0"; downstream treats empty as
    // "IP unknown" and skips persistence.

    // Emit player-connected at most once per connection. The guard is keyed on the player's session
    // GUID and survives the level boundary. connectEmittedGuids is reset only on genuine disconnect
    // (HandlePlayerDisconnected) and pruned on level exit (HandleServerExited), so a real reconnect
    // re-emits.
    if (connectEmittedGuids.count(currentGuid) != 0)
    {
        return;
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

    connectEmittedGuids.insert(currentGuid);
    QueueVpnEvaluation(playerState);
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

        if (state.ConnectionGeneration == 0)
        {
            state.ConnectionGeneration = ++nextConnectionGeneration;
        }

        QueueVpnEvaluation(state);
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

    // Reset the once-per-connection guard so a genuine reconnect for this player emits again.
    if (!state.PlayerGuid.empty())
    {
        connectEmittedGuids.erase(state.PlayerGuid);
        std::erase_if(pendingVpnEvaluations, [&](const PendingVpnEvaluation& evaluation) {
            return evaluation.ConnectionGeneration == state.ConnectionGeneration;
        });
    }

    if (activeVpnEvaluation.has_value() &&
        activeVpnEvaluation->ConnectionGeneration == state.ConnectionGeneration)
    {
        ResetVpnEvaluation(host);
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

    const std::string trimmedMessage = Trim(StripLeadingControlCharacters(std::string(message)));
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
    // Preserve the once-per-connection guard across the level boundary (players carry over a map
    // rotation without an OnPlayerDC), but prune GUIDs no longer present (e.g. a missed OnPlayerDC)
    // so the set cannot grow unbounded.
    std::unordered_set<std::string> stillConnected;
    stillConnected.reserve(connectedPlayers.size());
    for (const auto& entry : connectedPlayers)
    {
        if (!entry.second.PlayerGuid.empty())
        {
            stillConnected.insert(entry.second.PlayerGuid);
        }
    }

    for (auto it = connectEmittedGuids.begin(); it != connectEmittedGuids.end();)
    {
        if (stillConnected.count(*it) != 0)
        {
            ++it;
        }
        else
        {
            it = connectEmittedGuids.erase(it);
        }
    }

    connectedPlayers.clear();
}

std::string PluginRuntime::ResolveBanPlayerGuid(std::uint64_t playerId) const
{
    if (playerId == 0)
    {
        return {};
    }

    const std::string rawKey = std::to_string(playerId);

    // cod4x delivers the canonical 64-bit player id to OnPlayerGetBanStatus / GetPlayerId, but the
    // OnPlayerAddBan/RemoveBan hooks can deliver the shorter legacy id. The portal (and the agent's
    // ban reconcile) key every ban on the canonical PUID, so normalise a legacy id back to the
    // canonical form using the connected roster. This lets the agent import a server-side ban
    // without a fragile prefix search, and lets the plugin evict it once the portal reports it.
    //
    // Thread-safety: this reads connectedPlayers without a lock. That is safe because the only
    // callers (HandlePlayerBanAdded / HandlePlayerBanRemoved, from the OnPlayerAddBan /
    // OnPlayerRemoveBan engine hooks) run on the main server thread, the same thread that mutates
    // connectedPlayers via the connect / disconnect / frame handlers. This is deliberately NOT used
    // by TryFindPlayerBanMessage, whose OnPlayerGetBanStatus caller can run on the authentication
    // thread where a lock-free roster read would be a data race.
    //
    // Because the low 24 bits of the canonical id are discarded when forming the legacy id, two
    // connected PUIDs that differ only in those bits would derive the same legacy id; the first
    // roster match wins. Such a collision is vanishingly unlikely and the fallback stays correct.
    for (const auto& [slot, state] : connectedPlayers)
    {
        static_cast<void>(slot);
        if (state.PlayerGuid.empty())
        {
            continue;
        }

        if (state.PlayerGuid == rawKey)
        {
            return state.PlayerGuid; // already the canonical id
        }

        std::uint64_t canonical = 0;
        if (!TryParseUint64(state.PlayerGuid, canonical))
        {
            continue;
        }

        const std::uint64_t derivedLegacy =
            (kLegacyCoD4xIdentifierPrefix << 40) | ((canonical >> 24) & kLegacyCoD4xPayloadMask);
        if (derivedLegacy == playerId)
        {
            return state.PlayerGuid;
        }
    }

    return rawKey;
}

void PluginRuntime::HandlePlayerBanAdded(
    std::uint64_t playerId,
    std::string_view reason,
    std::uint64_t adminSteamId,
    std::string_view playerName,
    std::int64_t expireUnixSeconds)
{
    const std::string playerGuid = ResolveBanPlayerGuid(playerId);
    if (playerGuid.empty())
    {
        return;
    }

    ServerOriginatedBan ban;
    ban.PlayerGuid = playerGuid;
    ban.PlayerName = SanitiseBanField(std::string(playerName));
    // The nick is player-controlled; strip the field delimiter so a crafted name cannot shift the
    // agent's dumpbanlist parse into a later field (e.g. forging the [PORTAL-BAN] import-skip marker).
    std::replace(ban.PlayerName.begin(), ban.PlayerName.end(), ';', ' ');
    ban.AdminSteamId = adminSteamId != 0 ? std::to_string(adminSteamId) : std::string("System/Rcon");
    ban.Reason = SanitiseBanField(std::string(reason));
    if (ban.Reason.empty())
    {
        ban.Reason = std::string(kDefaultBanMessage);
    }
    // A native permban reports expire as -1 (or 0); a tempban reports a future unix timestamp.
    ban.ExpireUnixSeconds = expireUnixSeconds > 0 ? expireUnixSeconds : -1;

    {
        std::lock_guard<std::mutex> guard(activeBanCacheMutex);
        serverOriginatedBansByPlayerGuid[playerGuid] = std::move(ban);
    }
}

void PluginRuntime::HandlePlayerBanRemoved(std::uint64_t playerId)
{
    const std::string playerGuid = ResolveBanPlayerGuid(playerId);
    if (playerGuid.empty())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(activeBanCacheMutex);
        activeBanMessagesByPlayerGuid.erase(playerGuid);
        serverOriginatedBansByPlayerGuid.erase(playerGuid);
    }

    nextBanSyncUnixSeconds.store(0, std::memory_order_relaxed);
}

bool PluginRuntime::TryFindPlayerBanMessage(std::uint64_t playerId, std::string& message) const
{
    const std::string playerGuid = BuildPlayerGuidKey(playerId);
    if (playerGuid.empty())
    {
        return false;
    }

    std::lock_guard<std::mutex> guard(activeBanCacheMutex);

    const auto portalIt = activeBanMessagesByPlayerGuid.find(playerGuid);
    if (portalIt != activeBanMessagesByPlayerGuid.end())
    {
        message = portalIt->second.empty() ? std::string(kDefaultBanMessage) : portalIt->second;
        return true;
    }

    const auto serverIt = serverOriginatedBansByPlayerGuid.find(playerGuid);
    if (serverIt != serverOriginatedBansByPlayerGuid.end())
    {
        const ServerOriginatedBan& serverBan = serverIt->second;

        // An expired temporary server-side ban must stop being enforced (the native ban has already
        // lapsed); it is pruned lazily by RenderServerBanListDump / evicted on portal import.
        const std::int64_t nowUnixSeconds = static_cast<std::int64_t>(std::time(nullptr));
        if (serverBan.ExpireUnixSeconds > 0 && serverBan.ExpireUnixSeconds <= nowUnixSeconds)
        {
            return false;
        }

        message = serverBan.Reason.empty() ? std::string(kDefaultBanMessage) : serverBan.Reason;
        return true;
    }

    return false;
}

bool PluginRuntime::TryGetPlayerBanMessage(std::uint64_t playerId, std::string& message) const
{
    banStatusCheckCount.fetch_add(1, std::memory_order_relaxed);
    if (playerId == 0)
    {
        banStatusZeroPlayerIdCount.fetch_add(1, std::memory_order_relaxed);
        banStatusMissCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const bool found = TryFindPlayerBanMessage(playerId, message);
    (found ? banStatusHitCount : banStatusMissCount).fetch_add(1, std::memory_order_relaxed);
    return found;
}

bool PluginRuntime::TryGetAuthenticatedPlayerBanMessage(std::uint64_t playerId, std::string& message) const
{
    authenticatedBanCheckCount.fetch_add(1, std::memory_order_relaxed);
    if (playerId == 0)
    {
        authenticatedBanZeroPlayerIdCount.fetch_add(1, std::memory_order_relaxed);
        authenticatedBanMissCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const bool found = TryFindPlayerBanMessage(playerId, message);
    (found ? authenticatedBanHitCount : authenticatedBanMissCount).fetch_add(1, std::memory_order_relaxed);
    return found;
}

std::string PluginRuntime::RenderServerBanListDump()
{
    const std::int64_t nowUnixSeconds = static_cast<std::int64_t>(std::time(nullptr));

    std::string output;
    int index = 0;

    std::lock_guard<std::mutex> guard(activeBanCacheMutex);

    // Prune expired temporary bans first so they are neither reported nor re-imported.
    for (auto it = serverOriginatedBansByPlayerGuid.begin(); it != serverOriginatedBansByPlayerGuid.end();)
    {
        if (it->second.ExpireUnixSeconds > 0 && it->second.ExpireUnixSeconds <= nowUnixSeconds)
        {
            it = serverOriginatedBansByPlayerGuid.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const auto& [playerGuid, ban] : serverOriginatedBansByPlayerGuid)
    {
        std::string expireToken = "Never";
        if (ban.ExpireUnixSeconds > 0)
        {
            const std::time_t expireTime = static_cast<std::time_t>(ban.ExpireUnixSeconds);
            std::array<char, 32> buffer{};
            std::tm utc{};
#if defined(_WIN32)
            gmtime_s(&utc, &expireTime);
#else
            gmtime_r(&expireTime, &utc);
#endif
            if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) > 0)
            {
                expireToken = buffer.data();
            }
        }

        output += std::to_string(index);
        output += " playerid: ";
        output += playerGuid;
        output += "; nick: ";
        output += ban.PlayerName;
        output += "; adminsteamid: ";
        output += ban.AdminSteamId.empty() ? std::string("System/Rcon") : ban.AdminSteamId;
        output += "; expire: ";
        output += expireToken;
        output += "; reason: ";
        output += ban.Reason;
        output += "\n";
        ++index;
    }

    output += std::to_string(index);
    output += " Active bans\n";

    return output;
}

std::string PluginRuntime::RenderPortalBanListDump() const
{
    std::vector<std::pair<std::string, std::string>> portalBans;
    {
        std::lock_guard<std::mutex> guard(activeBanCacheMutex);
        portalBans.assign(activeBanMessagesByPlayerGuid.begin(), activeBanMessagesByPlayerGuid.end());
    }

    std::sort(portalBans.begin(), portalBans.end());

    std::string output;
    int index = 0;
    for (const auto& [playerGuid, banMessage] : portalBans)
    {
        const std::string safePlayerGuid = SanitiseBanField(playerGuid);
        const std::string safeBanMessage = SanitiseBanField(
            banMessage.empty() ? std::string(kDefaultBanMessage) : banMessage);

        output += std::to_string(index);
        output += " playerid: ";
        output += safePlayerGuid;
        output += "; reason: ";
        output += safeBanMessage;
        output += "\n";
        ++index;
    }

    output += std::to_string(index);
    output += " Active portal bans\n";

    return output;
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
    return loadedConfig.has_value() && !loadedConfig->IngestBaseUrl.empty() && !loadedConfig->IngestSubscriptionKey.empty() &&
        !loadedConfig->GameType.empty();
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
            LogInfo(host, "ingest egress disabled; configure ingestBaseUrl, ingestSubscriptionKey, and gameType to enable event emission");
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
                const std::string timeoutMessage = "ingest request timed out" + BuildIngestRequestContext(nowUnixSeconds);
                AbortIngest(host, nowUnixSeconds, timeoutMessage);
            }

            return;
        }

        host.EndHttpRequest(ingestRequest);
        ingestRequest = nullptr;

        if (status == HttpRequestStatus::Failed)
        {
            const std::string failedMessage = "ingest request failed" + BuildIngestRequestContext(nowUnixSeconds);
            AbortIngest(host, nowUnixSeconds, failedMessage);
            return;
        }

        if (ingestStage == IngestStage::PostingBatch)
        {
            if (response.StatusCode >= 200 && response.StatusCode < 300)
            {
                DropBufferedEventsByIndex(ingestBatchIndices);
                LogDebug(
                    host,
                    "ingest batch POST succeeded" + BuildIngestRequestContext(nowUnixSeconds));
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
            LogError(host, "ingest batch POST returned HTTP " + std::to_string(response.StatusCode) + BuildIngestRequestContext(nowUnixSeconds));

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

    if (!StartIngestBatchRequest(host, nowUnixSeconds))
    {
        AbortIngest(host, nowUnixSeconds, "failed to start ingest batch request");
    }
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

    std::string headers = BuildSubscriptionKeyHeaders(loadedConfig->IngestSubscriptionKey);
    headers += "Content-Type: application/json\r\n";

    const std::string requestUrl = loadedConfig->IngestBaseUrl + QueueEndpointPath(loadedConfig->IngestBaseUrl, ingestBatchQueueName);
    LogDebug(
        host,
        "starting ingest batch POST to " + requestUrl +
            " queue=" + ingestBatchQueueName +
            " eventCount=" + std::to_string(ingestBatchIndices.size()) +
            " payloadBytes=" + std::to_string(ingestBatchPayload.size()));

    ingestRequest = host.BeginHttpRequest(requestUrl, "POST", ingestBatchPayload, headers);
    if (ingestRequest == nullptr)
    {
        return false;
    }

    ingestRequestStartedUnixSeconds = nowUnixSeconds;
    ingestStage = IngestStage::PostingBatch;
    return true;
}

std::string PluginRuntime::BuildIngestRequestContext(std::int64_t nowUnixSeconds) const
{
    std::string context;

    context += " stage=";
    switch (ingestStage)
    {
        case IngestStage::Idle:
            context += "idle";
            break;
        case IngestStage::PostingBatch:
            context += "posting-batch";
            break;
    }

    const std::int64_t elapsedSeconds = std::max<std::int64_t>(0, nowUnixSeconds - ingestRequestStartedUnixSeconds);
    context += " elapsedSeconds=" + std::to_string(elapsedSeconds);

    if (ingestStage == IngestStage::PostingBatch)
    {
        if (!ingestBatchQueueName.empty())
        {
            context += " queue=" + ingestBatchQueueName;
        }

        context += " eventCount=" + std::to_string(ingestBatchIndices.size());
        context += " payloadBytes=" + std::to_string(ingestBatchPayload.size());

        if (loadedConfig.has_value() && !loadedConfig->IngestBaseUrl.empty() && !ingestBatchQueueName.empty())
        {
            context += " url=" + loadedConfig->IngestBaseUrl + QueueEndpointPath(loadedConfig->IngestBaseUrl, ingestBatchQueueName);
        }
    }

    return context;
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

void PluginRuntime::QueueVpnEvaluation(ConnectedPlayerState& playerState)
{
    if (playerState.PlayerGuid.empty() || playerState.Username.empty() ||
        playerState.IpAddress.empty() || playerState.SlotId < 0 ||
        playerState.ConnectionGeneration == 0 || playerState.VpnEvaluationQueued)
    {
        return;
    }

    if (pendingVpnEvaluations.size() >= kMaxPendingVpnEvaluations)
    {
        pendingVpnEvaluations.pop_front();
    }

    pendingVpnEvaluations.push_back(PendingVpnEvaluation{
        playerState.PlayerGuid,
        playerState.Username,
        playerState.IpAddress,
        playerState.SlotId,
        playerState.ConnectionGeneration});
    playerState.VpnEvaluationQueued = true;
}

void PluginRuntime::AdvanceVpnEvaluation(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    if (!loadedConfig.has_value() || !IsIngestConfigured())
    {
        return;
    }

    if (vpnEvaluationStage == VpnEvaluationStage::Idle)
    {
        StartVpnEvaluationRequest(host, nowUnixSeconds);
        return;
    }

    if (vpnEvaluationRequest == nullptr)
    {
        vpnEvaluationStage = VpnEvaluationStage::Idle;
        activeVpnEvaluation.reset();
        return;
    }

    HttpResponse response;
    const HttpRequestStatus status = host.PollHttpRequest(vpnEvaluationRequest, response);
    if (status == HttpRequestStatus::Pending)
    {
        if (nowUnixSeconds - vpnEvaluationRequestStartedUnixSeconds >= kVpnEvaluationRequestDeadlineSeconds)
        {
            LogError(host, "VPN Protection evaluation timed out; player remains connected");
            ResetVpnEvaluation(host);
        }

        return;
    }

    host.EndHttpRequest(vpnEvaluationRequest);
    vpnEvaluationRequest = nullptr;

    if (status == HttpRequestStatus::Completed && response.StatusCode >= 200 && response.StatusCode < 300)
    {
        CompleteVpnEvaluation(host, response);
    }
    else
    {
        LogError(
            host,
            "VPN Protection evaluation failed; player remains connected (HTTP " +
                std::to_string(response.StatusCode) + ")");
    }

    vpnEvaluationStage = VpnEvaluationStage::Idle;
    activeVpnEvaluation.reset();
}

bool PluginRuntime::StartVpnEvaluationRequest(ICod4xHost& host, std::int64_t nowUnixSeconds)
{
    while (!pendingVpnEvaluations.empty())
    {
        PendingVpnEvaluation evaluation = std::move(pendingVpnEvaluations.front());
        pendingVpnEvaluations.pop_front();

        const auto playerIt = connectedPlayers.find(evaluation.SlotId);
        if (playerIt == connectedPlayers.end() ||
            playerIt->second.PlayerGuid != evaluation.PlayerGuid ||
            playerIt->second.ConnectionGeneration != evaluation.ConnectionGeneration)
        {
            continue;
        }

        activeVpnEvaluation = std::move(evaluation);
        std::string headers = BuildSubscriptionKeyHeaders(loadedConfig->IngestSubscriptionKey);
        headers += "Content-Type: application/json\r\n";
        const std::string url = loadedConfig->IngestBaseUrl + "/vpn-protection/evaluate";
        vpnEvaluationRequest = host.BeginHttpRequest(
            url,
            "POST",
            BuildVpnEvaluationPayload(*activeVpnEvaluation),
            headers);
        if (vpnEvaluationRequest == nullptr)
        {
            activeVpnEvaluation.reset();
            LogError(host, "Unable to start VPN Protection evaluation; player remains connected");
            return false;
        }

        vpnEvaluationRequestStartedUnixSeconds = nowUnixSeconds;
        vpnEvaluationStage = VpnEvaluationStage::Evaluating;
        return true;
    }

    return false;
}

void PluginRuntime::CompleteVpnEvaluation(ICod4xHost& host, const HttpResponse& response)
{
    if (!activeVpnEvaluation.has_value())
    {
        return;
    }

    const auto matched = ExtractJsonBoolValue(response.Body, "matched");
    if (!matched.value_or(false))
    {
        return;
    }

    const auto action = ExtractJsonStringValue(response.Body, "action");
    const auto reason = ExtractJsonStringValue(response.Body, "reason");
    if (!action.has_value() || !reason.has_value() || !IsSafeCommandReason(*reason))
    {
        LogError(host, "VPN Protection returned an invalid action or reason; player remains connected");
        return;
    }

    const PendingVpnEvaluation& evaluation = *activeVpnEvaluation;
    const auto playerIt = connectedPlayers.find(evaluation.SlotId);
    const std::uint64_t livePlayerId = host.GetPlayerId(evaluation.SlotId);
    if (playerIt == connectedPlayers.end() ||
        playerIt->second.PlayerGuid != evaluation.PlayerGuid ||
        playerIt->second.ConnectionGeneration != evaluation.ConnectionGeneration ||
        livePlayerId == 0 ||
        std::to_string(livePlayerId) != evaluation.PlayerGuid)
    {
        LogInfo(host, "VPN Protection decision ignored because the player is no longer connected");
        return;
    }

    const std::string normalizedAction = ToLowerInvariant(*action);
    std::string command;
    if (normalizedAction == "ban")
    {
        command = "banClient " + std::to_string(evaluation.SlotId) + " " + QuoteCommandReason(*reason);
    }
    else if (normalizedAction == "kick")
    {
        command = "onlykick " + std::to_string(evaluation.SlotId) + " " + QuoteCommandReason(*reason);
    }
    else
    {
        return;
    }

    if (host.ExecuteServerCommand(command))
    {
        LogInfo(
            host,
            "VPN Protection executed " + normalizedAction + " for player " + evaluation.PlayerGuid);
    }
    else
    {
        LogError(
            host,
            "VPN Protection failed to execute " + normalizedAction + " for player " + evaluation.PlayerGuid);
    }
}

void PluginRuntime::ResetVpnEvaluation(ICod4xHost& host)
{
    if (vpnEvaluationRequest != nullptr)
    {
        host.EndHttpRequest(vpnEvaluationRequest);
        vpnEvaluationRequest = nullptr;
    }

    vpnEvaluationStage = VpnEvaluationStage::Idle;
    activeVpnEvaluation.reset();
}

std::string PluginRuntime::BuildVpnEvaluationPayload(const PendingVpnEvaluation& evaluation) const
{
    return "{\"serverId\":\"" + JsonEscape(serverContext.GameServerId) +
        "\",\"ipAddress\":\"" + JsonEscape(evaluation.IpAddress) +
        "\",\"playerGuid\":\"" + JsonEscape(evaluation.PlayerGuid) +
        "\",\"username\":\"" + JsonEscape(evaluation.Username) +
        "\",\"slotId\":" + std::to_string(evaluation.SlotId) + "}";
}

bool PluginRuntime::IsRepositoryConfigured() const
{
    return loadedConfig.has_value() && !loadedConfig->IngestBaseUrl.empty() &&
        !loadedConfig->IngestSubscriptionKey.empty() && !loadedConfig->GameServerId.empty();
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

            std::vector<std::string> liftedBanGuids;
            {
                std::lock_guard<std::mutex> guard(activeBanCacheMutex);

                // Snapshot the previously enforced portal bans so we can detect portal-side lifts
                // (bans present last sync but absent now) and clear their residual server-side state.
                std::unordered_set<std::string> previousPortalBanGuids;
                previousPortalBanGuids.reserve(activeBanMessagesByPlayerGuid.size());
                for (const auto& [playerGuid, banMessage] : activeBanMessagesByPlayerGuid)
                {
                    previousPortalBanGuids.insert(playerGuid);
                }

                activeBanMessagesByPlayerGuid = std::move(pendingActiveBanMessagesByPlayerGuid);

                // Drop any server-originated pending ban the portal now reports as active — it has
                // been imported by the agent's reconcile, so it no longer needs surfacing via
                // dumpbanlist (it is now enforced from the portal-synced cache).
                for (auto it = serverOriginatedBansByPlayerGuid.begin(); it != serverOriginatedBansByPlayerGuid.end();)
                {
                    if (activeBanMessagesByPlayerGuid.find(it->first) != activeBanMessagesByPlayerGuid.end())
                    {
                        it = serverOriginatedBansByPlayerGuid.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }

                // A ban enforced from the portal cache last sync that is now absent — and is not a
                // still-pending server-origin ban — was lifted on the portal (portal is the only
                // place a ban can be unbanned). Issue a native `unban` for it below so the engine's
                // residual short-lived IP ban is cleared and the lift takes effect immediately.
                for (const auto& playerGuid : previousPortalBanGuids)
                {
                    if (activeBanMessagesByPlayerGuid.find(playerGuid) == activeBanMessagesByPlayerGuid.end() &&
                        serverOriginatedBansByPlayerGuid.find(playerGuid) == serverOriginatedBansByPlayerGuid.end())
                    {
                        liftedBanGuids.push_back(playerGuid);
                    }
                }
            }

            // Bound the number of native unbans issued in a single sync so a spurious wholesale
            // clear (e.g. a transient empty portal response) cannot flood the command buffer; any
            // residual engine IP bans not cleared here self-expire (banlist_maxipbantime).
            constexpr std::size_t kMaxLiftUnbansPerSync = 25;
            if (liftedBanGuids.size() > kMaxLiftUnbansPerSync)
            {
                LogInfo(
                    host,
                    "portal reported " + std::to_string(liftedBanGuids.size()) +
                        " lifted bans in a single sync; capping native unban commands at " +
                        std::to_string(kMaxLiftUnbansPerSync));
                liftedBanGuids.resize(kMaxLiftUnbansPerSync);
            }

            for (const auto& playerGuid : liftedBanGuids)
            {
                // Deferred via the engine command buffer; safe/idempotent (silent if no record) and
                // also clears the short-lived engine IP ban associated with the original ban.
                host.ExecuteServerCommand("unban " + playerGuid);
                LogDebug(host, "issued native unban for portal-lifted ban " + playerGuid);
            }

            EnforceCachedBansForConnectedPlayers(host);

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

    activeBanFetchSkipEntries = 0;
    pendingActiveBanMessagesByPlayerGuid.clear();

    if (!StartActiveBanFetchRequest(host, nowUnixSeconds, activeBanFetchSkipEntries))
    {
        AbortBanSync(host, nowUnixSeconds, "failed to start repository active-ban request");
    }
}

void PluginRuntime::EnforceCachedBansForConnectedPlayers(ICod4xHost& host)
{
    const int slotCount = host.GetSlotCount();
    for (int slot = 0; slot < slotCount; ++slot)
    {
        const std::uint64_t playerId = host.GetPlayerId(slot);
        if (playerId == 0)
        {
            continue;
        }

        std::string banMessage;
        if (!TryFindPlayerBanMessage(playerId, banMessage))
        {
            continue;
        }

        host.DropPlayer(slot, banMessage);
        proactiveBanDropAttemptCount.fetch_add(1, std::memory_order_relaxed);
        LogInfo(host, "requested proactive drop for cached-banned player " + std::to_string(playerId) +
            " from slot " + std::to_string(slot));
    }
}

bool PluginRuntime::StartActiveBanFetchRequest(ICod4xHost& host, std::int64_t nowUnixSeconds, int skipEntries)
{
    const std::string gameType = loadedConfig->GameType.empty() ? "CallOfDuty4x" : loadedConfig->GameType;
    const std::string requestUrl = loadedConfig->IngestBaseUrl +
        "/active-bans?gameType=" + UrlEncode(gameType) +
        "&skipEntries=" + std::to_string(skipEntries) +
        "&takeEntries=" + std::to_string(kActiveBanPageSize);

    std::string headers = BuildSubscriptionKeyHeaders(loadedConfig->IngestSubscriptionKey);

    LogDebug(
        host,
        "starting active-ban request (skipEntries=" + std::to_string(skipEntries) + ") to " + requestUrl);
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

    // Reconcile the tracked roster against the live server slots so the snapshot is authoritative
    // regardless of missed or re-fired connect callbacks (map rotation) or a hot-load onto a
    // populated server. RCON status reads the engine directly; enumerating slots here keeps the
    // periodic snapshot in agreement with it instead of reflecting only event-observed players.
    const int slotCount = host.GetSlotCount();
    for (int slot = 0; slot < slotCount; ++slot)
    {
        const std::uint64_t playerId = host.GetPlayerId(slot);
        if (playerId == 0)
        {
            // Empty slot or bot (bots report id 0) — drop any stale entry for this slot.
            connectedPlayers.erase(slot);
            continue;
        }

        const std::string currentGuid = std::to_string(playerId);

        ConnectedPlayerState& state = connectedPlayers[slot];

        // If the slot is now occupied by a different player (e.g. a missed OnPlayerDC/OnPlayerConnect
        // pair reused the slot), discard the previous occupant's identity and captured IP so we
        // never bind the old username/steamId/address to the new player's GUID.
        if (!state.PlayerGuid.empty() && state.PlayerGuid != currentGuid)
        {
            state = ConnectedPlayerState{};
        }

        state.SlotId = slot;
        state.PlayerGuid = currentGuid;
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
        state.Score = host.GetPlayerScore(slot);
    }

    // Prune slots that remain unresolved (no GUID/username) so the snapshot never advertises a
    // half-populated player. BuildServerStatusPayload applies the same filter defensively.
    for (auto it = connectedPlayers.begin(); it != connectedPlayers.end();)
    {
        if (it->second.PlayerGuid.empty() || it->second.Username.empty())
        {
            it = connectedPlayers.erase(it);
            continue;
        }

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
        payload += ",\"ipAddress\":\"" + JsonEscape(state.IpAddress) + "\"";
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
    static constexpr char kHexDigits[] = "0123456789ABCDEF";

    std::string escaped;
    escaped.reserve(value.size() * 2);

    for (const unsigned char c : value)
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
                if (c < 0x20 || c > 0x7E)
                {
                    escaped += "\\u00";
                    escaped.push_back(kHexDigits[(c >> 4) & 0x0F]);
                    escaped.push_back(kHexDigits[c & 0x0F]);
                }
                else
                {
                    escaped.push_back(static_cast<char>(c));
                }
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

std::string PluginRuntime::QueueEndpointPath(std::string_view ingestBaseUrl, std::string_view queueName)
{
    const std::string baseUrl = ToLowerInvariant(std::string(ingestBaseUrl));
    const std::string ingestSuffix = "/ingest";

    const bool baseContainsIngestSegment =
        baseUrl.size() >= ingestSuffix.size() &&
        baseUrl.compare(baseUrl.size() - ingestSuffix.size(), ingestSuffix.size(), ingestSuffix) == 0;

    if (baseContainsIngestSegment)
    {
        return "/events/" + std::string(queueName);
    }

    return "/ingest/events/" + std::string(queueName);
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
        case BanSyncStage::FetchingActiveBans:
            banSyncStageName = "FetchingActiveBans";
            break;
        case BanSyncStage::Idle:
        default:
            break;
    }

    std::vector<std::string> lines;
    lines.reserve(10);

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
        "banSyncStage=" + banSyncStageName +
        " nextBanSyncUtc=" + FormatOptionalUnixTimestamp(nextBanSyncUnixSeconds.load(std::memory_order_relaxed)) +
        " activeBanCacheCount=" + std::to_string(activeBanCacheCount) +
        " pendingBanCacheCount=" + std::to_string(pendingActiveBanMessagesByPlayerGuid.size()) +
        " banSyncInFlight=" + std::string(banSyncRequest != nullptr ? "true" : "false"));

    lines.push_back(
        "banEnforcementStatusChecks=" + std::to_string(banStatusCheckCount.load(std::memory_order_relaxed)) +
        " statusHits=" + std::to_string(banStatusHitCount.load(std::memory_order_relaxed)) +
        " statusMisses=" + std::to_string(banStatusMissCount.load(std::memory_order_relaxed)) +
        " statusZeroPlayerIds=" + std::to_string(banStatusZeroPlayerIdCount.load(std::memory_order_relaxed)) +
        " authenticatedChecks=" + std::to_string(authenticatedBanCheckCount.load(std::memory_order_relaxed)) +
        " authenticatedHits=" + std::to_string(authenticatedBanHitCount.load(std::memory_order_relaxed)) +
        " authenticatedMisses=" + std::to_string(authenticatedBanMissCount.load(std::memory_order_relaxed)) +
        " authenticatedZeroPlayerIds=" + std::to_string(authenticatedBanZeroPlayerIdCount.load(std::memory_order_relaxed)) +
        " proactiveDropAttempts=" + std::to_string(proactiveBanDropAttemptCount.load(std::memory_order_relaxed)));

    lines.push_back(
        "ingestStage=" + ingestStageName +
        " bufferedEventCount=" + std::to_string(bufferedEvents.size()) +
        " ingestFailureCount=" + std::to_string(ingestConsecutiveFailureCount) +
        " nextIngestAttemptUtc=" + FormatOptionalUnixTimestamp(nextIngestAttemptUnixSeconds) +
        " ingestInFlight=" + std::string(ingestRequest != nullptr ? "true" : "false"));

    if (loadedConfig.has_value() && ingestStage == IngestStage::PostingBatch && !ingestBatchQueueName.empty())
    {
        const std::int64_t elapsedSeconds = std::max<std::int64_t>(0, nowUnixSeconds - ingestRequestStartedUnixSeconds);
        lines.push_back(
            "ingestRequestUrl=" + loadedConfig->IngestBaseUrl + QueueEndpointPath(loadedConfig->IngestBaseUrl, ingestBatchQueueName) +
            " ingestElapsedSeconds=" + std::to_string(elapsedSeconds) +
            " ingestBatchQueue=" + ingestBatchQueueName +
            " ingestBatchEventCount=" + std::to_string(ingestBatchIndices.size()));
    }

    lines.push_back(
        "connectedPlayerCount=" + std::to_string(connectedPlayers.size()) +
        " serverContextLastRefreshUtc=" + FormatOptionalUnixTimestamp(serverContext.LastRefreshUnixSeconds));

    lines.push_back(
        "lastTickUtc=" + FormatOptionalUnixTimestamp(lastTickUnixSeconds) +
        " secondsSinceLastTick=" +
        (lastTickUnixSeconds > 0 ? std::to_string(std::max<std::int64_t>(0, nowUnixSeconds - lastTickUnixSeconds)) : std::string("n/a")));

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

void NotifyPlayerBanAdded(
    std::uint64_t playerId,
    std::string_view reason,
    std::uint64_t adminSteamId,
    std::string_view playerName,
    std::int64_t expireUnixSeconds)
{
    g_runtime.HandlePlayerBanAdded(playerId, reason, adminSteamId, playerName, expireUnixSeconds);
}

void NotifyPlayerBanRemoved(std::uint64_t playerId)
{
    g_runtime.HandlePlayerBanRemoved(playerId);
}

bool TryGetPlayerBanMessage(std::uint64_t playerId, std::string& message)
{
    return g_runtime.TryGetPlayerBanMessage(playerId, message);
}

bool TryGetAuthenticatedPlayerBanMessage(std::uint64_t playerId, std::string& message)
{
    return g_runtime.TryGetAuthenticatedPlayerBanMessage(playerId, message);
}

std::string RenderServerBanListDump()
{
    return g_runtime.RenderServerBanListDump();
}

std::string RenderPortalBanListDump()
{
    return g_runtime.RenderPortalBanListDump();
}

const EffectiveServerContext& GetServerContext()
{
    return g_runtime.GetServerContext();
}
}
