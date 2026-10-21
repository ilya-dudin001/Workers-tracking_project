#pragma once

#include <memory>
#include <utility>

namespace monitor::observer {

// Transfers ownership to the completion path before initiation. The initiate
// callback may synchronously complete and destroy raw; this function never
// touches raw again when completion_expected is true.
template <typename Operation, typename Register, typename Initiate,
          typename ImmediateFailure>
bool launch_overlapped(std::unique_ptr<Operation> operation,
                       Register&& register_in_flight,
                       Initiate&& initiate,
                       ImmediateFailure&& unregister_immediate_failure) {
    std::forward<Register>(register_in_flight)();
    Operation* const raw = operation.release();
    bool completion_expected = false;
    try {
        completion_expected = std::forward<Initiate>(initiate)(raw);
    } catch (...) {
        std::forward<ImmediateFailure>(unregister_immediate_failure)();
        delete raw;
        throw;
    }
    if (completion_expected) {
        return true;
    }
    std::unique_ptr<Operation> reclaimed(raw);
    std::forward<ImmediateFailure>(unregister_immediate_failure)();
    return false;
}

}
