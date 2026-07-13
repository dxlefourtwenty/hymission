#include "overview_drag_logic.hpp"

#include <cmath>
#include <iostream>

using namespace hymission::overview_drag;

int main() {
    const auto expect = [](bool condition, const char* message) {
        if (!condition)
            std::cerr << "FAIL: " << message << '\n';
        return condition;
    };

    bool ok = true;
    const Rect workspace{0.0, 0.0, 1000.0, 600.0};
    const Rect dragged{0.0, 0.0, 240.0, 220.0};
    const std::vector<Column> columns{
        {.index = 0, .rect = {80.0, 50.0, 300.0, 500.0}, .tiles = {{0, {80.0, 50.0, 300.0, 240.0}}, {1, {80.0, 310.0, 300.0, 240.0}}}},
        {.index = 1, .rect = {420.0, 50.0, 300.0, 500.0}, .tiles = {{0, {420.0, 50.0, 300.0, 500.0}}}},
    };

    const auto newColumn = insertionTarget(workspace, columns, 400.0, 300.0, Axis::Horizontal, false, dragged);
    ok &= expect(newColumn && newColumn->kind == InsertKind::NewColumn && newColumn->column == 1, "middle gap should create the second column");

    const auto inColumn = insertionTarget(workspace, columns, 220.0, 300.0, Axis::Horizontal, false, dragged);
    ok &= expect(inColumn && inColumn->kind == InsertKind::InColumn && inColumn->column == 0 && inColumn->tile == 1,
                 "vertical gap should insert between tiles in the first column");

    const auto reversed = insertionTarget(workspace, columns, 40.0, 180.0, Axis::Horizontal, true, dragged);
    ok &= expect(reversed && reversed->kind == InsertKind::NewColumn && reversed->column == 2, "reversed layouts should invert the leading column gap");

    ok &= expect(edgeScrollVelocity(workspace, 0.0, 300.0, Axis::Horizontal, false, 30.0, 1500.0) == -1500.0,
                 "leading edge should use maximum negative speed");
    ok &= expect(edgeScrollVelocity(workspace, 15.0, 300.0, Axis::Horizontal, false, 30.0, 1500.0) == -750.0,
                 "edge speed should ramp linearly through the trigger zone");
    ok &= expect(edgeScrollVelocity(workspace, 1000.0, 300.0, Axis::Horizontal, false, 30.0, 1500.0) == 1500.0,
                 "trailing edge should use maximum positive speed");
    ok &= expect(edgeScrollVelocity(workspace, 1000.0, 300.0, Axis::Horizontal, true, 30.0, 1500.0) == -1500.0,
                 "reversed layouts should invert edge scrolling");
    ok &= expect(edgeScrollVelocity(workspace, 500.0, 0.0, Axis::Vertical, false, 50.0, 1500.0) == -1500.0,
                 "the top workspace edge should navigate toward the previous lane");
    ok &= expect(edgeScrollVelocity(workspace, 500.0, 600.0, Axis::Vertical, false, 50.0, 1500.0) == 1500.0,
                 "the bottom workspace edge should navigate toward the next lane");
    ok &= expect(edgeScrollVelocity(workspace, 1000.0, 300.0, Axis::Horizontal, false, 0.0, 1500.0) == 0.0,
                 "zero-width trigger zones should disable edge scrolling");
    ok &= expect(std::abs(edgeScrollVelocity(workspace, 500.0, 300.0, Axis::Horizontal, false, 30.0, 1500.0)) < 0.001,
                 "the workspace center should not edge-scroll");

    return ok ? 0 : 1;
}
