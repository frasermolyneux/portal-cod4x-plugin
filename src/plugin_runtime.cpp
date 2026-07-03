#include "portal_cod4x/plugin_runtime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace portal_cod4x
{
namespace
{
// Maximum wall-clock time a single in-flight HTTP request may stay pending before
// the refresh pipeline abandons it (and frees the handle) to avoid a stalled poll
// wedging future refreshes.
constexpr std::int64_t kHttpRequestDeadlineSeconds = 30;

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

int PluginRuntime::Initialize(ICod4xHost& host, std::string_view version, std::string_view prefix)
{
    const std::string onlineMessage = BuildOnlineBroadcastMessage(prefix, version);
    const std::string normalizedVersion = version.empty() ? "0.0.0-unknown" : std::string(version);

    host.Log("Portal Plugin is online (version " + normalizedVersion + ")");
    host.BroadcastChat(onlineMessage);

    const std::int64_t nowUnixSeconds = host.GetUnixTimeSeconds();
    TryLoadConfig(host, nowUnixSeconds);
    nextRefreshUnixSeconds = 0;

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
    }

    // A refresh is a multi-step, non-blocking pipeline (token -> global config ->
    // server config -> reconcile). While one is in flight, advance it by a single
    // step per frame so the main thread is never blocked on network I/O.
    if (refreshStage != RefreshStage::Idle)
    {
        AdvanceRefresh(host, nowUnixSeconds);
        return;
    }

    if (nowUnixSeconds < nextRefreshUnixSeconds)
    {
        return;
    }

    BeginRefresh(host, nowUnixSeconds);
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
        logConfigIssue("plugin config is invalid; expected tenantId, clientId, clientSecret, repositoryApiBaseUrl, repositoryApiResource, and gameServerId");
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

    if (!serverContext.CommandEnforcementEnabled)
    {
        host.Log("cod4xCommands enforcement disabled for current server context; command powers left unchanged");
        nextRefreshUnixSeconds = nowUnixSeconds + loadedConfig->RefreshIntervalSeconds;
        return;
    }

    if (serverContext.SnapshotHash == lastAppliedSnapshotHash && wasEnforced)
    {
        nextRefreshUnixSeconds = nowUnixSeconds + loadedConfig->RefreshIntervalSeconds;
        return;
    }

    const bool applied = ApplyCommandReconciliation(host);
    nextRefreshUnixSeconds = applied
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

const EffectiveServerContext& GetServerContext()
{
    return g_runtime.GetServerContext();
}
}
