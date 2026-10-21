#include "observer/frame_cache.h"

namespace monitor::observer {

bool FrameDecodeCache::should_decode(const FrameIdentity& identity) const {
    const auto found = records_.find(identity.guid);
    return found == records_.end() || found->second.identity != identity;
}

void FrameDecodeCache::record_attempt(const FrameIdentity& identity,
                                      const bool succeeded) {
    records_.insert_or_assign(
        identity.guid,
        Record{identity, succeeded ? DecodeOutcome::Succeeded
                                   : DecodeOutcome::Failed});
}

std::optional<DecodeOutcome> FrameDecodeCache::outcome(
    const FrameIdentity& identity) const {
    const auto found = records_.find(identity.guid);
    if (found == records_.end() || found->second.identity != identity) {
        return std::nullopt;
    }
    return found->second.outcome;
}

void FrameDecodeCache::erase(const protocol::AgentGuid& guid) {
    records_.erase(guid);
}

void FrameDecodeCache::clear() noexcept {
    records_.clear();
}

}
