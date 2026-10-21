#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace monitor::observer {

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    [[nodiscard]] bool intersects(const Rect& other) const noexcept;
    bool operator==(const Rect&) const = default;
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

class DpiMetrics {
public:
    [[nodiscard]] static DpiMetrics from_dpi(unsigned int dpi) noexcept;
    [[nodiscard]] Rect pixels_to_dips(Rect pixels) const noexcept;
    [[nodiscard]] std::pair<int, int> pixel_point_to_dips(
        int x, int y) const noexcept;
    [[nodiscard]] float dpi() const noexcept;

private:
    explicit DpiMetrics(float dpi) noexcept : dpi_(dpi) {}
    float dpi_ = 96.0F;
};

}
