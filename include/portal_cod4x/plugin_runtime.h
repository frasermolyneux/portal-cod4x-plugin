#pragma once

#include <cstdint>
#include <atomic>
#include <deque>
#include <mutex>
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

    explicit PluginRuntime(std::string configPath = std::string(kDefaultConfigFilePath));

    int Initialize(ICod4xHost& host, std::string_view version, std::string_view prefix = kDefaultBotPrefix);
    void Tick(ICod4xHost& host);
    void HandlePlayerConnect(ICod4xHost& host, int slot, std::string_view ipAddress);
    void HandlePlayerConnected(ICod4xHost& host, int slot);
    void HandleClientAuthorized(ICod4xHost& host);
    void HandlePlayerDisconnected(ICod4xHost& host, int slot);
    void HandleChatMessage(ICod4xHost& host, int slot, std::string_view message, bool teamMessage);
    void HandleClientCommand(ICod4xHost& host, int slot, std::string_view command, bool fromChatMessage = false);
    void HandlePortalPluginHealthCommand(ICod4xHost& host, int invokerSlot);
    void HandleServerSpawned(ICod4xHost& host);
    void HandleServerExited(ICod4xHost& host);
    void HandlePlayerBanAdded(
        std::uint64_t playerId,
        std::string_view reason,
        std::uint64_t adminSteamId = 0,
        std::string_view playerName = {},
        std::int64_t expireUnixSeconds = -1);
    void HandlePlayerBanRemoved(std::uint64_t playerId);
    bool TryGetPlayerBanMessage(std::uint64_t playerId, std::string& message) const;
    bool TryGetAuthenticatedPlayerBanMessage(std::uint64_t playerId, std::string& message) const;
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

    enum class VpnEvaluationStage
    {
        Idle,
        Evaluating
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
        std::uint64_t SteamId = 0;
        std::uint64_t ConnectionGeneration = 0;
        std::int64_t ConnectedAtUnixSeconds = 0;
        bool VpnEvaluationQueued = false;
    };

    struct ServerOriginatedBan
    {
        std::string PlayerGuid;
        std::string PlayerName;
        std::string AdminSteamId;
        std::string Reason;
        std::int64_t ExpireUnixSeconds = -1; // -1 => permanent (Never)
    };

    struct PendingVpnEvaluation
    {
        std::string PlayerGuid;
        std::string Username;
        std::string IpAddress;
        int SlotId = -1;
        std::uint64_t ConnectionGeneration = 0;
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
    std::int64_t lastTickUnixSeconds = 0;
    std::int64_t nextConfigLoadAttemptUnixSeconds = 0;
    std::string lastConfigLoadError;

    IngestStage ingestStage = IngestStage::Idle;
    HttpRequestHandle ingestRequest = nullptr;
    std::int64_t ingestRequestStartedUnixSeconds = 0;
    std::int64_t nextIngestAttemptUnixSeconds = 0;
    std::int64_t nextServerStatusUnixSeconds = 0;
    std::size_t ingestConsecutiveFailureCount = 0;
    bool ingestConfigWarningLogged = false;
    std::string ingestBatchQueueName;
    std::string ingestBatchPayload;
    std::vector<std::size_t> ingestBatchIndices;
    std::deque<BufferedEvent> bufferedEvents;
    std::unordered_map<int, ConnectedPlayerState> connectedPlayers;
    std::uint64_t nextConnectionGeneration = 0;

    // Player GUIDs for which a player-connected event has already been emitted this session.
    // Keyed by GUID (not slot) so it survives the map-rotation boundary (OnExitLevel clears the
    // per-slot map); reset only on genuine disconnect and pruned on level exit.
    std::unordered_set<std::string> connectEmittedGuids;

    VpnEvaluationStage vpnEvaluationStage = VpnEvaluationStage::Idle;
    HttpRequestHandle vpnEvaluationRequest = nullptr;
    std::int64_t vpnEvaluationRequestStartedUnixSeconds = 0;
    std::deque<PendingVpnEvaluation> pendingVpnEvaluations;
    std::optional<PendingVpnEvaluation> activeVpnEvaluation;

    BanSyncStage banSyncStage = BanSyncStage::Idle;
    HttpRequestHandle banSyncRequest = nullptr;
    std::int64_t banSyncRequestStartedUnixSeconds = 0;
    std::atomic<std::int64_t> nextBanSyncUnixSeconds = 0;
    std::size_t banSyncConsecutiveFailureCount = 0;
    LogLevel logLevel = LogLevel::Info;
    int activeBanFetchSkipEntries = 0;
    bool repositoryConfigWarningLogged = false;
    std::unordered_map<std::string, std::string> pendingActiveBanMessagesByPlayerGuid;
    std::unordered_map<std::string, std::string> activeBanMessagesByPlayerGuid;
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

    void QueueVpnEvaluation(ConnectedPlayerState& playerState);
    void AdvanceVpnEvaluation(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool StartVpnEvaluationRequest(ICod4xHost& host, std::int64_t nowUnixSeconds);
    void CompleteVpnEvaluation(ICod4xHost& host, const HttpResponse& response);
    void ResetVpnEvaluation(ICod4xHost& host);
    std::string BuildVpnEvaluationPayload(const PendingVpnEvaluation& evaluation) const;

    bool IsRepositoryConfigured() const;
    void AdvanceBanSync(ICod4xHost& host, std::int64_t nowUnixSeconds);
    bool StartActiveBanFetchRequest(ICod4xHost& host, std::int64_t nowUnixSeconds, int skipEntries);
    void AbortBanSync(ICod4xHost& host, std::int64_t nowUnixSeconds, std::string_view reason);
    void EnforceCachedBansForConnectedPlayers(ICod4xHost& host);
    bool TryFindPlayerBanMessage(std::uint64_t playerId, std::string& message) const;
    std::size_t CountActiveBanItems(const std::string& responseBody) const;
    std::unordered_map<std::string, std::string> ParseActiveBanMessagesByPlayerGuid(const std::string& responseBody) const;

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
    static std::string QueueEndpointPath(std::string_view ingestBaseUrl, std::string_view queueName);
    static std::string NormalizeIpAddress(std::string ipAddress);
    static std::string Trim(std::string value);
    std::string BuildPrefixedChatMessage(std::string_view message) const;
    void SendPrivateChat(ICod4xHost& host, int slot, std::string_view message) const;
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
void NotifyPlayerConnect(ICod4xHost& host, int slot, std::string_view ipAddress);
void NotifyPlayerConnected(ICod4xHost& host, int slot);
void NotifyClientAuthorized(ICod4xHost& host);
void NotifyPlayerDisconnected(ICod4xHost& host, int slot);
void NotifyChatMessage(ICod4xHost& host, int slot, std::string_view message, bool teamMessage);
void NotifyClientCommand(ICod4xHost& host, int slot, std::string_view command);
void NotifyServerSpawned(ICod4xHost& host);
void NotifyServerExited(ICod4xHost& host);
void NotifyPortalPluginHealthCommand(ICod4xHost& host, int slot);
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
bool TryGetPlayerBanMessage(std::uint64_t playerId, std::string& message);
bool TryGetAuthenticatedPlayerBanMessage(std::uint64_t playerId, std::string& message);
std::string RenderServerBanListDump();
std::string RenderPortalBanListDump();
const EffectiveServerContext& GetServerContext();
}
