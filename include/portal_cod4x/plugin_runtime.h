#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace portal_cod4x
{
struct HttpResponse
{
    int StatusCode = 0;
    std::string Body;
};

struct PluginConfig
{
    std::string TenantId;
    std::string ClientId;
    std::string ClientSecret;
    std::string RepositoryApiBaseUrl;
    std::string RepositoryApiResource;
    std::string GameServerId;
    int RefreshIntervalSeconds = 120;
};

struct CommandPowerRule
{
    bool Enabled = true;
    int MinPower = 100;
};

struct EffectiveServerContext
{
    std::string GameServerId;
    bool CommandEnforcementEnabled = false;
    std::unordered_map<std::string, CommandPowerRule> CommandRules;
    std::string SnapshotHash;
    std::int64_t LastRefreshUnixSeconds = 0;
};

class ICod4xHost
{
public:
    virtual ~ICod4xHost() = default;

    virtual void BroadcastChat(std::string_view message) = 0;
    virtual void Log(std::string_view message) = 0;
    virtual bool ExecuteServerCommand(std::string_view command) = 0;
    virtual std::optional<HttpResponse> HttpRequest(
        std::string_view url,
        std::string_view method,
        std::string_view body,
        std::string_view additionalHeaders) = 0;
    virtual std::int64_t GetUnixTimeSeconds() const = 0;
};

inline constexpr std::string_view kDefaultBotPrefix = "^4[^1XI-BOT^4]^7";
inline constexpr std::string_view kDefaultConfigFilePath = "portal-cod4x-plugin.config.json";

class PluginRuntime
{
public:
    explicit PluginRuntime(std::string configPath = std::string(kDefaultConfigFilePath));

    int Initialize(ICod4xHost& host, std::string_view version, std::string_view prefix = kDefaultBotPrefix);
    void Tick(ICod4xHost& host);

    [[nodiscard]] const EffectiveServerContext& GetServerContext() const;

private:
    std::string configPath;
    std::optional<PluginConfig> loadedConfig;
    EffectiveServerContext serverContext;
    std::string accessToken;
    std::int64_t accessTokenExpiresAtUnixSeconds = 0;
    std::int64_t nextRefreshUnixSeconds = 0;
    std::int64_t nextConfigLoadAttemptUnixSeconds = 0;
    std::string lastAppliedSnapshotHash;
    std::string lastConfigLoadError;

    bool TryLoadConfig(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool RefreshServerContext(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool EnsureAccessToken(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool ApplyCommandReconciliation(ICod4xHost& host);
};

std::string BuildOnlineBroadcastMessage(std::string_view prefix, std::string_view version);
int InitializePlugin(ICod4xHost& host, std::string_view version, std::string_view prefix = kDefaultBotPrefix);
void TickPlugin(ICod4xHost& host);
const EffectiveServerContext& GetServerContext();
}
