#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace portal_cod4x
{
class ICod4xHost;
enum class NativeEffect
{
    Unsupported,
    Drop,
    PublicMessage,
    PrivateMessage
};

struct NativeActionTarget
{
    std::string PlayerGuid;
    std::string ConnectionId;
    int SlotId = -1;
    std::uint64_t Generation = 0;

    bool operator==(const NativeActionTarget&) const = default;
};

// Internal executor input, not a wire contract or a moderation action definition.
struct NativeAction
{
    std::string ExecutionId;
    NativeActionTarget Target;
    NativeEffect Effect = NativeEffect::Unsupported;
    std::string Text;
    std::int64_t ExpiresUnixSeconds = 0;
    bool Enforce = false;

    bool operator==(const NativeAction&) const = default;
};

struct NativeActionResult
{
    std::string Outcome;
    std::string Detail;
    std::optional<std::int64_t> OccurredUnixTicks = std::nullopt;
};

class NativeActionExecutor
{
  public:
    std::optional<NativeActionResult> PreviousResult(const NativeAction& action) const;
    NativeActionResult Execute(ICod4xHost& host, const NativeAction& action, const NativeActionTarget& liveTarget);

  private:
    struct HandledAction
    {
        NativeAction Action;
        NativeActionResult Result;
    };

    static constexpr std::size_t kMaxHandledActions = 1000;
    std::unordered_map<std::string, HandledAction> handledActions;
};
} // namespace portal_cod4x
