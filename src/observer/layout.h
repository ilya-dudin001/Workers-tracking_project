#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace monitor::observer {

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    [[nodiscard]] bool intersects(const Rect& other) const noexcept;
};

struct TileLayout {
    Rect bounds;
    Rect content;
    Rect label;
    std::size_t row = 0;
    std::size_t column = 0;
};

class LayoutEngine {
public:
    [[nodiscard]] static std::vector<TileLayout> compute(std::size_t count,
                                                         Rect client);
    [[nodiscard]] static std::optional<std::size_t> hit_test(
        const std::vector<TileLayout>& tiles, int x, int y);
};

}
