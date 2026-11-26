#pragma once

#include "portal_cod4x/native_action_executor.h"

#include <cstdint>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
    std::string IngestBaseUrl;
    std::string IngestSubscriptionKey;
    std::string GameServerId;
    std::string GameType;
    int RefreshIntervalSeconds = 120;
    bool PortalPluginHealthEnabled = true;
    int PortalPluginHealthMinPower = 98;
};

struct EffectiveServerContext
{
    std::string GameServerId;
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
    virtual void DropPlayer(int slot, std::string_view reason) = 0;

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
    virtual bool CanPlayerUseCommand(int slot, std::string_view commandName) const = 0;

    virtual std::int64_t GetUnixTimeSeconds() const = 0;
    virtual std::int64_t GetUnixTime100Nanoseconds() const = 0;
};

inline constexpr std::string_view kDefaultBotPrefix = "^4[^1XI-BOT^4]^7";
inline constexpr std::string_view kDefaultConfigFilePath = "portal-cod4x-plugin.config.json";
inline constexpr std::string_view kPortalPluginHealthCommandName = "portalpluginhealth";
inline constexpr std::string_view kPortalPluginLogLevelCommandName = "portalpluginloglevel";

class PluginRuntime
{
public:
    enum class LogLevel : int
    {
        Debug = 1,
        Info = 2,
        Error = 3
    };

    explicit PluginRuntime(std::string configPath = std::string(kDefaultConfigFilePath),
        std::function<std::int64_t()> utcClock = {},
        std::function<std::int64_t()> utcTicksClock = {});

    int Initialize(ICod4xHost& host, std::string_view version, std::string_view prefix = kDefaultBotPrefix);
    void Shutdown(ICod4xHost& host);
    void Tick(ICod4xHost& host);
    void HandlePlayerConnect(ICod4xHost& host, int slot, std::string_view ipAddress);
    void HandlePlayerConnected(ICod4xHost& host, int slot);
    void HandleClientAuthorized(ICod4xHost& host);
    void HandlePlayerDisconnected(ICod4xHost& host, int slot);
    void HandleChatMessage(ICod4xHost& host, int slot, std::string_view message, bool teamMessage);
    void HandleClientCommand(ICod4xHost& host, int slot, std::string_view command, bool fromChatMessage = false);
    void HandlePortalPluginHealthCommand(ICod4xHost& host, int invokerSlot);
    void SendPrivateChat(ICod4xHost& host, int slot, std::string_view message) const;
    void HandleServerSpawned(ICod4xHost& host);
    void HandleServerExited(ICod4xHost& host);
    void HandlePlayerBanAdded(
        std::uint64_t playerId,
        std::string_view reason,
        std::uint64_t adminSteamId = 0,
        std::string_view playerName = {},
        std::int64_t expireUnixSeconds = -1);
    void HandlePlayerBanRemoved(std::uint64_t playerId);
    bool TryGetPlayerBanMessage(std::uint64_t playerId, std::string& message, std::uint64_t steamId = 0) const;
    bool TryGetAuthenticatedPlayerBanMessage(std::uint64_t playerId, std::string& message, std::uint64_t steamId = 0) const;
    // Renders the pending server-originated ban list in the cod4x `dumpbanlist` output format so
    // the agent's RCON reconcile can import bans created directly on the server. Prunes expired
    // temporary bans as a side effect.
    [[nodiscard]] std::string RenderServerBanListDump();
    // Renders every portal-synchronized ban currently used by OnPlayerGetBanStatus.
    [[nodiscard]] std::string RenderPortalBanListDump() const;
    bool TrySetLogLevel(ICod4xHost& host, int levelValue, bool announce = true);
    bool TrySetLogLevel(ICod4xHost& host, std::string_view levelToken, bool announce = true);
    [[nodiscard]] int GetLogLevelValue() const;
    [[nodiscard]] std::string GetLogLevelName() const;

    [[nodiscard]] const EffectiveServerContext& GetServerContext() const;

private:
    enum class IngestStage
    {
        Idle,
        PostingBatch
    };

    enum class BanSyncStage
    {
        Idle,
        FetchingActiveBans
    };

    struct BufferedEvent
    {
        std::string QueueName;
        std::string PayloadJson;
        std::string MessageId;
        std::int64_t CreatedUnixSeconds = 0;
        std::size_t AttemptCount = 0;
        bool Published = false;
    };

    struct ConnectedPlayerState
    {
        std::string PlayerGuid;
        std::string ConnectionId;
        std::string ConnectedObservationId;
        std::string Username;
        std::string IpAddress;
        int SlotId = -1;
        int Score = 0;
        std::uint64_t SteamId = 0;
        std::uint64_t ConnectionGeneration = 0;
        std::uint64_t IpRevision = 0;
        std::int64_t ConnectedAtUnixSeconds = 0;
        bool GuidOnlyEvaluationQueued = false;
        std::string LastRulesEvaluationIp;
    };

    struct PlaytimePlayerState
    {
        std::string PlayerGuid;
        std::string Username;
        bool IdentityResolved = false;
    };

    struct ServerOriginatedBan
    {
        std::string PlayerGuid;
        std::string PlayerName;
        std::string AdminSteamId;
        std::string Reason;
        std::int64_t ExpireUnixSeconds = -1; // -1 => permanent (Never)
    };

    struct PortalBan
    {
        std::string Message;
        std::optional<std::int64_t> ExpiresUnixTicks;
        // Keep each Steam association owned by its player-ID ban, with its own expiry.
        std::unordered_map<std::uint64_t, std::optional<std::int64_t>> SteamIdExpiries;

        void Merge(const PortalBan& other)
        {
            for (const auto& [steamId, expiry] : other.SteamIdExpiries)
            {
                const auto [it, inserted] = SteamIdExpiries.emplace(steamId, expiry);
                if (!inserted && it->second && (!expiry || *expiry > *it->second))
                {
                    it->second = expiry;
                }
            }
            if (ExpiresUnixTicks &&
                (!other.ExpiresUnixTicks || *other.ExpiresUnixTicks > *ExpiresUnixTicks))
            {
                Message = other.Message;
                ExpiresUnixTicks = other.ExpiresUnixTicks;
            }
        }
    };

    struct ActiveBanPage
    {
        std::unordered_map<std::string, PortalBan> Bans;
        std::size_t ItemCount = 0;
    };

    struct PendingRulesEvaluation
    {
        std::string PlayerGuid;
        std::string ConnectionId;
        std::string Username;
        std::string IpAddress;
        int SlotId = -1;
        std::uint64_t ConnectionGeneration = 0;
        std::uint64_t IpRevision = 0;
        std::string TriggerId;
        std::string ObservationId;
        std::string TriggerPayloadJson;
        std::int64_t OccurredUnixSeconds = 0;
        std::vector<std::string> ExecutionIds;
        std::int64_t NextPollUnixSeconds = 0;
    };

    struct RulesOutcome
    {
        std::string CorrelationKey;
        std::string PayloadJson;
        PendingRulesEvaluation Target;
    };

    std::string configPath;
    std::string agentNameConfigPath;
    std::function<std::int64_t()> utcClock;
    std::function<std::int64_t()> utcTicksClock;
    std::string pluginVersion = "0.0.0-unknown";
    std::string chatPrefix = std::string(kDefaultBotPrefix);
    std::string fallbackChatPrefix = std::string(kDefaultBotPrefix);
    std::string lastHandledCommandToken;
    int lastHandledCommandSlot = -1;
    std::int64_t lastHandledCommandUnixSeconds = 0;
    bool lastHandledCommandFromChat = false;
    std::optional<PluginConfig> loadedConfig;
    EffectiveServerContext serverContext;
    std::int64_t lastTickUnixSeconds = 0;
    std::int64_t nextConfigLoadAttemptUnixSeconds = 0;
    std::string lastConfigLoadError;
    std::int64_t nextChatPrefixRefreshUnixSeconds = 0;
    std::string lastChatPrefixConfigError;

    IngestStage ingestStage = IngestStage::Idle;
    HttpRequestHandle ingestRequest = nullptr;
    std::int64_t ingestRequestStartedUnixSeconds = 0;
    std::int64_t nextIngestAttemptUnixSeconds = 0;
    std::int64_t nextServerStatusUnixSeconds = 0;
    std::size_t ingestConsecutiveFailureCount = 0;
    bool ingestConfigWarningLogged = false;
    std::string ingestBatchQueueName;
    std::string ingestBatchPayload;
    std::vector<std::string> ingestBatchMessageIds;
    std::deque<BufferedEvent> bufferedEvents;
    std::unordered_map<int, ConnectedPlayerState> connectedPlayers;
    std::unordered_map<int, ConnectedPlayerState> playersAtLevelExit;
    std::uint64_t nextConnectionGeneration = 0;
    std::string producerEpoch;
    std::int64_t producerStartedUnixSeconds = 0;
    std::uint64_t droppedPlaytimeCapacity = 0;
    std::uint64_t droppedPlaytimeAttempts = 0;
    std::uint64_t droppedPlaytimeAge = 0;
    std::unordered_map<int, PlaytimePlayerState> playtimePlayersBySlot;

    // Player GUIDs for which a player-connected event has already been emitted this session.
    // Keyed by GUID (not slot) so it survives the map-rotation boundary (OnExitLevel clears the
    // per-slot map); reset only on genuine disconnect and pruned on level exit.
    std::unordered_set<std::string> connectEmittedGuids;

    HttpRequestHandle rulesEvaluationRequest = nullptr;
    std::int64_t rulesEvaluationRequestStartedUnixSeconds = 0;
    std::deque<PendingRulesEvaluation> pendingRulesEvaluations;
    std::optional<PendingRulesEvaluation> activeRulesEvaluation;
    bool preferRulesPoll = false;
    NativeActionExecutor nativeActionExecutor;
    std::deque<PendingRulesEvaluation> pendingRulesPolls;
    std::deque<RulesOutcome> pendingRulesOutcomes;
    HttpRequestHandle rulesOutcomeRequest = nullptr;
    std::int64_t rulesOutcomeRequestStartedUnixSeconds = 0;
    std::int64_t nextRulesOutcomeAttemptUnixSeconds = 0;

    BanSyncStage banSyncStage = BanSyncStage::Idle;
    HttpRequestHandle banSyncRequest = nullptr;
    std::int64_t banSyncRequestStartedUnixSeconds = 0;
    std::atomic<std::int64_t> nextBanSyncUnixSeconds = 0;
    std::size_t banSyncConsecutiveFailureCount = 0;
    LogLevel logLevel = LogLevel::Info;
    int activeBanFetchSkipEntries = 0;
    bool repositoryConfigWarningLogged = false;
    std::unordered_map<std::string, PortalBan> pendingActiveBanMessagesByPlayerGuid;
    std::unordered_map<std::string, PortalBan> activeBanMessagesByPlayerGuid;
    std::unordered_map<int, std::pair<std::uint64_t, std::uint64_t>> observedBanIdentitiesBySlot;
    // Bans created directly on the server (native permban/tempban, observed via OnPlayerAddBan).
    // Held durably here — separate from the portal-synced cache, which is replaced wholesale on
    // each sync — so they survive until the agent imports them into the portal (confirmed when the
    // guid appears in a subsequent active-bans response).
    std::unordered_map<std::string, ServerOriginatedBan> serverOriginatedBansByPlayerGuid;
    mutable std::mutex activeBanCacheMutex;
    mutable std::atomic<std::uint64_t> banStatusCheckCount = 0;
    mutable std::atomic<std::uint64_t> banStatusHitCount = 0;
    mutable std::atomic<std::uint64_t> banStatusMissCount = 0;
    mutable std::atomic<std::uint64_t> banStatusZeroPlayerIdCount = 0;
    mutable std::atomic<std::uint64_t> authenticatedBanCheckCount = 0;
    mutable std::atomic<std::uint64_t> authenticatedBanHitCount = 0;
    mutable std::atomic<std::uint64_t> authenticatedBanMissCount = 0;
    mutable std::atomic<std::uint64_t> authenticatedBanZeroPlayerIdCount = 0;
    std::atomic<std::uint64_t> proactiveBanDropAttemptCount = 0;

    bool IsIngestConfigured() const;
    void AdvanceIngest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool StartIngestBatchRequest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    std::string BuildIngestRequestContext(std::int64_t nowUnixSeconds) const;
    void AbortIngest(ICod4xHost& host, std::int64_t nowUnixSeconds, std::string_view reason);
    void FlushServerStatusSnapshot(ICod4xHost& host, std::int64_t nowUnixSeconds);

    void QueueRulesEvaluation(ICod4xHost& host, ConnectedPlayerState& playerState,
        std::string_view observationId = {});
    void AdvanceRulesEvaluation(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool StartRulesEvaluationRequest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool CompleteRulesEvaluation(ICod4xHost& host, const HttpResponse& response,
        const PendingRulesEvaluation& target);
    void AdvanceRulesOutcomes(ICod4xHost& host, std::int64_t nowUnixSeconds);
    void ResetRulesEvaluation(ICod4xHost& host);
    std::string BuildRulesEvaluationPayload(const PendingRulesEvaluation& evaluation) const;
    bool IsCurrentRulesTarget(ICod4xHost& host, const PendingRulesEvaluation& target) const;
    std::string BuildRulesRuntimeCapabilities() const;
    void RetainRulesPoll(ICod4xHost& host, PendingRulesEvaluation poll);
    void RetainRulesTrigger(ICod4xHost& host, PendingRulesEvaluation trigger);
    std::string BuildRulesSnapshotMetadata(std::int64_t nowUnixSeconds,
        const std::string& messageId, long long sequenceId) const;

    bool IsRepositoryConfigured() const;
    void AdvanceBanSync(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool StartActiveBanFetchRequest(ICod4xHost& host, std::int64_t nowUnixSeconds, int skipEntries);
    void AbortBanSync(ICod4xHost& host, std::int64_t nowUnixSeconds, std::string_view reason);
    void EnforceCachedBansForConnectedPlayers(ICod4xHost& host);
    void EnforceCachedBansForChangedIdentities(ICod4xHost& host);
    bool EnforceCachedBanForPlayer(ICod4xHost& host, int slot);
    bool TryFindPlayerBanMessage(std::uint64_t playerId, std::string& message, std::uint64_t steamId = 0) const;
    // Normalises a ban player id to the canonical PUID the portal keys bans on. cod4x's ban hooks
    // (OnPlayerAddBan/RemoveBan) can deliver the legacy short id, whereas the portal and the agent
    // reconcile use the canonical PUID; resolve via the connected roster so the two agree.
    [[nodiscard]] std::string ResolveBanPlayerGuid(std::uint64_t playerId) const;
    std::optional<ActiveBanPage> ParseActiveBans(const std::string& responseBody) const;
    static std::optional<std::int64_t> ParseUtcTimestamp(const std::string& value, bool allowUnspecifiedUtc = false);

    std::string BuildPlayerConnectedPayload(
        std::int64_t nowUnixSeconds,
        const std::string& messageId,
        const std::string& playerGuid,
        const std::string& connectionId,
        std::uint64_t steamId,
        const std::string& username,
        const std::string& ipAddress,
        int slotId);
    std::string BuildPlayerDisconnectedPayload(
        std::int64_t nowUnixSeconds,
        const std::string& messageId,
        const std::string& playerGuid,
        const std::string& connectionId,
        const std::string& username,
        int slotId,
        std::uint64_t connectionGeneration);
    std::string BuildPlayerIpResolvedPayload(
        std::int64_t nowUnixSeconds,
        const std::string& messageId,
        const ConnectedPlayerState& playerState);
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
    void DropBufferedEventsById(const std::vector<std::string>& messageIds);
    std::string BuildBaseEventPrefix(std::int64_t nowUnixSeconds, const std::string& messageId, long long sequenceId) const;
    std::string BuildPlaytimeMetadata(std::int64_t nowUnixSeconds, const std::string& messageId, long long sequenceId, int timestampQuality, int rosterCompleteness) const;
    void BufferPlaytimeObservation(std::int64_t nowUnixSeconds, int kind, const std::string& fields);
    long long NextSequenceId();
    static std::string GenerateMessageId();
    static std::string JsonEscape(std::string_view value);
    static std::string ToIso8601Utc(std::int64_t unixSeconds);
    static std::string ToIso8601UtcPrecise(std::int64_t unixTicks);
    static std::string StampPublishedUtc(std::string payloadJson, std::int64_t nowUnixSeconds);
    static std::string QueueEndpointPath(std::string_view ingestBaseUrl, std::string_view queueName);
    static std::string NormalizeIpAddress(std::string ipAddress);
    static std::string Trim(std::string value);
    std::string BuildPrefixedChatMessage(std::string_view message) const;
    void UpdateChatPrefix(ICod4xHost& host, std::string_view configJson, bool isRuntimeConfig);
    void RefreshChatPrefix(ICod4xHost& host, std::int64_t nowUnixSeconds);
    void LogChatPrefixConfigIssue(ICod4xHost& host, std::string_view issue);
    bool ShouldLog(LogLevel level) const;
    void LogDebug(ICod4xHost& host, std::string_view message) const;
    void LogInfo(ICod4xHost& host, std::string_view message) const;
    void LogError(ICod4xHost& host, std::string_view message) const;
    std::vector<std::string> BuildPortalPluginHealthReportLines(std::int64_t nowUnixSeconds) const;
    static std::string FormatOptionalUnixTimestamp(std::int64_t unixSeconds);
    long long nextSequenceId = 1;

    bool TryLoadConfig(ICod4xHost& host, std::int64_t nowUnixSeconds);
};

std::string BuildOnlineBroadcastMessage(std::string_view prefix, std::string_view version);
int InitializePlugin(ICod4xHost& host, std::string_view version, std::string_view prefix = kDefaultBotPrefix);
void TickPlugin(ICod4xHost& host);
void ShutdownPlugin(ICod4xHost& host);
void NotifyPlayerConnect(ICod4xHost& host, int slot, std::string_view ipAddress);
void NotifyPlayerConnected(ICod4xHost& host, int slot);
void NotifyClientAuthorized(ICod4xHost& host);
void NotifyPlayerDisconnected(ICod4xHost& host, int slot);
void NotifyChatMessage(ICod4xHost& host, int slot, std::string_view message, bool teamMessage);
void NotifyClientCommand(ICod4xHost& host, int slot, std::string_view command);
void NotifyServerSpawned(ICod4xHost& host);
void NotifyServerExited(ICod4xHost& host);
void NotifyPortalPluginHealthCommand(ICod4xHost& host, int slot);
void SendPluginPrivateChat(ICod4xHost& host, int slot, std::string_view message);
bool TrySetPluginLogLevel(ICod4xHost& host, int levelValue, bool announce = true);
bool TrySetPluginLogLevel(ICod4xHost& host, std::string_view levelToken, bool announce = true);
int GetPluginLogLevelValue();
std::string GetPluginLogLevelName();
void NotifyPlayerBanAdded(
    std::uint64_t playerId,
    std::string_view reason,
    std::uint64_t adminSteamId = 0,
    std::string_view playerName = {},
    std::int64_t expireUnixSeconds = -1);
void NotifyPlayerBanRemoved(std::uint64_t playerId);
bool TryGetPlayerBanMessage(std::uint64_t playerId, std::string& message, std::uint64_t steamId = 0);
bool TryGetAuthenticatedPlayerBanMessage(std::uint64_t playerId, std::string& message, std::uint64_t steamId = 0);
std::string RenderServerBanListDump();
std::string RenderPortalBanListDump();
const EffectiveServerContext& GetServerContext();
}
