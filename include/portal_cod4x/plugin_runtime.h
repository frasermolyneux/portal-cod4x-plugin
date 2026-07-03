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

// Opaque handle representing a single in-flight, non-blocking HTTP request.
using HttpRequestHandle = void*;

enum class HttpRequestStatus
{
    Pending,
    Completed,
    Failed
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

    // Non-blocking HTTP: BeginHttpRequest kicks off a request and returns an opaque
    // handle (nullptr if the request could not be started). PollHttpRequest is called
    // once per frame to advance the transfer without blocking the main thread, and
    // EndHttpRequest releases the handle once it is no longer needed.
    virtual HttpRequestHandle BeginHttpRequest(
        std::string_view url,
        std::string_view method,
        std::string_view body,
        std::string_view additionalHeaders) = 0;
    virtual HttpRequestStatus PollHttpRequest(HttpRequestHandle handle, HttpResponse& response) = 0;
    virtual void EndHttpRequest(HttpRequestHandle handle) = 0;

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
    enum class RefreshStage
    {
        Idle,
        AcquiringToken,
        FetchingGlobalConfig,
        FetchingServerConfig
    };

    std::string configPath;
    std::optional<PluginConfig> loadedConfig;
    EffectiveServerContext serverContext;
    std::string accessToken;
    std::int64_t accessTokenExpiresAtUnixSeconds = 0;
    std::int64_t nextRefreshUnixSeconds = 0;
    std::int64_t nextConfigLoadAttemptUnixSeconds = 0;
    std::string lastAppliedSnapshotHash;
    std::string lastConfigLoadError;

    RefreshStage refreshStage = RefreshStage::Idle;
    HttpRequestHandle inFlightRequest = nullptr;
    std::int64_t inFlightStartedUnixSeconds = 0;
    bool pendingHasGlobalConfig = false;
    bool pendingHasServerConfig = false;
    std::string pendingGlobalConfigPayload;
    std::string pendingServerConfigPayload;

    bool TryLoadConfig(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool IsAccessTokenValid(std::int64_t nowUnixSeconds) const;
    void BeginRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds);
    void AdvanceRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds);
    void AbortRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds, std::string_view reason);
    bool StartGlobalConfigRequest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool StartServerConfigRequest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    void FinalizeRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool ApplyCommandReconciliation(ICod4xHost& host);
};

std::string BuildOnlineBroadcastMessage(std::string_view prefix, std::string_view version);
int InitializePlugin(ICod4xHost& host, std::string_view version, std::string_view prefix = kDefaultBotPrefix);
void TickPlugin(ICod4xHost& host);
const EffectiveServerContext& GetServerContext();
}
