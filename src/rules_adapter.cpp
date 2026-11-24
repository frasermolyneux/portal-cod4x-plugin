#include "portal_cod4x/plugin_runtime.h"
#include "rules_json.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>

namespace portal_cod4x
{
namespace
{
constexpr std::int64_t kRequestDeadlineSeconds = 10;
constexpr std::size_t kMaxPendingTriggers = 64;
constexpr std::size_t kMaxPendingOutcomes = 1000;

bool IsUuid(const std::string& value)
{
    static const std::regex pattern("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    return std::regex_match(value, pattern);
}

std::string CanonicalId(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

} // namespace

std::optional<std::int64_t> PluginRuntime::ParseUtcTimestamp(const std::string& value, bool allowUnspecifiedUtc)
{
    if (value.size() < 19 || value.size() > 35)
    {
        return std::nullopt;
    }
    static const std::regex pattern("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]+)?(Z|\\+00:00)?$");
    if (!std::regex_match(value, pattern) ||
        (!allowUnspecifiedUtc && !value.ends_with("Z") && !value.ends_with("+00:00")))
    {
        return std::nullopt;
    }
    std::tm parsed{};
    std::istringstream input(value.substr(0, 19));
    input >> std::get_time(&parsed, "%Y-%m-%dT%H:%M:%S");
    if (input.fail())
    {
        return std::nullopt;
    }
#ifdef _WIN32
    const auto timestamp = _mkgmtime(&parsed);
#else
    const auto timestamp = timegm(&parsed);
#endif
    if (timestamp < 0)
    {
        return std::nullopt;
    }
    std::tm checked{};
#ifdef _WIN32
    if (gmtime_s(&checked, &timestamp) != 0)
    {
        return std::nullopt;
    }
#else
    if (!gmtime_r(&timestamp, &checked))
    {
        return std::nullopt;
    }
#endif
    std::ostringstream canonical;
    canonical << std::put_time(&checked, "%Y-%m-%dT%H:%M:%S");
    if (canonical.str() != value.substr(0, 19))
    {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(timestamp);
}

namespace
{
std::string Headers(const PluginConfig& config)
{
    return "Ocp-Apim-Subscription-Key: " + config.IngestSubscriptionKey +
           "\r\nAccept: application/json\r\nContent-Type: application/json\r\n";
}
} // namespace

bool PluginRuntime::IsCurrentRulesTarget(ICod4xHost& host, const PendingRulesEvaluation& target) const
{
    const auto player = connectedPlayers.find(target.SlotId);
    return target.SlotId >= 0 && target.SlotId < host.GetSlotCount() && player != connectedPlayers.end() &&
           player->second.PlayerGuid == target.PlayerGuid &&
           player->second.ConnectionGeneration == target.ConnectionGeneration &&
           player->second.ConnectionId == target.ConnectionId && player->second.IpRevision == target.IpRevision &&
           host.GetPlayerId(target.SlotId) != 0 && std::to_string(host.GetPlayerId(target.SlotId)) == target.PlayerGuid;
}

void PluginRuntime::QueueRulesEvaluation(ICod4xHost& host, ConnectedPlayerState& player, std::string_view observationId)
{
    if (player.PlayerGuid.empty() || player.ConnectionId.empty() || player.Username.empty() || player.SlotId < 0 ||
        player.ConnectionGeneration == 0)
    {
        return;
    }
    const bool hasIp = !player.IpAddress.empty();
    if ((hasIp && player.LastRulesEvaluationIp == player.IpAddress) ||
        (!hasIp && (player.ConnectionId.empty() || player.GuidOnlyEvaluationQueued)))
    {
        return;
    }
    if (pendingRulesEvaluations.size() >= kMaxPendingTriggers)
    {
        pendingRulesEvaluations.pop_front();
        LogError(host, "Rules trigger queue full; oldest evaluation skipped");
    }

    PendingRulesEvaluation trigger;
    trigger.PlayerGuid = player.PlayerGuid;
    trigger.ConnectionId = player.ConnectionId;
    trigger.Username = player.Username;
    trigger.IpAddress = player.IpAddress;
    trigger.SlotId = player.SlotId;
    trigger.ConnectionGeneration = player.ConnectionGeneration;
    trigger.IpRevision = player.IpRevision;
    trigger.TriggerId = player.GuidOnlyEvaluationQueued || !player.LastRulesEvaluationIp.empty() ? "player.ip-resolved"
                                                                                                 : "player.connected";
    trigger.ObservationId = observationId.empty() ? GenerateMessageId() : std::string(observationId);
    trigger.OccurredUnixSeconds = host.GetUnixTimeSeconds();
    pendingRulesEvaluations.push_back(std::move(trigger));
    if (hasIp)
    {
        player.LastRulesEvaluationIp = player.IpAddress;
    }
    player.GuidOnlyEvaluationQueued = true;
}

void PluginRuntime::AdvanceRulesEvaluation(ICod4xHost& host, std::int64_t now)
{
    if (!IsIngestConfigured())
    {
        return;
    }
    if (!rulesEvaluationRequest)
    {
        StartRulesEvaluationRequest(host, now);
        return;
    }
    HttpResponse response;
    const auto status = host.PollHttpRequest(rulesEvaluationRequest, response);
    if (status == HttpRequestStatus::Pending &&
        now - rulesEvaluationRequestStartedUnixSeconds < kRequestDeadlineSeconds)
    {
        return;
    }
    host.EndHttpRequest(rulesEvaluationRequest);
    rulesEvaluationRequest = nullptr;
    // Copy before invoking the host: a native drop may re-enter connection callbacks.
    const auto target = *activeRulesEvaluation;
    activeRulesEvaluation.reset();
    const bool completed = status == HttpRequestStatus::Completed && response.StatusCode >= 200 &&
                           response.StatusCode < 300 && CompleteRulesEvaluation(host, response, target);
    if (!completed)
    {
        LogError(host, "Rules request failed or timed out (HTTP " + std::to_string(response.StatusCode) + ")");
        if (!target.ExecutionIds.empty())
        {
            auto retry = target;
            retry.NextPollUnixSeconds = now + 5;
            RetainRulesPoll(host, std::move(retry));
        }
    }
}

bool PluginRuntime::StartRulesEvaluationRequest(ICod4xHost& host, std::int64_t now)
{
    std::erase_if(pendingRulesPolls, [&](const auto& entry) {
        if (now - entry.OccurredUnixSeconds <= 15 * 60)
        {
            return false;
        }
        LogError(host, "Rules continuation expired locally; cloud executions require reconciliation");
        return true;
    });
    auto readyPoll = std::find_if(pendingRulesPolls.begin(), pendingRulesPolls.end(),
                                  [now](const auto& poll) { return poll.NextPollUnixSeconds <= now; });
    const bool poll = readyPoll != pendingRulesPolls.end() && (pendingRulesEvaluations.empty() || preferRulesPoll);
    if (poll)
    {
        activeRulesEvaluation = std::move(*readyPoll);
        pendingRulesPolls.erase(readyPoll);
    }
    else
    {
        while (!pendingRulesEvaluations.empty())
        {
            auto target = std::move(pendingRulesEvaluations.front());
            pendingRulesEvaluations.pop_front();
            if (!IsCurrentRulesTarget(host, target))
            {
                continue;
            }
            activeRulesEvaluation = std::move(target);
            break;
        }
    }
    if (!activeRulesEvaluation)
    {
        return false;
    }
    rulesEvaluationRequest =
        host.BeginHttpRequest(loadedConfig->IngestBaseUrl + (poll ? "/rules/poll" : "/rules/evaluate"), "POST",
                              BuildRulesEvaluationPayload(*activeRulesEvaluation), Headers(*loadedConfig));
    if (!rulesEvaluationRequest)
    {
        LogError(host, "Unable to start rules request");
        if (poll)
        {
            activeRulesEvaluation->NextPollUnixSeconds = now + 5;
            RetainRulesPoll(host, *activeRulesEvaluation);
        }
        activeRulesEvaluation.reset();
        return false;
    }
    rulesEvaluationRequestStartedUnixSeconds = now;
    preferRulesPoll = !poll;
    return true;
}

bool PluginRuntime::CompleteRulesEvaluation(ICod4xHost& host, const HttpResponse& response,
                                            const PendingRulesEvaluation& target)
{
    const auto document = RulesJsonReader::Parse(response.Body);
    const auto* commands = document ? document->Find("commands") : nullptr;
    const auto* continuations = document ? document->Find("executionIds") : nullptr;
    if (!document || document->Integer("protocolVersion") != 1 || !commands ||
        commands->Type != RulesJson::Kind::Array || commands->Items.size() > 64 || !continuations ||
        continuations->Type != RulesJson::Kind::Array || continuations->Items.size() > 64)
    {
        LogError(host, "Invalid rules response; no native effects dispatched");
        return false;
    }

    for (const auto& command : commands->Items)
    {
        const auto executionId = command.String("executionId");
        const auto actionId = command.String("actionId");
        const auto logicalEffectId = command.String("logicalEffectId");
        const auto connectionId = command.String("connectionId");
        const auto serverId = command.String("gameServerId");
        const auto playerGuid = command.String("playerGuid");
        const auto slot = command.Integer("slotId");
        const auto generation = command.Unsigned("connectionGeneration");
        const auto mode = command.String("mode");
        const auto expiryText = command.String("expiresAtUtc");
        const auto* correlation = command.Find("correlation");
        const auto* effect = command.Find("effect");
        if (command.Integer("protocolVersion") != 1 || !executionId || !IsUuid(*executionId) || !actionId ||
            actionId->empty() || actionId->size() > 256 || !logicalEffectId || logicalEffectId->empty() ||
            logicalEffectId->size() > 256 || !serverId || !IsUuid(*serverId) || !correlation ||
            correlation->Type != RulesJson::Kind::Object || correlation->String("actionId") != actionId ||
            !correlation->String("triggerId") || !correlation->String("triggerOccurrenceId") ||
            !correlation->Find("featureId") || !correlation->Find("ruleId") || !correlation->Find("ruleSetRevision") ||
            !effect || effect->Type != RulesJson::Kind::Object)
        {
            LogError(host, "Invalid rules command identity/protocol; command skipped");
            continue;
        }
        const auto correlationKey = CanonicalId(*executionId) + "|" + CanonicalId(*actionId) + "|" + *logicalEffectId +
                                    "|" + correlation->Dump();
        if (std::any_of(pendingRulesOutcomes.begin(), pendingRulesOutcomes.end(),
                        [&](const auto& pending) { return pending.CorrelationKey == correlationKey; }))
        {
            continue;
        }
        if (pendingRulesOutcomes.size() >= kMaxPendingOutcomes)
        {
            LogError(host, "Rules outcome capacity exhausted; command not dispatched");
            continue;
        }

        NativeAction action;
        action.ExecutionId = *logicalEffectId;
        action.Target = {playerGuid.value_or(""), CanonicalId(connectionId.value_or("")),
                         slot && *slot >= 0 && *slot <= std::numeric_limits<int>::max() ? static_cast<int>(*slot) : -1,
                         generation.value_or(0)};
        const auto expiry = expiryText ? ParseUtcTimestamp(*expiryText) : std::nullopt;
        action.ExpiresUnixSeconds = expiry.value_or(0);
        action.Enforce = mode == "Enforce";
        const auto type = effect->String("type");
        if (type == "DropPlayer")
        {
            action.Effect = NativeEffect::Drop;
        }
        else if (type == "PublicMessage")
        {
            action.Effect = NativeEffect::PublicMessage;
        }
        else if (type == "PrivateMessage")
        {
            action.Effect = NativeEffect::PrivateMessage;
        }
        action.Text = effect->String(type == "DropPlayer" ? "reason" : "message").value_or("");

        NativeActionResult result;
        if (!mode || (*mode != "Enforce" && *mode != "DryRun") || !expiry)
        {
            result = {"Failed", "Invalid mode or expiry"};
        }
        else if (!serverId || CanonicalId(*serverId) != CanonicalId(serverContext.GameServerId) || !connectionId ||
                 correlation->String("connectionId") != connectionId || !IsUuid(*connectionId) ||
                 action.Target.PlayerGuid != target.PlayerGuid || action.Target.SlotId != target.SlotId ||
                 action.Target.Generation != target.ConnectionGeneration ||
                 action.Target.ConnectionId != CanonicalId(target.ConnectionId) || !IsCurrentRulesTarget(host, target))
        {
            result = {"Stale", "Command target or request connection no longer matches"};
        }
        else
        {
            const auto player = connectedPlayers.find(action.Target.SlotId);
            NativeActionTarget liveTarget;
            if (player != connectedPlayers.end())
            {
                liveTarget = {player->second.PlayerGuid, CanonicalId(player->second.ConnectionId),
                              player->second.SlotId, player->second.ConnectionGeneration};
            }
            result = nativeActionExecutor.Execute(host, action, liveTarget);
        }
        const auto payload =
            "{\"protocolVersion\":1,\"executionId\":" + RulesJson::Quote(*executionId) +
            ",\"gameServerId\":" + RulesJson::Quote(*serverId) + ",\"actionId\":" + RulesJson::Quote(*actionId) +
            ",\"logicalEffectId\":" + RulesJson::Quote(*logicalEffectId) + ",\"correlation\":" + correlation->Dump() +
            ",\"outcome\":" + RulesJson::Quote(result.Outcome) +
            ",\"occurredAtUtc\":" + RulesJson::Quote(ToIso8601Utc(host.GetUnixTimeSeconds())) +
            ",\"detail\":" + RulesJson::Quote(result.Detail) + "}";
        auto outcomeTarget = target;
        outcomeTarget.ExecutionIds = {*executionId};
        pendingRulesOutcomes.push_back({correlationKey, payload, std::move(outcomeTarget)});
        LogInfo(host, "Rules action " + *executionId + ": " + result.Outcome + " (" + result.Detail + ")");
    }

    auto poll = target;
    poll.ExecutionIds.clear();
    for (const auto& entry : pendingRulesPolls)
    {
        if (entry.ObservationId != target.ObservationId)
        {
            continue;
        }
        for (const auto& execution : entry.ExecutionIds)
        {
            if (std::find(target.ExecutionIds.begin(), target.ExecutionIds.end(), execution) ==
                target.ExecutionIds.end())
            {
                poll.ExecutionIds.push_back(execution);
            }
        }
    }
    for (const auto& execution : continuations->Items)
    {
        if (execution.Type != RulesJson::Kind::String || !IsUuid(execution.Scalar))
        {
            LogError(host, "Invalid rules continuation identity");
            continue;
        }
        if (std::find(poll.ExecutionIds.begin(), poll.ExecutionIds.end(), execution.Scalar) == poll.ExecutionIds.end())
        {
            poll.ExecutionIds.push_back(execution.Scalar);
        }
    }
    std::erase_if(pendingRulesPolls, [&](const auto& entry) { return entry.ObservationId == target.ObservationId; });
    if (!poll.ExecutionIds.empty())
    {
        poll.NextPollUnixSeconds = host.GetUnixTimeSeconds() + 2;
        RetainRulesPoll(host, std::move(poll));
    }
    return true;
}

void PluginRuntime::RetainRulesPoll(ICod4xHost& host, PendingRulesEvaluation poll)
{
    const auto existing = std::find_if(pendingRulesPolls.begin(), pendingRulesPolls.end(),
                                       [&](const auto& entry) { return entry.ObservationId == poll.ObservationId; });
    if (existing != pendingRulesPolls.end())
    {
        for (const auto& execution : existing->ExecutionIds)
        {
            if (std::find(poll.ExecutionIds.begin(), poll.ExecutionIds.end(), execution) == poll.ExecutionIds.end())
            {
                poll.ExecutionIds.push_back(execution);
            }
        }
        pendingRulesPolls.erase(existing);
    }
    if (poll.ExecutionIds.size() > 64)
    {
        LogError(host,
                 "Rules continuation identity capacity exhausted; excess cloud executions require reconciliation");
        poll.ExecutionIds.resize(64);
    }
    if (pendingRulesPolls.size() >= kMaxPendingTriggers)
    {
        LogError(host, "Rules poll capacity exhausted; oldest cloud execution requires reconciliation");
        pendingRulesPolls.pop_front();
    }
    pendingRulesPolls.push_back(std::move(poll));
}

void PluginRuntime::AdvanceRulesOutcomes(ICod4xHost& host, std::int64_t now)
{
    if (!IsIngestConfigured() || pendingRulesOutcomes.empty())
    {
        return;
    }
    if (rulesOutcomeRequest)
    {
        HttpResponse response;
        const auto status = host.PollHttpRequest(rulesOutcomeRequest, response);
        if (status == HttpRequestStatus::Pending &&
            now - rulesOutcomeRequestStartedUnixSeconds < kRequestDeadlineSeconds)
        {
            return;
        }
        host.EndHttpRequest(rulesOutcomeRequest);
        rulesOutcomeRequest = nullptr;
        const auto target = pendingRulesOutcomes.front().Target;
        if (status == HttpRequestStatus::Completed && response.StatusCode >= 200 && response.StatusCode < 300 &&
            CompleteRulesEvaluation(host, response, target))
        {
            pendingRulesOutcomes.pop_front();
        }
        else
        {
            LogError(host, "Rules outcome not acknowledged; retained for retry (HTTP " +
                               std::to_string(response.StatusCode) + ")");
            pendingRulesOutcomes.push_back(std::move(pendingRulesOutcomes.front()));
            pendingRulesOutcomes.pop_front();
            nextRulesOutcomeAttemptUnixSeconds = now + 5;
        }
        return;
    }
    if (now < nextRulesOutcomeAttemptUnixSeconds)
    {
        return;
    }
    rulesOutcomeRequest = host.BeginHttpRequest(loadedConfig->IngestBaseUrl + "/rules/outcomes", "POST",
                                                pendingRulesOutcomes.front().PayloadJson, Headers(*loadedConfig));
    if (!rulesOutcomeRequest)
    {
        LogError(host, "Unable to start rules outcome request; retained for retry");
        nextRulesOutcomeAttemptUnixSeconds = now + 5;
        return;
    }
    rulesOutcomeRequestStartedUnixSeconds = now;
}

void PluginRuntime::ResetRulesEvaluation(ICod4xHost& host)
{
    if (rulesEvaluationRequest)
    {
        host.EndHttpRequest(rulesEvaluationRequest);
    }
    rulesEvaluationRequest = nullptr;
    activeRulesEvaluation.reset();
}

std::string PluginRuntime::BuildRulesRuntimeCapabilities() const
{
    return "{\"id\":\"cod4x-plugin\",\"version\":" + RulesJson::Quote(pluginVersion) +
           ",\"protocolVersion\":1,\"instanceId\":" + RulesJson::Quote(producerEpoch) +
           ",\"startedAtUtc\":" + RulesJson::Quote(ToIso8601Utc(producerStartedUnixSeconds)) +
           ",\"capabilities\":[\"cod4x.player.drop\",\"cod4x.chat.public\",\"cod4x.chat.private\"]}";
}

std::string PluginRuntime::BuildRulesSnapshotMetadata(std::int64_t nowUnixSeconds, const std::string& messageId,
                                                      long long sequenceId) const
{
    return ",\"snapshotId\":" + RulesJson::Quote(messageId) + ",\"sequence\":" + std::to_string(sequenceId) +
           ",\"observedAtUtc\":" + RulesJson::Quote(ToIso8601Utc(nowUnixSeconds)) +
           ",\"runtime\":" + BuildRulesRuntimeCapabilities();
}

std::string PluginRuntime::BuildRulesEvaluationPayload(const PendingRulesEvaluation& trigger) const
{
    std::string payload =
        "{\"protocolVersion\":1,\"gameServerId\":" + RulesJson::Quote(serverContext.GameServerId) +
        ",\"slotId\":" + std::to_string(trigger.SlotId) + ",\"playerGuid\":" + RulesJson::Quote(trigger.PlayerGuid) +
        ",\"connectionId\":" + (trigger.ConnectionId.empty() ? "null" : RulesJson::Quote(trigger.ConnectionId)) +
        ",\"connectionGeneration\":" + std::to_string(trigger.ConnectionGeneration);
    if (!trigger.ExecutionIds.empty())
    {
        payload += ",\"executionIds\":[";
        for (std::size_t i = 0; i < trigger.ExecutionIds.size(); ++i)
        {
            if (i)
            {
                payload += ',';
            }
            payload += RulesJson::Quote(trigger.ExecutionIds[i]);
        }
        return payload + "]}";
    }
    return payload + ",\"triggerId\":" + RulesJson::Quote(trigger.TriggerId) +
           ",\"triggerOccurrenceId\":" + RulesJson::Quote(trigger.ObservationId) +
           ",\"observationId\":" + RulesJson::Quote(trigger.ObservationId) +
           ",\"occurredAtUtc\":" + RulesJson::Quote(ToIso8601Utc(trigger.OccurredUnixSeconds)) +
           ",\"gameType\":\"CallOfDuty4x\"" +
           ",\"playerIpAddress\":" + (trigger.IpAddress.empty() ? "null" : RulesJson::Quote(trigger.IpAddress)) +
           ",\"username\":" + RulesJson::Quote(trigger.Username) + ",\"runtime\":" + BuildRulesRuntimeCapabilities() +
           "}";
}
} // namespace portal_cod4x
