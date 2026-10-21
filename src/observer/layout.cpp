#include "observer/layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace monitor::observer {

bool Rect::intersects(const Rect& other) const noexcept {
    return left < other.right && right > other.left &&
           top < other.bottom && bottom > other.top;
}

std::vector<TileLayout> LayoutEngine::compute(const std::size_t count,
                                               const Rect client) {
    if (count == 0U) {
        return {};
    }
    const int width = std::max(0, client.right - client.left);
    const int height = std::max(0, client.bottom - client.top);
    std::size_t best_columns = 1;
    double best_score = -std::numeric_limits<double>::infinity();
    for (std::size_t columns = 1; columns <= count; ++columns) {
        const std::size_t rows = (count + columns - 1U) / columns;
        const double cell_width = static_cast<double>(width) / columns;
        const double cell_height = static_cast<double>(height) / rows;
        const double label = std::min(32.0, cell_height * 0.25);
        const double image_width =
            std::min(cell_width, std::max(0.0, (cell_height - label) * 16.0 / 9.0));
        const double image_height = image_width * 9.0 / 16.0;
        const double occupancy = static_cast<double>(count) /
                                 static_cast<double>(rows * columns);
        const double score = image_width * image_height *
                             std::pow(occupancy, 4.0);
        if (score > best_score) {
            best_score = score;
            best_columns = columns;
        }
    }
    const std::size_t rows = (count + best_columns - 1U) / best_columns;
    std::vector<TileLayout> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t row = index / best_columns;
        const std::size_t column = index % best_columns;
        const int left = client.left +
            static_cast<int>((static_cast<long long>(width) * column) / best_columns);
        const int right = client.left +
            static_cast<int>((static_cast<long long>(width) * (column + 1U)) / best_columns);
        const int top = client.top +
            static_cast<int>((static_cast<long long>(height) * row) / rows);
        const int bottom = client.top +
            static_cast<int>((static_cast<long long>(height) * (row + 1U)) / rows);
        const int label_height = std::min(32, std::max(0, (bottom - top) / 4));
        result.push_back(TileLayout{
            {left, top, right, bottom},
            {left, top, right, std::max(top, bottom - label_height)},
            {left, std::max(top, bottom - label_height), right, bottom},
            row, column});
    }
    return result;
}

std::optional<std::size_t> LayoutEngine::hit_test(
    const std::vector<TileLayout>& tiles, const int x, const int y) {
    for (std::size_t index = 0; index < tiles.size(); ++index) {
        const Rect& bounds = tiles[index].bounds;
        if (x >= bounds.left && x < bounds.right &&
            y >= bounds.top && y < bounds.bottom) {
            return index;
        }
    }
    return std::nullopt;
}

DpiMetrics DpiMetrics::from_dpi(const unsigned int dpi) noexcept {
    return DpiMetrics(static_cast<float>(dpi == 0U ? 96U : dpi));
}

Rect DpiMetrics::pixels_to_dips(const Rect pixels) const noexcept {
    const auto convert = [this](const int value) {
        return static_cast<int>(std::lround(
            static_cast<double>(value) * 96.0 / static_cast<double>(dpi_)));
    };
    return {convert(pixels.left), convert(pixels.top),
            convert(pixels.right), convert(pixels.bottom)};
}

std::pair<int, int> DpiMetrics::pixel_point_to_dips(
    const int x, const int y) const noexcept {
    const Rect converted = pixels_to_dips({x, y, x, y});
    return {converted.left, converted.top};
}

float DpiMetrics::dpi() const noexcept {
    return dpi_;
}

}
