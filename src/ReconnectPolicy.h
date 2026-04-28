#pragma once

#include <algorithm>

namespace mwb {

constexpr int kMinimumReconnectBackoffMs = 100;
constexpr int kMaximumReconnectBackoffMs = 30000;

struct ReconnectPolicy {
    int initialBackoffMs{kMinimumReconnectBackoffMs};
    int maxBackoffMs{kMinimumReconnectBackoffMs};
    int idleRetryMs{kMinimumReconnectBackoffMs};
};

struct ReconnectState {
    int nextBackoffMs{kMinimumReconnectBackoffMs};
    bool useIdleRetry{false};
};

inline int ClampReconnectBackoff(int value) {
    return std::clamp(value, kMinimumReconnectBackoffMs, kMaximumReconnectBackoffMs);
}

inline ReconnectPolicy NormalizeReconnectPolicy(int initialBackoffMs, int maxBackoffMs, int idleRetryMs) {
    ReconnectPolicy policy;
    policy.initialBackoffMs = ClampReconnectBackoff(initialBackoffMs);
    policy.maxBackoffMs = std::max(policy.initialBackoffMs, ClampReconnectBackoff(maxBackoffMs));
    policy.idleRetryMs = std::max(policy.initialBackoffMs, ClampReconnectBackoff(idleRetryMs));
    return policy;
}

inline ReconnectState InitialReconnectState(const ReconnectPolicy& policy) {
    return ReconnectState{policy.initialBackoffMs, false};
}

inline int ScheduledReconnectDelayMs(const ReconnectPolicy& policy, const ReconnectState& state) {
    return state.useIdleRetry ? policy.idleRetryMs : state.nextBackoffMs;
}

inline ReconnectState AdvanceReconnectAfterFailure(const ReconnectPolicy& policy, ReconnectState state) {
    if (state.useIdleRetry) {
        state.nextBackoffMs = policy.initialBackoffMs;
        return state;
    }

    state.nextBackoffMs = std::min(
        policy.maxBackoffMs,
        std::max(state.nextBackoffMs + kMinimumReconnectBackoffMs, state.nextBackoffMs * 2));
    state.useIdleRetry = state.nextBackoffMs >= policy.maxBackoffMs;
    return state;
}

inline ReconnectState ResetReconnectAfterSuccess(const ReconnectPolicy& policy) {
    return InitialReconnectState(policy);
}

} // namespace mwb
