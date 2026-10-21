#pragma once

#include "common/protocol.h"

#include <filesystem>
#include <map>
#include <string>

namespace monitor::observer {

class NameStore {
public:
    explicit NameStore(std::filesystem::path directory);
    [[nodiscard]] static NameStore local_app_data();
    [[nodiscard]] bool save(const protocol::AgentGuid& guid,
                            const std::string& utf8_name) const;
    [[nodiscard]] std::map<protocol::AgentGuid, std::string> load() const;
    [[nodiscard]] const std::filesystem::path& directory() const noexcept;

private:
    std::filesystem::path directory_;
};

}
