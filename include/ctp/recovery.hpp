#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ctp {

enum class RecoveryPhase : std::uint8_t {
    Idle,
    Disconnected,
    Connecting,
    Authenticating,
    LoggingIn,
    QueryingOrders,
    QueryingTrades,
    QueryingPositions,
    QueryingFunds,
    Reconciling,
    Ready,
    Frozen,
};

enum class RecoveryFailure : std::uint8_t {
    None,
    RequestRejected,
    ResponseError,
    UnexpectedResponse,
    UnknownOrder,
    MissingOrder,
    UnknownTrade,
    InvalidPosition,
    InvalidFunds,
    CapacityExceeded,
    CallbackQueueOverflow,
};

// 与 CTP ErrorMsg 容量一致；输出始终终止并已脱敏。
struct RecoveryDiagnostic {
    RecoveryPhase failed_phase{RecoveryPhase::Idle};
    std::int32_t error_id{0};
    std::array<char, 81> error_message{};
};

inline std::string_view recovery_phase_name(RecoveryPhase value) noexcept
{
    switch (value) {
    case RecoveryPhase::Idle: return "Idle";
    case RecoveryPhase::Disconnected: return "Disconnected";
    case RecoveryPhase::Connecting: return "Connecting";
    case RecoveryPhase::Authenticating: return "Authenticating";
    case RecoveryPhase::LoggingIn: return "LoggingIn";
    case RecoveryPhase::QueryingOrders: return "QueryingOrders";
    case RecoveryPhase::QueryingTrades: return "QueryingTrades";
    case RecoveryPhase::QueryingPositions: return "QueryingPositions";
    case RecoveryPhase::QueryingFunds: return "QueryingFunds";
    case RecoveryPhase::Reconciling: return "Reconciling";
    case RecoveryPhase::Ready: return "Ready";
    case RecoveryPhase::Frozen: return "Frozen";
    }
    return "Invalid";
}

inline std::string_view recovery_failure_name(RecoveryFailure value) noexcept
{
    switch (value) {
    case RecoveryFailure::None: return "None";
    case RecoveryFailure::RequestRejected: return "RequestRejected";
    case RecoveryFailure::ResponseError: return "ResponseError";
    case RecoveryFailure::UnexpectedResponse: return "UnexpectedResponse";
    case RecoveryFailure::UnknownOrder: return "UnknownOrder";
    case RecoveryFailure::MissingOrder: return "MissingOrder";
    case RecoveryFailure::UnknownTrade: return "UnknownTrade";
    case RecoveryFailure::InvalidPosition: return "InvalidPosition";
    case RecoveryFailure::InvalidFunds: return "InvalidFunds";
    case RecoveryFailure::CapacityExceeded: return "CapacityExceeded";
    case RecoveryFailure::CallbackQueueOverflow: return "CallbackQueueOverflow";
    }
    return "Invalid";
}

}
