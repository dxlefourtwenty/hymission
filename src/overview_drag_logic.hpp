#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace hymission::overview_drag {

enum class Axis {
    Horizontal,
    Vertical,
};

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct Tile {
    std::size_t index = 0;
    Rect rect;
};

struct Column {
    std::size_t index = 0;
    Rect rect;
    std::vector<Tile> tiles;
};

enum class InsertKind {
    NewColumn,
    InColumn,
};

struct InsertTarget {
    InsertKind kind = InsertKind::NewColumn;
    std::size_t column = 0;
    std::size_t tile = 0;
    Rect hint;
};

[[nodiscard]] std::optional<InsertTarget> insertionTarget(const Rect &workspace, const std::vector<Column> &columns, double pointerX, double pointerY,
                                                          Axis primaryAxis, bool reversed, const Rect &draggedPreview);
[[nodiscard]] double edgeScrollVelocity(const Rect &workspace, double pointerX, double pointerY, Axis primaryAxis, bool reversed, double triggerWidth,
                                        double maxSpeed);

} // namespace hymission::overview_drag
