#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    std::string IngestBaseUrl;
    std::string IngestApiResource;
    std::string GameServerId;
    std::string GameType;
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
    virtual void SendChat(int slot, std::string_view message) = 0;
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

    virtual std::uint64_t GetPlayerId(int slot) const = 0;
    virtual std::uint64_t GetPlayerSteamId(int slot) const = 0;
    virtual std::string GetPlayerName(int slot) const = 0;
    virtual int GetSlotCount() const = 0;
    virtual int GetPlayerScore(int slot) const = 0;
    virtual std::string GetCvarString(std::string_view cvarName) const = 0;

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
    void HandlePlayerConnect(ICod4xHost& host, int slot, std::string_view ipAddress);
    void HandlePlayerConnected(ICod4xHost& host, int slot);
    void HandleClientAuthorized(ICod4xHost& host);
    void HandlePlayerDisconnected(ICod4xHost& host, int slot);
    void HandleChatMessage(ICod4xHost& host, int slot, std::string_view message, bool teamMessage);
    void HandleClientCommand(ICod4xHost& host, int slot, std::string_view command, bool fromChatMessage = false);
    void HandleServerSpawned(ICod4xHost& host);
    void HandleServerExited(ICod4xHost& host);

    [[nodiscard]] const EffectiveServerContext& GetServerContext() const;

private:
    enum class RefreshStage
    {
        Idle,
        AcquiringToken,
        FetchingGlobalConfig,
        FetchingServerConfig,
        FetchingAdminRoster
    };

    enum class IngestStage
    {
        Idle,
        AcquiringToken,
        PostingBatch
    };

    struct BufferedEvent
    {
        std::string QueueName;
        std::string PayloadJson;
        std::string MessageId;
        std::int64_t CreatedUnixSeconds = 0;
        std::size_t AttemptCount = 0;
    };

    struct ConnectedPlayerState
    {
        std::string PlayerGuid;
        std::string Username;
        std::string IpAddress;
        int SlotId = -1;
        int Score = 0;
        int AppliedAdminPower = 1;
        bool HasAppliedAdminPower = false;
        std::uint64_t SteamId = 0;
        std::int64_t ConnectedAtUnixSeconds = 0;
    };

    struct AdminRosterEntry
    {
        int Power = 1;
        std::vector<std::string> Tags;
    };

    std::string configPath;
    std::string pluginVersion = "0.0.0-unknown";
    std::string chatPrefix = std::string(kDefaultBotPrefix);
    std::string lastHandledCommandToken;
    int lastHandledCommandSlot = -1;
    std::int64_t lastHandledCommandUnixSeconds = 0;
    bool lastHandledCommandFromChat = false;
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
    bool pendingHasAdminRoster = false;
    std::string pendingGlobalConfigPayload;
    std::string pendingServerConfigPayload;
    std::string pendingAdminRosterPayload;

    IngestStage ingestStage = IngestStage::Idle;
    HttpRequestHandle ingestRequest = nullptr;
    std::int64_t ingestRequestStartedUnixSeconds = 0;
    std::string ingestAccessToken;
    std::int64_t ingestAccessTokenExpiresAtUnixSeconds = 0;
    std::int64_t nextIngestAttemptUnixSeconds = 0;
    std::int64_t nextServerStatusUnixSeconds = 0;
    std::size_t ingestConsecutiveFailureCount = 0;
    bool ingestConfigWarningLogged = false;
    std::string ingestBatchQueueName;
    std::string ingestBatchPayload;
    std::vector<std::size_t> ingestBatchIndices;
    std::deque<BufferedEvent> bufferedEvents;
    std::unordered_map<int, ConnectedPlayerState> connectedPlayers;
    bool adminPowerEnabled = false;
    int adminDefaultPower = 1;
    std::unordered_map<std::string, AdminRosterEntry> adminRosterByPlayerGuid;
    std::string adminRosterSnapshotHash;
    std::unordered_set<std::uint64_t> loggedUnsupportedSteamIds;

    bool IsIngestConfigured() const;
    bool IsIngestTokenValid(std::int64_t nowUnixSeconds) const;
    void AdvanceIngest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool StartIngestTokenRequest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool StartIngestBatchRequest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    void AbortIngest(ICod4xHost& host, std::int64_t nowUnixSeconds, std::string_view reason);
    void FlushServerStatusSnapshot(ICod4xHost& host, std::int64_t nowUnixSeconds);

    std::string BuildPlayerConnectedPayload(
        std::int64_t nowUnixSeconds,
        const std::string& messageId,
        const std::string& playerGuid,
        std::uint64_t steamId,
        const std::string& username,
        const std::string& ipAddress,
        int slotId);
    std::string BuildPlayerDisconnectedPayload(
        std::int64_t nowUnixSeconds,
        const std::string& messageId,
        const std::string& playerGuid,
        const std::string& username,
        int slotId);
    std::string BuildChatMessagePayload(
        std::int64_t nowUnixSeconds,
        const std::string& messageId,
        const std::string& playerGuid,
        const std::string& username,
        int slotId,
        std::string_view message,
        bool teamMessage);
    std::string BuildServerConnectedPayload(std::int64_t nowUnixSeconds, const std::string& messageId);
    std::string BuildMapChangePayload(
        std::int64_t nowUnixSeconds,
        const std::string& messageId,
        const std::string& mapName,
        const std::string& gameName);
    std::string BuildServerStatusPayload(std::int64_t nowUnixSeconds, const std::string& messageId, ICod4xHost& host);

    std::string GetMapName(ICod4xHost& host) const;
    std::string GetGameName(ICod4xHost& host) const;
    std::string GetServerTitle(ICod4xHost& host) const;
    std::string GetServerMod(ICod4xHost& host) const;

    void BufferEvent(std::string queueName, std::string payloadJson, std::string messageId, std::int64_t nowUnixSeconds);
    void PruneBufferedEvents(ICod4xHost& host, std::int64_t nowUnixSeconds);
    std::vector<std::size_t> BuildBatchIndicesForQueue(const std::string& queueName, std::size_t maxEvents, std::size_t maxBytes) const;
    void DropBufferedEventsByIndex(const std::vector<std::size_t>& indices);
    std::string BuildBaseEventPrefix(std::int64_t nowUnixSeconds, const std::string& messageId, long long sequenceId) const;
    long long NextSequenceId();
    static std::string GenerateMessageId();
    static std::string JsonEscape(std::string_view value);
    static std::string ToIso8601Utc(std::int64_t unixSeconds);
    static std::string StampPublishedUtc(std::string payloadJson, std::int64_t nowUnixSeconds);
    static std::string QueueEndpointPath(std::string_view queueName);
    static std::string NormalizeIpAddress(std::string ipAddress);
    static std::string Trim(std::string value);
    std::string BuildPrefixedChatMessage(std::string_view message) const;
    void SendPrivateChat(ICod4xHost& host, int slot, std::string_view message) const;
    long long nextSequenceId = 1;

    bool TryLoadConfig(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool IsAccessTokenValid(std::int64_t nowUnixSeconds) const;
    void BeginRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds);
    void AdvanceRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds);
    void AbortRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds, std::string_view reason);
    bool StartGlobalConfigRequest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool StartServerConfigRequest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool StartAdminRosterRequest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    void FinalizeRefresh(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool ApplyCommandReconciliation(ICod4xHost& host);
    bool ApplyAdminPowerReconciliation(ICod4xHost& host);
    int ResolveDesiredAdminPower(const ConnectedPlayerState& playerState) const;
    static std::string BuildAdminRosterSnapshotHash(
        bool enabled,
        int defaultPower,
        const std::unordered_map<std::string, AdminRosterEntry>& entries);
};

std::string BuildOnlineBroadcastMessage(std::string_view prefix, std::string_view version);
int InitializePlugin(ICod4xHost& host, std::string_view version, std::string_view prefix = kDefaultBotPrefix);
void TickPlugin(ICod4xHost& host);
void NotifyPlayerConnect(ICod4xHost& host, int slot, std::string_view ipAddress);
void NotifyPlayerConnected(ICod4xHost& host, int slot);
void NotifyClientAuthorized(ICod4xHost& host);
void NotifyPlayerDisconnected(ICod4xHost& host, int slot);
void NotifyChatMessage(ICod4xHost& host, int slot, std::string_view message, bool teamMessage);
void NotifyClientCommand(ICod4xHost& host, int slot, std::string_view command);
void NotifyServerSpawned(ICod4xHost& host);
void NotifyServerExited(ICod4xHost& host);
const EffectiveServerContext& GetServerContext();
}
