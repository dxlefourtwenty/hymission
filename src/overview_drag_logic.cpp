#include "overview_drag_logic.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hymission::overview_drag {
namespace {

double primaryStart(const Rect &rect, Axis axis) { return axis == Axis::Horizontal ? rect.x : rect.y; }

double primarySize(const Rect &rect, Axis axis) { return axis == Axis::Horizontal ? rect.width : rect.height; }

double secondaryStart(const Rect &rect, Axis axis) { return axis == Axis::Horizontal ? rect.y : rect.x; }

double secondarySize(const Rect &rect, Axis axis) { return axis == Axis::Horizontal ? rect.height : rect.width; }

double pointerPrimary(double x, double y, Axis axis) { return axis == Axis::Horizontal ? x : y; }

double pointerSecondary(double x, double y, Axis axis) { return axis == Axis::Horizontal ? y : x; }

Rect fromAxes(double primary, double secondary, double primaryLength, double secondaryLength, Axis axis) {
    return axis == Axis::Horizontal ? Rect{primary, secondary, primaryLength, secondaryLength} : Rect{secondary, primary, secondaryLength, primaryLength};
}

double clampedHintLength(double preferred, double available) { return std::clamp(preferred, std::min(24.0, available), std::max(24.0, available)); }

} // namespace

std::optional<InsertTarget> insertionTarget(const Rect &workspace, const std::vector<Column> &columns, double pointerX, double pointerY, Axis primaryAxis,
                                            bool reversed, const Rect &draggedPreview) {
    if (workspace.width <= 1.0 || workspace.height <= 1.0)
        return std::nullopt;

    const double pointerP = pointerPrimary(pointerX, pointerY, primaryAxis);
    const double pointerS = pointerSecondary(pointerX, pointerY, primaryAxis);
    const double workspaceP = primaryStart(workspace, primaryAxis);
    const double workspacePLength = primarySize(workspace, primaryAxis);
    const double workspaceS = secondaryStart(workspace, primaryAxis);
    const double workspaceSLength = secondarySize(workspace, primaryAxis);
    const double draggedP = primarySize(draggedPreview, primaryAxis);
    const double draggedS = secondarySize(draggedPreview, primaryAxis);

    if (columns.empty()) {
        const double hintP = clampedHintLength(draggedP, workspacePLength * 0.45);
        const double hintS = std::min(std::max(24.0, draggedS), workspaceSLength * 0.9);
        return InsertTarget{
            .kind = InsertKind::NewColumn,
            .hint = fromAxes(workspaceP + (workspacePLength - hintP) * 0.5, workspaceS + (workspaceSLength - hintS) * 0.5, hintP, hintS, primaryAxis),
        };
    }

    struct Gap {
        double position = 0.0;
        std::size_t insertionIndex = 0;
    };

    std::vector<Gap> columnGaps;
    columnGaps.reserve(columns.size() + 1);
    for (std::size_t visualIndex = 0; visualIndex <= columns.size(); ++visualIndex) {
        double position = 0.0;
        if (visualIndex == 0) {
            position = primaryStart(columns.front().rect, primaryAxis);
        } else if (visualIndex == columns.size()) {
            position = primaryStart(columns.back().rect, primaryAxis) + primarySize(columns.back().rect, primaryAxis);
        } else {
            const auto &previous = columns[visualIndex - 1].rect;
            const auto &next = columns[visualIndex].rect;
            position = (primaryStart(previous, primaryAxis) + primarySize(previous, primaryAxis) + primaryStart(next, primaryAxis)) * 0.5;
        }

        const std::size_t insertionIndex = reversed ? columns.size() - visualIndex : visualIndex;
        columnGaps.push_back({position, insertionIndex});
    }

    const auto nearestColumnGap = std::min_element(columnGaps.begin(), columnGaps.end(), [&](const Gap &lhs, const Gap &rhs) {
        return std::abs(lhs.position - pointerP) < std::abs(rhs.position - pointerP);
    });

    const auto containingColumn = std::min_element(columns.begin(), columns.end(), [&](const Column &lhs, const Column &rhs) {
        const double lhsCenter = primaryStart(lhs.rect, primaryAxis) + primarySize(lhs.rect, primaryAxis) * 0.5;
        const double rhsCenter = primaryStart(rhs.rect, primaryAxis) + primarySize(rhs.rect, primaryAxis) * 0.5;
        return std::abs(lhsCenter - pointerP) < std::abs(rhsCenter - pointerP);
    });

    const double columnDistance = std::abs(nearestColumnGap->position - pointerP);
    double tileDistance = std::numeric_limits<double>::infinity();
    std::size_t tileIndex = 0;
    double tileGapPosition = workspaceS + workspaceSLength * 0.5;
    if (!containingColumn->tiles.empty()) {
        for (std::size_t visualIndex = 0; visualIndex <= containingColumn->tiles.size(); ++visualIndex) {
            double position = 0.0;
            if (visualIndex == 0) {
                position = secondaryStart(containingColumn->tiles.front().rect, primaryAxis);
            } else if (visualIndex == containingColumn->tiles.size()) {
                const auto &last = containingColumn->tiles.back().rect;
                position = secondaryStart(last, primaryAxis) + secondarySize(last, primaryAxis);
            } else {
                const auto &previous = containingColumn->tiles[visualIndex - 1].rect;
                const auto &next = containingColumn->tiles[visualIndex].rect;
                position = (secondaryStart(previous, primaryAxis) + secondarySize(previous, primaryAxis) + secondaryStart(next, primaryAxis)) * 0.5;
            }

            const double distance = std::abs(position - pointerS);
            if (distance < tileDistance) {
                tileDistance = distance;
                tileIndex = visualIndex;
                tileGapPosition = position;
            }
        }
    }

    if (columnDistance <= tileDistance) {
        const double hintP = clampedHintLength(draggedP, workspacePLength * 0.28);
        const double hintS = std::min(std::max(24.0, draggedS), workspaceSLength * 0.9);
        return InsertTarget{
            .kind = InsertKind::NewColumn,
            .column = nearestColumnGap->insertionIndex,
            .hint = fromAxes(nearestColumnGap->position - hintP * 0.5, workspaceS + (workspaceSLength - hintS) * 0.5, hintP, hintS, primaryAxis),
        };
    }

    const double hintP = std::min(primarySize(containingColumn->rect, primaryAxis), workspacePLength);
    const double hintS = clampedHintLength(draggedS, workspaceSLength * 0.22);
    return InsertTarget{
        .kind = InsertKind::InColumn,
        .column = containingColumn->index,
        .tile = tileIndex,
        .hint = fromAxes(primaryStart(containingColumn->rect, primaryAxis), tileGapPosition - hintS * 0.5, hintP, hintS, primaryAxis),
    };
}

double edgeScrollVelocity(const Rect &workspace, double pointerX, double pointerY, Axis primaryAxis, bool reversed, double triggerWidth, double maxSpeed) {
    const double length = primarySize(workspace, primaryAxis);
    const double trigger = std::clamp(triggerWidth, 0.0, length * 0.5);
    if (trigger <= 0.01 || maxSpeed <= 0.0)
        return 0.0;

    const double position = std::clamp(pointerPrimary(pointerX, pointerY, primaryAxis) - primaryStart(workspace, primaryAxis), 0.0, length);
    double normalized = 0.0;
    if (position < trigger)
        normalized = -(trigger - position) / trigger;
    else if (length - position < trigger)
        normalized = (trigger - (length - position)) / trigger;

    return normalized * maxSpeed * (reversed ? -1.0 : 1.0);
}

} // namespace hymission::overview_drag
