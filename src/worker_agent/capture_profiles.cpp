#include "worker_agent/capture_profiles.h"

#include <limits>

namespace monitor::worker_agent {

CaptureProfile grid_profile() noexcept {
    using namespace std::chrono_literals;
    return {480U, std::numeric_limits<std::uint32_t>::max(), 60U, 250ms, 500ms};
}

CaptureProfile focus_profile() noexcept {
    using namespace std::chrono_literals;
    return {1280U, 720U, 75U, 250ms, 250ms};
}

}
