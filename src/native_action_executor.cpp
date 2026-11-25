#include "portal_cod4x/native_action_executor.h"
#include "portal_cod4x/chat_messages.h"
#include "portal_cod4x/plugin_runtime.h"

#include <algorithm>
#include <utility>

namespace portal_cod4x
{
std::optional<NativeActionResult> NativeActionExecutor::PreviousResult(const NativeAction& action) const
{
    const auto existing = handledActions.find(action.ExecutionId);
    if (existing == handledActions.end())
    {
        return std::nullopt;
    }
    if (existing->second.Action != action)
    {
        return NativeActionResult{"Failed", "Execution identity reused with different command"};
    }
    return existing->second.Result;
}

NativeActionResult NativeActionExecutor::Execute(ICod4xHost& host, const NativeAction& action,
                                                 const NativeActionTarget& liveTarget)
{
    const auto now = host.GetUnixTime100Nanoseconds();
    if (action.ExecutionId.empty())
    {
        return {"Failed", "Missing execution identity"};
    }

    if (const auto previous = PreviousResult(action))
    {
        return *previous;
    }

    if (action.ExpiresUnixTicks <= now)
    {
        return {"Stale", "Command expired"};
    }

    const auto& deliveryTarget = action.RecipientTarget ? *action.RecipientTarget : action.Target;
    NativeActionResult result;
    if (!action.Enforce)
    {
        result = {"WouldExecute", "Native effects are disabled in Dry Run"};
    }
    else if (action.Effect == NativeEffect::Unsupported)
    {
        result = {"SkippedUnsupported", "Native effect is not supported by this plugin"};
    }
    else if (action.RecipientTarget && action.Effect != NativeEffect::PrivateMessage)
    {
        result = {"Failed", "Recipient target is only supported for private messages"};
    }
    else if (deliveryTarget.ConnectionId.empty() || deliveryTarget.PlayerGuid.empty() ||
             deliveryTarget.Generation == 0 || deliveryTarget != liveTarget || deliveryTarget.SlotId < 0 ||
             deliveryTarget.SlotId >= host.GetSlotCount() || host.GetPlayerId(deliveryTarget.SlotId) == 0 ||
             std::to_string(host.GetPlayerId(deliveryTarget.SlotId)) != deliveryTarget.PlayerGuid)
    {
        result = {"Stale", "Connection no longer matches"};
    }
    else if ((action.Effect != NativeEffect::Drop &&
              (action.Text.size() > kChatPayloadBytes || !IsValidChatText(action.Text))) ||
             action.Text.empty() || action.Text.size() > 256 ||
             std::any_of(action.Text.begin(), action.Text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
    {
        result = {"Failed", "Invalid native effect text"};
    }
    else
    {
        result = {"Dispatched", "Native API invoked; effect is not confirmed"};
    }

    if (result.Outcome != "Dispatched")
    {
        return result;
    }

    std::erase_if(handledActions, [now](const auto& entry) { return entry.second.Action.ExpiresUnixTicks <= now; });
    if (handledActions.size() >= kMaxHandledActions)
    {
        // Never evict a live command identity: a repeated response could repeat a native effect.
        return {"Failed", "Native execution deduplication capacity exhausted"};
    }

    // Reserve identity and outcome before host callbacks, which can synchronously disconnect.
    result.OccurredUnixTicks = host.GetUnixTime100Nanoseconds();
    handledActions.emplace(action.ExecutionId, HandledAction{action, result});
    switch (action.Effect)
    {
    case NativeEffect::Drop:
        host.DropPlayer(action.Target.SlotId, action.Text);
        break;
    case NativeEffect::PublicMessage:
        host.BroadcastChat(action.Text);
        break;
    case NativeEffect::PrivateMessage:
        host.SendChat(deliveryTarget.SlotId, action.Text);
        break;
    case NativeEffect::Unsupported:
        break;
    }
    result.OccurredUnixTicks = host.GetUnixTime100Nanoseconds();
    if (const auto handled = handledActions.find(action.ExecutionId); handled != handledActions.end())
    {
        handled->second.Result = result;
    }
    return result;
}
} // namespace portal_cod4x
