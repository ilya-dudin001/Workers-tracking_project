#pragma once

#include "common/protocol.h"

#include <cstdint>
#include <map>
#include <optional>

namespace monitor::observer {

struct FrameIdentity {
    protocol::AgentGuid guid{};
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    bool operator==(const FrameIdentity&) const = default;
};

enum class DecodeOutcome { Succeeded, Failed };

class FrameDecodeCache {
public:
    [[nodiscard]] bool should_decode(const FrameIdentity& identity) const;
    void record_attempt(const FrameIdentity& identity, bool succeeded);
    [[nodiscard]] std::optional<DecodeOutcome> outcome(
        const FrameIdentity& identity) const;
    void erase(const protocol::AgentGuid& guid);
    void clear() noexcept;

private:
    struct Record {
        FrameIdentity identity;
        DecodeOutcome outcome;
    };
    std::map<protocol::AgentGuid, Record> records_;
};

}
