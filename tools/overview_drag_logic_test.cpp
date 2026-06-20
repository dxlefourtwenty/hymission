#include "overview_drag_logic.hpp"

#include <cassert>
#include <cmath>

using namespace hymission::overview_drag;

int main() {
    const Rect workspace{0.0, 0.0, 1000.0, 600.0};
    const Rect dragged{0.0, 0.0, 240.0, 220.0};
    const std::vector<Column> columns{
        {.index = 0, .rect = {80.0, 50.0, 300.0, 500.0}, .tiles = {{0, {80.0, 50.0, 300.0, 240.0}}, {1, {80.0, 310.0, 300.0, 240.0}}}},
        {.index = 1, .rect = {420.0, 50.0, 300.0, 500.0}, .tiles = {{0, {420.0, 50.0, 300.0, 500.0}}}},
    };

    const auto newColumn = insertionTarget(workspace, columns, 400.0, 300.0, Axis::Horizontal, false, dragged);
    assert(newColumn && newColumn->kind == InsertKind::NewColumn && newColumn->column == 1);

    const auto inColumn = insertionTarget(workspace, columns, 220.0, 300.0, Axis::Horizontal, false, dragged);
    assert(inColumn && inColumn->kind == InsertKind::InColumn && inColumn->column == 0 && inColumn->tile == 1);

    const auto reversed = insertionTarget(workspace, columns, 40.0, 180.0, Axis::Horizontal, true, dragged);
    assert(reversed && reversed->kind == InsertKind::NewColumn && reversed->column == 2);

    assert(edgeScrollVelocity(workspace, 0.0, 300.0, Axis::Horizontal, false, 30.0, 1500.0) == -1500.0);
    assert(edgeScrollVelocity(workspace, 1000.0, 300.0, Axis::Horizontal, false, 30.0, 1500.0) == 1500.0);
    assert(std::abs(edgeScrollVelocity(workspace, 500.0, 300.0, Axis::Horizontal, false, 30.0, 1500.0)) < 0.001);
}
