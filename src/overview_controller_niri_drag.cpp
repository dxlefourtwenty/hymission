#include "overview_controller.hpp"
#include "overview_controller_niri_scrolling.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#define private public
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#undef private

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>

namespace hymission {
namespace {

long configInt(const char *name, long fallback) {
    const auto value = Config::mgr()->getConfigValue(name);
    if (!value.dataptr || !value.type)
        return fallback;
    if (*value.type == typeid(bool))
        return **reinterpret_cast<bool *const *>(value.dataptr) ? 1L : 0L;
    if (*value.type == typeid(Config::INTEGER))
        return static_cast<long>(**reinterpret_cast<Config::INTEGER *const *>(value.dataptr));
    return fallback;
}

double configFloat(const char *name, double fallback) {
    const auto value = Config::mgr()->getConfigValue(name);
    if (!value.dataptr || !value.type)
        return fallback;
    if (*value.type == typeid(Config::FLOAT))
        return static_cast<double>(**reinterpret_cast<Config::FLOAT *const *>(value.dataptr));
    if (*value.type == typeid(Config::INTEGER))
        return static_cast<double>(**reinterpret_cast<Config::INTEGER *const *>(value.dataptr));
    return fallback;
}

Layout::Tiled::CScrollingAlgorithm *scrollingForWorkspace(const PHLWORKSPACE &workspace) {
    if (!workspace || !workspace->m_space)
        return nullptr;
    const auto algorithm = workspace->m_space->algorithm();
    if (!algorithm || !algorithm->tiledAlgo())
        return nullptr;
    return dynamic_cast<Layout::Tiled::CScrollingAlgorithm *>(algorithm->tiledAlgo().get());
}

overview_drag::Rect dragRect(const Rect &rect) { return {rect.x, rect.y, rect.width, rect.height}; }

Rect overviewRect(const overview_drag::Rect &rect) { return {rect.x, rect.y, rect.width, rect.height}; }

bool contains(const Rect &rect, const Vector2D &point) {
    return point.x >= rect.x && point.x <= rect.x + rect.width && point.y >= rect.y && point.y <= rect.y + rect.height;
}


double rectArea(const Rect &rect) {
    return std::max(0.0, rect.width) * std::max(0.0, rect.height);
}

double rectIntersectionArea(const Rect &lhs, const Rect &rhs) {
    const double x1 = std::max(lhs.x, rhs.x);
    const double y1 = std::max(lhs.y, rhs.y);
    const double x2 = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
    const double y2 = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
    return std::max(0.0, x2 - x1) * std::max(0.0, y2 - y1);
}

template <typename SessionLike, typename TargetLike>
bool directNiriDropReturnsToSourceSlot(const SessionLike &, const std::optional<TargetLike> &, const Vector2D &) {
    // Do not short-circuit same-workspace drops here.  The target resolver omits
    // the dragged window while computing insert gaps, so a broad “source slot”
    // check can mistake real same-workspace moves for cancel/no-op.  Let the
    // apply path perform the exact remove/reinsert; dropping on the true original
    // slot naturally re-adds the target at the same index.
    return false;
}

Rect unionRect(const Rect &lhs, const Rect &rhs) {
    if (lhs.width <= 0.0 || lhs.height <= 0.0)
        return rhs;
    const double x1 = std::min(lhs.x, rhs.x);
    const double y1 = std::min(lhs.y, rhs.y);
    const double x2 = std::max(lhs.x + lhs.width, rhs.x + rhs.width);
    const double y2 = std::max(lhs.y + lhs.height, rhs.y + rhs.height);
    return {x1, y1, x2 - x1, y2 - y1};
}

bool g_directNiriDragEdgeScrollScheduled = false;
bool g_directNiriDragFinishScheduled = false;

bool usableRect(const Rect &rect) {
    return rect.width > 1.0 && rect.height > 1.0;
}

double dragPrimaryStart(const overview_drag::Rect &rect, overview_drag::Axis axis) {
    return axis == overview_drag::Axis::Horizontal ? rect.x : rect.y;
}

double dragPrimarySize(const overview_drag::Rect &rect, overview_drag::Axis axis) {
    return axis == overview_drag::Axis::Horizontal ? rect.width : rect.height;
}

double dragSecondaryStart(const overview_drag::Rect &rect, overview_drag::Axis axis) {
    return axis == overview_drag::Axis::Horizontal ? rect.y : rect.x;
}

double dragSecondarySize(const overview_drag::Rect &rect, overview_drag::Axis axis) {
    return axis == overview_drag::Axis::Horizontal ? rect.height : rect.width;
}

double dragPointerPrimary(const Vector2D &point, overview_drag::Axis axis) {
    return axis == overview_drag::Axis::Horizontal ? point.x : point.y;
}

double dragPointerSecondary(const Vector2D &point, overview_drag::Axis axis) {
    return axis == overview_drag::Axis::Horizontal ? point.y : point.x;
}

double dragPrimaryEnd(const overview_drag::Rect &rect, overview_drag::Axis axis) {
    return dragPrimaryStart(rect, axis) + dragPrimarySize(rect, axis);
}

double dragPrimaryCenter(const overview_drag::Rect &rect, overview_drag::Axis axis) {
    return dragPrimaryStart(rect, axis) + dragPrimarySize(rect, axis) * 0.5;
}

Vector2D dragPointFromAxes(double primary, double secondary, overview_drag::Axis axis) {
    return axis == overview_drag::Axis::Horizontal ? Vector2D{primary, secondary} : Vector2D{secondary, primary};
}

overview_drag::Rect dragRectFromAxes(double primary, double secondary, double primaryLength, double secondaryLength, overview_drag::Axis axis) {
    return axis == overview_drag::Axis::Horizontal ? overview_drag::Rect{primary, secondary, primaryLength, secondaryLength} :
                                                    overview_drag::Rect{secondary, primary, secondaryLength, primaryLength};
}

double clampedDragHintLength(double preferred, double available) {
    return std::clamp(preferred, std::min(24.0, available), std::max(24.0, available));
}

Rect expandedLaneHitRectForDirectDrag(const Rect &lane, overview_drag::Axis axis) {
    if (!usableRect(lane))
        return lane;

    Rect expanded = lane;
    const double primaryExpansion = std::max(48.0, (axis == overview_drag::Axis::Horizontal ? lane.width : lane.height) * 0.75);
    const double secondaryExpansion = 8.0;
    if (axis == overview_drag::Axis::Horizontal) {
        expanded.x -= primaryExpansion;
        expanded.width += primaryExpansion * 2.0;
        expanded.y -= secondaryExpansion;
        expanded.height += secondaryExpansion * 2.0;
    } else {
        expanded.y -= primaryExpansion;
        expanded.height += primaryExpansion * 2.0;
        expanded.x -= secondaryExpansion;
        expanded.width += secondaryExpansion * 2.0;
    }
    return expanded;
}

std::size_t backendColumnIndexForVisualGap(const std::vector<overview_drag::Column> &columns, std::size_t visualIndex) {
    if (columns.empty())
        return 0;

    visualIndex = std::min(visualIndex, columns.size());
    if (visualIndex == 0)
        return columns.front().index;
    if (visualIndex >= columns.size())
        return columns.back().index + 1;
    return columns[visualIndex].index;
}

std::size_t backendTileIndexForVisualGap(const std::vector<overview_drag::Column> &columns, std::size_t backendColumnIndex, std::size_t visualIndex) {
    const auto columnIt = std::find_if(columns.begin(), columns.end(), [&](const overview_drag::Column &column) {
        return column.index == backendColumnIndex;
    });
    if (columnIt == columns.end() || columnIt->tiles.empty())
        return visualIndex;

    visualIndex = std::min(visualIndex, columnIt->tiles.size());
    if (visualIndex == 0)
        return columnIt->tiles.front().index;
    if (visualIndex >= columnIt->tiles.size())
        return columnIt->tiles.back().index + 1;

    return columnIt->tiles[visualIndex].index;
}

std::size_t finalColumnIndexForSameWorkspaceGap(std::size_t backendGapIndex,
                                                std::size_t currentColumnCount,
                                                std::size_t sourceColumnIndex,
                                                bool sourceColumnMovesAsColumn) {
    if (currentColumnCount == 0)
        return 0;

    std::size_t desired = backendGapIndex;

    // The drag target is computed from the preview list with the dragged window
    // omitted.  When the dragged window is already a single-tile column, the
    // backend gap still used the old real column indices.  Normalize the gap to
    // the post-move column order before issuing native movecol commands; otherwise
    // drops between visible columns land one slot too far and often end up at an
    // edge of the strip.
    if (sourceColumnMovesAsColumn && sourceColumnIndex < desired)
        --desired;

    return std::min(desired, currentColumnCount - 1);
}


template <typename ScrollingDataPtr>
bool moveExistingColumnToBackendGap(ScrollingDataPtr &data,
                                    const SP<Layout::Tiled::SColumnData> &column,
                                    std::size_t backendGapIndex) {
    if (!data || !column || data->columns.size() < 2)
        return false;

    const auto sourceIt = std::find(data->columns.begin(), data->columns.end(), column);
    if (sourceIt == data->columns.end())
        return false;

    // backendGapIndex is expressed in the original backend column coordinates,
    // with the dragged column omitted from the visual hit-test.  Convert it to
    // an insertion index in the vector after the source column is erased.
    std::size_t desiredIndex = 0;
    for (std::size_t index = 0; index < data->columns.size(); ++index) {
        if (data->columns[index] == column)
            continue;
        if (index < backendGapIndex)
            ++desiredIndex;
    }

    auto movedColumn = column;
    data->columns.erase(sourceIt);
    desiredIndex = std::min(desiredIndex, data->columns.size());
    data->columns.insert(data->columns.begin() + static_cast<std::ptrdiff_t>(desiredIndex), movedColumn);
    data->recalculate();
    return true;
}


template <typename ScrollingDataPtr>
bool moveExistingColumnToNeighborGap(ScrollingDataPtr &data,
                                     const SP<Layout::Tiled::SColumnData> &column,
                                     const SP<Layout::Tiled::SColumnData> &leftNeighbor,
                                     const SP<Layout::Tiled::SColumnData> &rightNeighbor,
                                     std::size_t fallbackGapIndex) {
    if (!data || !column || data->columns.size() < 2)
        return false;

    const auto sourceIt = std::find(data->columns.begin(), data->columns.end(), column);
    if (sourceIt == data->columns.end())
        return false;

    auto movedColumn = column;
    data->columns.erase(sourceIt);

    std::size_t desiredIndex = data->columns.size();
    if (rightNeighbor) {
        const auto rightIt = std::find(data->columns.begin(), data->columns.end(), rightNeighbor);
        if (rightIt != data->columns.end())
            desiredIndex = static_cast<std::size_t>(std::distance(data->columns.begin(), rightIt));
    }
    if (desiredIndex == data->columns.size() && leftNeighbor) {
        const auto leftIt = std::find(data->columns.begin(), data->columns.end(), leftNeighbor);
        if (leftIt != data->columns.end())
            desiredIndex = static_cast<std::size_t>(std::distance(data->columns.begin(), leftIt)) + 1;
    }
    if (desiredIndex == data->columns.size())
        desiredIndex = std::min(fallbackGapIndex, data->columns.size());

    data->columns.insert(data->columns.begin() + static_cast<std::ptrdiff_t>(desiredIndex), movedColumn);
    data->recalculate();
    return true;
}

std::optional<overview_drag::InsertTarget> sideColumnInsertionTarget(const overview_drag::Rect &workspace,
                                                                    const std::vector<overview_drag::Column> &columns,
                                                                    const Vector2D &pointer, overview_drag::Axis axis,
                                                                    const overview_drag::Rect &draggedPreview) {
    if (columns.empty() || workspace.width <= 1.0 || workspace.height <= 1.0)
        return std::nullopt;

    const double pointerP = dragPointerPrimary(pointer, axis);
    const double pointerS = dragPointerSecondary(pointer, axis);
    const double workspaceS = dragSecondaryStart(workspace, axis);
    const double workspaceSLength = dragSecondarySize(workspace, axis);
    const double workspacePLength = dragPrimarySize(workspace, axis);
    const double draggedP = dragPrimarySize(draggedPreview, axis);
    const double draggedS = dragSecondarySize(draggedPreview, axis);

    const double secondaryLaneMargin = std::clamp(workspaceSLength * 0.18, 36.0, 180.0);
    if (pointerS < workspaceS - secondaryLaneMargin || pointerS > workspaceS + workspaceSLength + secondaryLaneMargin)
        return std::nullopt;

    const auto makeTarget = [&](std::size_t visualGapIndex, double gapPosition) {
        const double hintP = clampedDragHintLength(draggedP, workspacePLength * 0.28);
        const double hintS = std::min(std::max(24.0, draggedS), workspaceSLength * 0.9);
        return overview_drag::InsertTarget{
            .kind = overview_drag::InsertKind::NewColumn,
            .column = visualGapIndex,
            .hint = dragRectFromAxes(gapPosition - hintP * 0.5, workspaceS + (workspaceSLength - hintS) * 0.5, hintP, hintS, axis),
        };
    };

    const double firstStart = dragPrimaryStart(columns.front().rect, axis);
    const double lastEnd = dragPrimaryEnd(columns.back().rect, axis);
    const double edgeTolerance = std::clamp(std::max(16.0, draggedP * 0.22), 16.0, 128.0);
    if (pointerP < firstStart + edgeTolerance * 0.35)
        return makeTarget(0, firstStart);
    if (pointerP > lastEnd - edgeTolerance * 0.35)
        return makeTarget(columns.size(), lastEnd);

    // Real niri move grabs resolve column placement from the moving preview's
    // placement, not only from the raw cursor point.  In Hymission the cursor can
    // stay on the titlebar/edge of a large or partial-width preview while the
    // preview itself is visually between columns.  Treat the inter-column bands as
    // first-class drop zones so partial (<1.0) columns do not fall through to the
    // in-column/tile heuristic or lose the hint entirely.
    for (std::size_t visualIndex = 1; visualIndex < columns.size(); ++visualIndex) {
        const double previousEnd = dragPrimaryEnd(columns[visualIndex - 1].rect, axis);
        const double nextStart = dragPrimaryStart(columns[visualIndex].rect, axis);
        const double gapStart = std::min(previousEnd, nextStart);
        const double gapEnd = std::max(previousEnd, nextStart);
        const double gapCenter = (previousEnd + nextStart) * 0.5;
        const double gapWidth = std::max(0.0, gapEnd - gapStart);
        const double hitBand = std::clamp(std::max(std::max(18.0, draggedP * 0.24), gapWidth * 0.5 + 12.0), 18.0, 128.0);

        if (pointerP >= gapStart - hitBand && pointerP <= gapEnd + hitBand)
            return makeTarget(visualIndex, gapCenter);
    }

    for (std::size_t visualIndex = 0; visualIndex < columns.size(); ++visualIndex) {
        const auto &column = columns[visualIndex];
        const double columnP = dragPrimaryStart(column.rect, axis);
        const double columnPLength = dragPrimarySize(column.rect, axis);
        const double columnS = dragSecondaryStart(column.rect, axis);
        const double columnSLength = dragSecondarySize(column.rect, axis);
        if (columnPLength <= 1.0 || columnSLength <= 1.0)
            continue;

        const double secondaryMargin = std::clamp(workspaceSLength * 0.16, 24.0, 144.0);
        if (pointerS < columnS - secondaryMargin || pointerS > columnS + columnSLength + secondaryMargin)
            continue;

        // Niri's move grab treats the primary axis as the column placement axis:
        // dropping near a column's primary edge creates a neighboring column,
        // while dropping through the middle stacks/splits inside that column.
        // Partial columns expose very narrow visible rects, so keep a minimum
        // side band large enough to hit even when only a sliver is on-screen.
        double sideBand = std::clamp(std::max(columnPLength * 0.34, draggedP * 0.18), 20.0, 112.0);
        sideBand = std::min(sideBand, std::max(20.0, columnPLength * 0.48));
        if (pointerP <= columnP + sideBand)
            return makeTarget(visualIndex, columnP);
        if (pointerP >= columnP + columnPLength - sideBand)
            return makeTarget(visualIndex + 1, columnP + columnPLength);
    }

    return std::nullopt;
}

bool tiledWorkspaceHasWindowOtherThan(const PHLWORKSPACE &workspace, const PHLWINDOW &ignoredWindow) {
    if (!workspace)
        return false;

    for (const auto &candidate : g_pCompositor->m_windows) {
        if (!candidate || candidate == ignoredWindow || !candidate->m_isMapped || candidate->m_fadingOut || candidate->m_pinned || candidate->onSpecialWorkspace() ||
            candidate->m_workspace != workspace)
            continue;

        const auto target = candidate->layoutTarget();
        if (target && !target->floating())
            return true;
    }

    return false;
}

bool scrollingDataHasUsableColumnOtherThan(Layout::Tiled::CScrollingAlgorithm *scrolling, const PHLWINDOW &ignoredWindow) {
    if (!scrolling || !scrolling->m_scrollingData)
        return false;

    for (const auto &column : scrolling->m_scrollingData->columns) {
        if (!column)
            continue;

        for (const auto &targetData : column->targetDatas) {
            const auto target = targetData ? targetData->target.lock() : nullptr;
            const auto window = target ? target->window() : PHLWINDOW{};
            if (window && window != ignoredWindow && window->m_isMapped && !window->m_fadingOut && !window->m_pinned && !target->floating())
                return true;
        }
    }

    return false;
}



template <typename ScrollingDataPtr>
bool eraseColumnIfEmpty(ScrollingDataPtr &data, const SP<Layout::Tiled::SColumnData> &column) {
    if (!data || !column || !column->targetDatas.empty())
        return false;

    const auto it = std::find(data->columns.begin(), data->columns.end(), column);
    if (it == data->columns.end())
        return false;

    data->columns.erase(it);
    return true;
}


template <typename Index>
std::size_t nonNegativeIndex(Index index) {
    if constexpr (std::is_signed_v<Index>)
        return index >= 0 ? static_cast<std::size_t>(index) : 0;
    else
        return static_cast<std::size_t>(index);
}

void focusWindowForDirectNiriNativeMove(const PHLWINDOW &window) {
    if (!window)
        return;

    // Native movewindow/movecol operate on Hyprland's focused window/column,
    // not Hymission's selectedIndex.  The overview can drag an unfocused window,
    // so make the dragged window the native focus only for the drop transaction;
    // otherwise the dispatcher may move the previously focused column or no-op,
    // which is what made side hints land on the wrong side or snap back.
    Desktop::focusState()->rawWindowFocus(window, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    if (window->m_workspace)
        window->m_workspace->m_lastFocusedWindow = window;
}

std::string oppositeDirectionToken(const std::string &direction) {
    if (direction == "l")
        return "r";
    if (direction == "r")
        return "l";
    if (direction == "u")
        return "d";
    if (direction == "d")
        return "u";
    return direction;
}

std::optional<std::size_t> scrollingColumnIndexForWindow(Layout::Tiled::CScrollingAlgorithm *scrolling, const PHLWINDOW &window) {
    if (!scrolling || !scrolling->m_scrollingData || !window)
        return std::nullopt;

    const auto layoutTarget = window->layoutTarget();
    if (!layoutTarget)
        return std::nullopt;

    const auto targetData = scrolling->dataFor(layoutTarget);
    const auto column = targetData && targetData->column ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};
    if (!column)
        return std::nullopt;

    const auto index = scrolling->m_scrollingData->idx(column);
    if constexpr (std::is_signed_v<decltype(index)>) {
        if (index < 0)
            return std::nullopt;
    }

    return nonNegativeIndex(index);
}

bool scrollingWindowColumnIsSingleTile(Layout::Tiled::CScrollingAlgorithm *scrolling, const PHLWINDOW &window) {
    if (!scrolling || !scrolling->m_scrollingData || !window)
        return false;

    const auto layoutTarget = window->layoutTarget();
    if (!layoutTarget)
        return false;

    const auto targetData = scrolling->dataFor(layoutTarget);
    const auto column = targetData && targetData->column ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};
    if (!column)
        return false;

    std::size_t liveTiles = 0;
    for (const auto &candidateData : column->targetDatas) {
        const auto candidateTarget = candidateData ? candidateData->target.lock() : nullptr;
        const auto candidateWindow = candidateTarget ? candidateTarget->window() : PHLWINDOW{};
        if (!candidateWindow || !candidateWindow->m_isMapped || candidateWindow->m_fadingOut || candidateWindow->m_pinned ||
            candidateWindow->onSpecialWorkspace() || candidateWindow->m_workspace != window->m_workspace)
            continue;

        const auto liveTarget = candidateWindow->layoutTarget();
        if (!liveTarget || liveTarget != candidateTarget || liveTarget->floating())
            continue;

        ++liveTiles;
        if (candidateWindow != window)
            return false;
    }

    return liveTiles == 1;
}

template <typename WorkspaceStripEntryLike>
std::optional<Rect> stripWindowPreviewRect(const WorkspaceStripEntryLike &entry, const Rect &stripRect, const PHLWINDOW &window) {
    if (!window || !usableRect(stripRect))
        return std::nullopt;

    const auto targetPreview = std::find_if(entry.windows.begin(), entry.windows.end(), [&](const auto &preview) {
        return preview.window == window && usableRect(preview.naturalGlobal);
    });
    if (targetPreview == entry.windows.end())
        return std::nullopt;

    Rect sourceBounds{};
    bool hasSourceBounds = false;
    for (const auto &preview : entry.windows) {
        if (!preview.window || preview.window->m_pinned || !usableRect(preview.naturalGlobal))
            continue;

        sourceBounds = hasSourceBounds ? unionRect(sourceBounds, preview.naturalGlobal) : preview.naturalGlobal;
        hasSourceBounds = true;
    }

    if (!hasSourceBounds)
        sourceBounds = targetPreview->naturalGlobal;
    if (!usableRect(sourceBounds))
        return std::nullopt;

    const double padding = std::clamp(std::min(stripRect.width, stripRect.height) * 0.045, 2.0, 10.0);
    const Rect inner = {
        stripRect.x + padding,
        stripRect.y + padding,
        std::max(1.0, stripRect.width - padding * 2.0),
        std::max(1.0, stripRect.height - padding * 2.0),
    };

    const double scale = std::min(inner.width / std::max(1.0, sourceBounds.width), inner.height / std::max(1.0, sourceBounds.height));
    if (scale <= 0.0)
        return std::nullopt;

    const Rect &natural = targetPreview->naturalGlobal;
    const double centerX = inner.centerX() + (natural.centerX() - sourceBounds.centerX()) * scale;
    const double centerY = inner.centerY() + (natural.centerY() - sourceBounds.centerY()) * scale;
    return Rect{
        centerX - natural.width * scale * 0.5,
        centerY - natural.height * scale * 0.5,
        std::max(1.0, natural.width * scale),
        std::max(1.0, natural.height * scale),
    };
}

template <typename LayoutTargetPtr>
void addLayoutTargetToColumn(const SP<Layout::Tiled::SColumnData> &column, const LayoutTargetPtr &layoutTarget,
                             std::size_t tileIndex = 0, bool append = false) {
    if (!column || !layoutTarget)
        return;

    if (append) {
        column->add(layoutTarget);
        return;
    }

    const int beforeIndex = static_cast<int>(std::min(tileIndex, column->targetDatas.size())) - 1;
    column->add(layoutTarget, beforeIndex);
}

void restoreDetachedDragSource(const PHLWINDOW &window, const PHLWORKSPACE &workspace, std::size_t sourceColumn, std::size_t sourceTile, float sourceColumnWidth) {
    (void)sourceColumnWidth;
    if (!window || !workspace)
        return;

    auto *scrolling = scrollingForWorkspace(workspace);
    const auto layoutTarget = window->layoutTarget();
    if (!scrolling || !scrolling->m_scrollingData || !layoutTarget || scrolling->dataFor(layoutTarget))
        return;

    auto &data = scrolling->m_scrollingData;
    const std::size_t columnIndex = std::min(sourceColumn, data->columns.size());
    if (columnIndex == data->columns.size()) {
        // Do not create scrolling columns from the plugin during drag recovery.
        // The live Hyprland layout owns column creation; creating one here can
        // reintroduce the same SScrollingData::add crash path as failed drops.
        return;
    } else {
        auto column = data->columns[columnIndex];
        if (!column)
            return;
        const std::size_t tileIndex = std::min(sourceTile, column->targetDatas.size());
        addLayoutTargetToColumn(column, layoutTarget, tileIndex, false);
    }
    data->recalculate();
}

} // namespace

bool OverviewController::canDragWindowInDirectNiriOverview(const PHLWINDOW &window) const {
    return window && window->m_isMapped && !window->m_fadingOut && !window->m_pinned && !window->onSpecialWorkspace() && m_state.phase == Phase::Active &&
           m_state.collectionPolicy.onlyActiveWorkspace && usesDirectNiriScrollingOverview(m_state) && !m_workspaceTransition.active;
}

double OverviewController::nativeWindowDragThreshold() const { return std::max(0L, configInt("binds:drag_threshold", 0)); }

void OverviewController::beginDirectNiriWindowDrag(std::size_t windowIndex, const Vector2D &pointer) {
    if (windowIndex >= m_state.windows.size())
        return;

    const auto &managed = m_state.windows[windowIndex];
    if (!canDragWindowInDirectNiriOverview(managed.window))
        return;

    Rect preview = currentPreviewRect(managed);
    if (!usableRect(preview) || !contains(preview, pointer)) {
        for (const auto &entry : m_state.stripEntries) {
            if (!entry.monitor)
                continue;

            const Rect stripRect = animatedWorkspaceStripRect(currentWorkspaceStripRect(entry), entry.monitor);
            const auto stripPreview = stripWindowPreviewRect(entry, stripRect, managed.window);
            if (!stripPreview || !usableRect(*stripPreview) || !contains(*stripPreview, pointer))
                continue;

            preview = *stripPreview;
            break;
        }
    }
    if (!usableRect(preview))
        return;

    std::size_t sourceColumnIndex = 0;
    std::size_t sourceTileIndex = 0;
    float sourceColumnWidth = 1.0F;
    const auto sourceWorkspace = managed.window->m_workspace;
    const auto layoutTarget = managed.window->layoutTarget();
    if (layoutTarget && !layoutTarget->floating()) {
        if (auto *scrolling = scrollingForWorkspace(sourceWorkspace); scrolling && scrolling->m_scrollingData) {
            if (const auto targetData = scrolling->dataFor(layoutTarget); targetData && targetData->column) {
                const auto sourceColumn = targetData->column.lock();
                const auto columnIndex = scrolling->m_scrollingData->idx(sourceColumn);
                if (sourceColumn && columnIndex >= 0) {
                    sourceColumnIndex = static_cast<std::size_t>(columnIndex);
                    sourceTileIndex = nonNegativeIndex(sourceColumn->idx(layoutTarget));
                    sourceTileIndex = std::min(sourceTileIndex, sourceColumn->targetDatas.size());
                    sourceColumnWidth = sourceColumn->getColumnWidth();
                }
            }
        }
    }

    m_draggedWindowIndex = windowIndex;
    m_niriDragSession = {
        .active = true,
        .window = managed.window,
        .sourceWorkspace = sourceWorkspace,
        .pointerRatio =
            {
                std::clamp((pointer.x - preview.x) / preview.width, 0.0, 1.0),
                std::clamp((pointer.y - preview.y) / preview.height, 0.0, 1.0),
            },
        .previewRect = preview,
        .sourceColumn = sourceColumnIndex,
        .sourceTile = sourceTileIndex,
        .sourceColumnWidth = sourceColumnWidth,
        .detached = false,
        .lastEdgeTick = std::chrono::steady_clock::now(),
    };
    updateDirectNiriWindowDrag(pointer);
}

Rect OverviewController::directNiriDraggedPreviewRect() const {
    if (!m_niriDragSession.active)
        return {};

    const Vector2D pointer = g_pInputManager->getMouseCoordsInternal();
    const Rect &preview = m_niriDragSession.previewRect;
    return {
        pointer.x - preview.width * m_niriDragSession.pointerRatio.x,
        pointer.y - preview.height * m_niriDragSession.pointerRatio.y,
        preview.width,
        preview.height,
    };
}

float OverviewController::directNiriDraggedPreviewAlpha(const PHLWINDOW &window, float fallback) const {
    if (!m_niriDragSession.active || m_niriDragSession.window.lock() != window)
        return fallback;
    return static_cast<float>(std::clamp(configFloat("plugin:hymission:niri_drag_preview_alpha", 0.75), 0.05, 1.0));
}

std::optional<OverviewController::NiriDragTarget> OverviewController::directNiriDragTargetAt(const Vector2D &pointer) const {
    const auto draggedWindow = m_niriDragSession.window.lock();
    if (!draggedWindow)
        return std::nullopt;

    const auto direction = scrollingLayoutDirection();
    const bool horizontal = direction == ScrollingLayoutDirection::Right || direction == ScrollingLayoutDirection::Left;
    const bool reversed = direction == ScrollingLayoutDirection::Left || direction == ScrollingLayoutDirection::Up;
    const auto primaryAxis = horizontal ? overview_drag::Axis::Horizontal : overview_drag::Axis::Vertical;
    const Rect draggedPreview = directNiriDraggedPreviewRect();

    const EmptyWorkspacePlaceholder *lane = nullptr;
    Rect laneRect;
    double laneArea = std::numeric_limits<double>::infinity();
    double lanePreviewOverlap = 0.0;
    bool laneExactHit = false;
    bool lanePointerHit = false;
    for (const auto &placeholder : m_state.emptyWorkspacePlaceholders) {
        if (!placeholder.monitor || placeholder.workspaceId == WORKSPACE_INVALID)
            continue;
        const Rect current = currentEmptyWorkspacePlaceholderRect(placeholder);
        if (!usableRect(current))
            continue;

        const Rect expanded = expandedLaneHitRectForDirectDrag(current, primaryAxis);
        const bool exactHit = contains(current, pointer);
        const bool expandedPointerHit = contains(expanded, pointer);
        const double previewOverlap = rectIntersectionArea(current, draggedPreview);
        const double expandedPreviewOverlap = rectIntersectionArea(expanded, draggedPreview);
        if (!exactHit && !expandedPointerHit && previewOverlap <= 1.0 && expandedPreviewOverlap <= 1.0)
            continue;

        const double area = current.width * current.height;
        const bool previewHit = previewOverlap > 1.0 || expandedPreviewOverlap > 1.0;
        const bool preferExactLane = exactHit && !laneExactHit;
        const bool preferPointerLane = !preferExactLane && expandedPointerHit && !lanePointerHit;
        const bool preferPreviewLane = !preferExactLane && !preferPointerLane && previewHit && !laneExactHit && !lanePointerHit &&
            previewOverlap > lanePreviewOverlap + 1.0;
        const bool preferVisibleEmptyViewport = !preferExactLane && !preferPointerLane && !preferPreviewLane && lane && lane->backingOnly && !placeholder.backingOnly;
        const bool preferSmallerSameKind = !preferExactLane && !preferPointerLane && !preferPreviewLane && (!lane || placeholder.backingOnly == lane->backingOnly) && area < laneArea;
        if (!lane || preferExactLane || preferPointerLane || preferPreviewLane || preferVisibleEmptyViewport || preferSmallerSameKind) {
            lane = &placeholder;
            laneRect = current;
            laneArea = area;
            lanePreviewOverlap = previewOverlap;
            laneExactHit = exactHit;
            lanePointerHit = exactHit || expandedPointerHit;
        }
    }
    if (!lane)
        return std::nullopt;

    auto workspace = lane->workspace ? lane->workspace : g_pCompositor->getWorkspaceByID(lane->workspaceId);
    const bool floating = draggedWindow->m_isFloating || (draggedWindow->layoutTarget() && draggedWindow->layoutTarget()->floating());
    if (floating) {
        return NiriDragTarget{
            .workspace = workspace,
            .monitor = lane->monitor,
            .workspaceId = lane->workspaceId,
            .insertion = {.hint = dragRect(draggedPreview)},
            .floating = true,
        };
    }

    std::vector<overview_drag::Column> columns;
    if (auto *scrolling = scrollingForWorkspace(workspace); scrolling && scrolling->m_scrollingData) {
        std::size_t destinationColumnIndex = 0;
        for (const auto &column : scrolling->m_scrollingData->columns) {
            overview_drag::Column dragColumn{.index = destinationColumnIndex};
            Rect columnRect;
            std::size_t destinationTileIndex = 0;
            for (const auto &targetData : column->targetDatas) {
                const auto target = targetData ? targetData->target.lock() : nullptr;
                const auto window = target ? target->window() : PHLWINDOW{};
                if (!window || window == draggedWindow || !window->m_isMapped || window->m_fadingOut || window->m_pinned ||
                    window->onSpecialWorkspace() || window->m_workspace != workspace)
                    continue;

                const auto liveTarget = window->layoutTarget();
                if (!liveTarget || liveTarget != target || liveTarget->floating())
                    continue;

                const auto *managed = managedWindowFor(m_state, window, true);
                if (!managed)
                    continue;
                const Rect preview = currentPreviewRect(*managed);
                if (!usableRect(preview))
                    continue;
                columnRect = unionRect(columnRect, preview);
                dragColumn.tiles.push_back({destinationTileIndex++, dragRect(preview)});
            }
            if (!dragColumn.tiles.empty()) {
                dragColumn.rect = dragRect(columnRect);
                columns.push_back(std::move(dragColumn));
            }
            ++destinationColumnIndex;
        }
    }

    std::ranges::sort(columns, [&](const overview_drag::Column &lhs, const overview_drag::Column &rhs) {
        const double lhsStart = horizontal ? lhs.rect.x : lhs.rect.y;
        const double rhsStart = horizontal ? rhs.rect.x : rhs.rect.y;
        return lhsStart < rhsStart;
    });
    for (auto &column : columns) {
        std::ranges::sort(column.tiles, [&](const overview_drag::Tile &lhs, const overview_drag::Tile &rhs) {
            return horizontal ? lhs.rect.y < rhs.rect.y : lhs.rect.x < rhs.rect.x;
        });
    }

    const auto laneDragRect = dragRect(laneRect);
    const auto draggedDragRect = dragRect(draggedPreview);
    const double laneS = dragSecondaryStart(laneDragRect, primaryAxis);
    const double laneSLength = dragSecondarySize(laneDragRect, primaryAxis);
    const double effectivePrimary = dragPrimaryCenter(draggedDragRect, primaryAxis);
    const double effectiveSecondary = std::clamp(dragPointerSecondary(pointer, primaryAxis), laneS, laneS + laneSLength);
    const Vector2D effectivePointer = dragPointFromAxes(effectivePrimary, effectiveSecondary, primaryAxis);

    auto insertion = overview_drag::insertionTarget(laneDragRect, columns, effectivePointer.x, effectivePointer.y, primaryAxis, reversed, draggedDragRect);
    if (!insertion)
        insertion = overview_drag::insertionTarget(laneDragRect, columns, pointer.x, pointer.y, primaryAxis, reversed, draggedDragRect);

    auto sideInsertion = sideColumnInsertionTarget(laneDragRect, columns, effectivePointer, primaryAxis, draggedDragRect);
    if (!sideInsertion)
        sideInsertion = sideColumnInsertionTarget(laneDragRect, columns, pointer, primaryAxis, draggedDragRect);
    if (sideInsertion) {
        insertion = sideInsertion;
        insertion->column = backendColumnIndexForVisualGap(columns, insertion->column);
    } else if (!insertion) {
        return std::nullopt;
    } else if (insertion->kind == overview_drag::InsertKind::NewColumn && !columns.empty()) {
        // overview_drag::insertionTarget returns NewColumn indices in the
        // insertion axis's logical direction. Convert back to screen/visual gap
        // space, then to the real SScrollingData backend column index. This
        // especially matters for partial-width columns and skipped/dragged
        // columns, where visual index != backend index.
        std::size_t visualIndex = std::min(insertion->column, columns.size());
        if (reversed)
            visualIndex = columns.size() - visualIndex;

        insertion->column = backendColumnIndexForVisualGap(columns, visualIndex);
    } else if (insertion->kind == overview_drag::InsertKind::InColumn && !columns.empty()) {
        // overview_drag::insertionTarget computes the vertical gap in the
        // sorted preview order.  Niri's scrolling_insert_position returns the
        // layout/backend tile index directly, and add_tile_to_column consumes
        // that same backend index.  Convert the visual gap to the backend index
        // here so between-window hints apply to the exact hinted slot rather
        // than drifting to the top or bottom of the column.
        insertion->tile = backendTileIndexForVisualGap(columns, insertion->column, insertion->tile);
    }

    return NiriDragTarget{
        .workspace = workspace,
        .monitor = lane->monitor,
        .workspaceId = lane->workspaceId,
        .insertion = *insertion,
    };
}

void OverviewController::updateDirectNiriWindowDrag(const Vector2D &pointer) {
    if (!m_niriDragSession.active)
        return;

    m_niriDragSession.target = directNiriDragTargetAt(pointer);

    // Niri's move grab keeps placement as pointer/UI state.  Do not pan the
    // scrolling tape while a window is being positioned in the overview: the
    // slow edge-scroll fights the drop hint, makes off-viewport placement janky,
    // and can move the target out from under the pointer before release.
    m_niriDragSession.edgeEnteredAt = {};
    m_niriDragSession.edgeVelocity = 0.0;
    m_niriDragSession.lastEdgeTick = std::chrono::steady_clock::now();
    damageOwnedMonitors();
}

void OverviewController::tickDirectNiriWindowDragEdgeScroll() {
    if (!m_niriDragSession.active || !m_niriDragSession.target || std::abs(m_niriDragSession.edgeVelocity) <= 0.001)
        return;

    if (insideRenderLifecycle()) {
        if (!g_pEventLoopManager || g_directNiriDragEdgeScrollScheduled)
            return;

        g_directNiriDragEdgeScrollScheduled = true;
        g_pEventLoopManager->doLater([this] {
            g_directNiriDragEdgeScrollScheduled = false;
            if (!m_niriDragSession.active)
                return;
            tickDirectNiriWindowDragEdgeScroll();
        });
        damageOwnedMonitors();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto delay = std::chrono::milliseconds(std::max(0L, configInt("plugin:hymission:niri_drag_edge_scroll_delay_ms", 100)));
    if (m_niriDragSession.edgeEnteredAt == std::chrono::steady_clock::time_point{} || now - m_niriDragSession.edgeEnteredAt < delay) {
        m_niriDragSession.lastEdgeTick = now;
        damageOwnedMonitors();
        return;
    }

    const double elapsed = std::clamp(std::chrono::duration<double>(now - m_niriDragSession.lastEdgeTick).count(), 0.0, 0.05);
    if (elapsed <= 0.0001)
        return;
    m_niriDragSession.lastEdgeTick = now;
    auto workspace = m_niriDragSession.target->workspace;
    if (!workspace)
        return;
    auto *scrolling = scrollingForWorkspace(workspace);
    if (!scrolling || !scrolling->m_scrollingData)
        return;

    // Niri keeps the move drag itself as pointer/UI state and only scrolls the
    // view-offset during the frame tick; it does not rebuild overview cards while
    // the pointer is still moving. Rebuilding Hymission metadata here was causing
    // relayout origins/placeholders to be retargeted every edge-scroll frame,
    // which made workspace wallpaper viewports drift and left ghost previews.
    scrolling->moveTape(static_cast<float>(-m_niriDragSession.edgeVelocity * elapsed));
    refreshWorkspaceLayoutSnapshot(workspace);
    updateDirectNiriWindowDrag(g_pInputManager->getMouseCoordsInternal());
    damageOwnedMonitors();
}

bool OverviewController::applyDirectNiriDragTarget(const PHLWINDOW &window, const NiriDragTarget &target, const PreviewRectSnapshot &previousPreviewRects) {
    if (!window || !target.monitor || target.workspaceId == WORKSPACE_INVALID)
        return false;

    auto workspace = target.workspace ? target.workspace : g_pCompositor->getWorkspaceByID(target.workspaceId);
    const bool createdWorkspaceForDrop = !workspace;
    if (!workspace)
        workspace = g_pCompositor->createNewWorkspace(target.workspaceId, target.monitor->m_id, std::to_string(target.workspaceId), false);
    if (!workspace || workspace->m_isSpecialWorkspace)
        return false;

    const auto sourceWorkspace = window->m_workspace;
    auto *scrollingBeforeMove = scrollingForWorkspace(workspace);
    const bool targetHadTiledContentBeforeMove = tiledWorkspaceHasWindowOtherThan(workspace, window) || scrollingDataHasUsableColumnOtherThan(scrollingBeforeMove, window);
    const bool crossWorkspaceDrop = sourceWorkspace && sourceWorkspace != workspace;
    const bool dropIntoEmptyWorkspace = !target.floating && crossWorkspaceDrop && (createdWorkspaceForDrop || !targetHadTiledContentBeforeMove);

    const auto preservedOwnerWorkspace = m_state.ownerWorkspace;
    const auto preservedOwnerMonitor = m_state.ownerMonitor;
    const auto preservedOwnerActiveWorkspace = preservedOwnerMonitor ? preservedOwnerMonitor->m_activeWorkspace : PHLWORKSPACE{};
    const auto preservedNativeFocus = Desktop::focusState()->window();
    const auto preservedOverviewFocus = selectedWindow() ? selectedWindow() : (m_state.focusDuringOverview ? m_state.focusDuringOverview : preservedNativeFocus);
    const PHLWINDOW preservedSourceLastFocus = sourceWorkspace ? sourceWorkspace->getLastFocusedWindow() : PHLWINDOW{};
    const PHLWINDOW preservedTargetLastFocus = workspace ? workspace->getLastFocusedWindow() : PHLWINDOW{};
    const PHLWINDOW preservedOwnerLastFocus = preservedOwnerWorkspace ? preservedOwnerWorkspace->getLastFocusedWindow() : PHLWINDOW{};
    const bool      dropIntoPreservedEmptyOwner = dropIntoEmptyWorkspace && workspace == preservedOwnerWorkspace;

    const auto validPreservedFocus = [&](const PHLWINDOW &candidate) {
        return candidate && candidate->m_isMapped && !candidate->m_fadingOut && !candidate->m_pinned && !candidate->onSpecialWorkspace() &&
            candidate->m_workspace == preservedOwnerWorkspace;
    };

    const auto validLastFocusForWorkspace = [](const PHLWORKSPACE &workspace, const PHLWINDOW &candidate) -> PHLWINDOW {
        if (!workspace || !candidate || !candidate->m_isMapped || candidate->m_fadingOut || candidate->m_pinned || candidate->onSpecialWorkspace() ||
            candidate->m_workspace != workspace)
            return {};

        return candidate;
    };

    const auto focusFallbackForPreservedOwner = [&]() -> PHLWINDOW {
        if (dropIntoPreservedEmptyOwner && validPreservedFocus(window))
            return window;
        if (validPreservedFocus(preservedOverviewFocus))
            return preservedOverviewFocus;
        if (validPreservedFocus(preservedNativeFocus))
            return preservedNativeFocus;
        if (validPreservedFocus(preservedOwnerLastFocus))
            return preservedOwnerLastFocus;

        for (const auto &managed : m_state.windows) {
            const auto candidate = managed.window;
            if (validPreservedFocus(candidate) && candidate != window)
                return candidate;
        }

        for (const auto &candidate : g_pCompositor->m_windows) {
            if (!validPreservedFocus(candidate) || candidate == window)
                continue;

            const auto layoutTarget = candidate->layoutTarget();
            if (layoutTarget && !layoutTarget->floating())
                return candidate;
        }

        return {};
    };

    const auto restorePreservedOwnerWorkspace = [&]() {
        if (!preservedOwnerMonitor || !preservedOwnerWorkspace)
            return;

        if (preservedOwnerActiveWorkspace == preservedOwnerWorkspace && preservedOwnerMonitor->m_activeWorkspace != preservedOwnerWorkspace) {
            const bool previousGuard = m_applyingWorkspaceTransitionCommit;
            m_applyingWorkspaceTransitionCommit = true;
            preservedOwnerMonitor->changeWorkspace(preservedOwnerWorkspace, true, true, true);
            preservedOwnerWorkspace->m_renderOffset->setValueAndWarp(Vector2D{});
            preservedOwnerWorkspace->m_alpha->setValueAndWarp(1.F);
            m_applyingWorkspaceTransitionCommit = previousGuard;
            m_rebuildVisibleStateAfterWorkspaceTransitionCommit = false;
        }

        m_state.ownerMonitor = preservedOwnerMonitor;
        m_state.ownerWorkspace = preservedOwnerWorkspace;
    };

    const auto restoreFocusForMouseEdit = [&]() -> PHLWINDOW {
        if (sourceWorkspace)
            sourceWorkspace->m_lastFocusedWindow = validLastFocusForWorkspace(sourceWorkspace, preservedSourceLastFocus);
        if (workspace && workspace != sourceWorkspace)
            workspace->m_lastFocusedWindow = validLastFocusForWorkspace(workspace, preservedTargetLastFocus);
        if (preservedOwnerWorkspace)
            preservedOwnerWorkspace->m_lastFocusedWindow = validLastFocusForWorkspace(preservedOwnerWorkspace, preservedOwnerLastFocus);

        restorePreservedOwnerWorkspace();

        const auto focus = focusFallbackForPreservedOwner();
        if (focus) {
            if (Desktop::focusState()->window() != focus)
                Desktop::focusState()->rawWindowFocus(focus, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            if (focus->m_workspace)
                focus->m_workspace->m_lastFocusedWindow = focus;
            selectWindowInState(m_state, focus);
            m_state.focusDuringOverview = focus;
        } else {
            // The preserved focus window can be the dragged window itself.  If it
            // was moved out of the owner workspace and that workspace became
            // empty, restoring that native focus immediately activates the drop
            // workspace again.  Clear overview selection instead and keep the
            // owner workspace as the focused lane/empty placeholder.
            if (Desktop::focusState()->window())
                Desktop::focusState()->rawWindowFocus(PHLWINDOW{}, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            m_state.selectedIndex.reset();
            m_state.focusDuringOverview = {};
        }

        restorePreservedOwnerWorkspace();
        return focus;
    };

    const auto armMouseEditSnapshotRefresh = [&]() {
        if (!workspaceStripEnabled(m_state))
            return;

        // Editing a non-focused strip must not activate that workspace just to
        // make its thumbnail render.  Newly-created / never-visited workspaces
        // often have valid layout targets and borders before their clients have
        // received enough frame feedback to repaint their surfaces.  If we take
        // only the immediate fake-render snapshot, the strip keeps a wallpaper +
        // border-only thumbnail.  Keep surface feedback unblocked for a short
        // run and force delayed strip snapshots so the client contents can land
        // without stealing the focused workspace.
        const std::size_t configuredFrames = static_cast<std::size_t>(std::max(1, stripThemeSurfaceFeedbackFrames()));
        const std::size_t refreshFrames = std::max<std::size_t>(12, std::min<std::size_t>(configuredFrames, 60));
        armThemeSurfaceFeedback(refreshFrames);
        m_stripSnapshotSurfaceFeedbackFrames = std::max(m_stripSnapshotSurfaceFeedbackFrames, refreshFrames);
        m_stripSnapshotsDirty = true;
        scheduleWorkspaceStripSnapshotRefresh();

        if (preservedOwnerMonitor)
            preservedOwnerMonitor->m_forceFullFrames = std::max(preservedOwnerMonitor->m_forceFullFrames, 3);
        if (target.monitor)
            target.monitor->m_forceFullFrames = std::max(target.monitor->m_forceFullFrames, 3);
    };

    const auto workspaceHasMappedSurface = [](const PHLWORKSPACE &candidateWorkspace) {
        if (!candidateWorkspace || candidateWorkspace->m_isSpecialWorkspace)
            return false;

        for (const auto &candidate : g_pCompositor->m_windows) {
            if (!candidate || !candidate->m_isMapped || candidate->m_fadingOut || candidate->m_workspace != candidateWorkspace || candidate->m_pinned ||
                candidate->onSpecialWorkspace())
                continue;

            return true;
        }

        return false;
    };

    const auto armInactiveWorkspaceActivationPulse = [&]() {
        if (!g_pEventLoopManager || !workspaceStripEnabled(m_state) || !isVisible() || !target.monitor || !workspace || workspace->m_isSpecialWorkspace)
            return;

        if (target.monitor->m_activeWorkspace == workspace)
            return;

        if (!workspaceHasMappedSurface(workspace))
            return;

        // Some inactive / never-visited workspaces have valid layout boxes and
        // decorations, but their client buffers do not repaint into a fake strip
        // snapshot until Hyprland briefly treats that workspace as active.  A real
        // workspace switch elsewhere fixes it for the same reason.  Pulse the
        // edited workspace as active under the workspace-change guard, then restore
        // the original owner lane on the next tick.  This feeds the clients and
        // strip snapshot renderer without letting the mouse edit become a logical
        // workspace switch.
        const auto pulseMonitor = target.monitor;
        const auto pulseWorkspace = workspace;
        const auto restoreMonitor = preservedOwnerMonitor ? preservedOwnerMonitor : pulseMonitor;
        const auto restoreWorkspace = preservedOwnerWorkspace ? preservedOwnerWorkspace : preservedOwnerActiveWorkspace;
        const auto restoreFocus = focusFallbackForPreservedOwner();

        const auto forceWorkspaceActive = [&](const PHLMONITOR &monitor, const PHLWORKSPACE &activateWorkspace) {
            if (!monitor || !activateWorkspace || activateWorkspace->m_monitor.lock() != monitor)
                return;

            const bool previousGuard = m_applyingWorkspaceTransitionCommit;
            m_applyingWorkspaceTransitionCommit = true;
            monitor->changeWorkspace(activateWorkspace, true, true, true);
            activateWorkspace->m_renderOffset->setValueAndWarp(Vector2D{});
            activateWorkspace->m_alpha->setValueAndWarp(1.F);
            if (g_layoutManager)
                g_layoutManager->recalculateMonitor(monitor);
            m_applyingWorkspaceTransitionCommit = previousGuard;
            m_rebuildVisibleStateAfterWorkspaceTransitionCommit = false;
        };

        forceWorkspaceActive(pulseMonitor, pulseWorkspace);
        pulseMonitor->m_forceFullFrames = std::max(pulseMonitor->m_forceFullFrames, 3);
        refreshWorkspaceLayoutSnapshot(pulseWorkspace);
        armMouseEditSnapshotRefresh();

        // Capture once immediately while the edited workspace is the compositor's
        // active workspace. The normal scheduled refresh is delayed by surface
        // feedback timing; if we restore first, unvisited clients can still render
        // as wallpaper + borders only.
        m_stripSnapshotsDirty = true;
        m_stripSnapshotSurfaceFeedbackFrames = std::max<std::size_t>(m_stripSnapshotSurfaceFeedbackFrames, 12);
        refreshWorkspaceStripSnapshots();

        g_pEventLoopManager->doLater([this, pulseMonitor, pulseWorkspace, restoreMonitor, restoreWorkspace, restoreFocus] {
            if (!isVisible())
                return;

            if (restoreMonitor && restoreWorkspace)
                m_pendingStripWorkspaceChangeTarget = restoreWorkspace;
            if (restoreMonitor && restoreWorkspace)
                restoreMonitor->m_forceFullFrames = std::max(restoreMonitor->m_forceFullFrames, 3);

            const bool previousGuard = m_applyingWorkspaceTransitionCommit;
            m_applyingWorkspaceTransitionCommit = true;
            if (restoreMonitor && restoreWorkspace && restoreWorkspace->m_monitor.lock() == restoreMonitor) {
                restoreMonitor->changeWorkspace(restoreWorkspace, true, true, true);
                restoreWorkspace->m_renderOffset->setValueAndWarp(Vector2D{});
                restoreWorkspace->m_alpha->setValueAndWarp(1.F);
            }
            m_applyingWorkspaceTransitionCommit = previousGuard;
            m_rebuildVisibleStateAfterWorkspaceTransitionCommit = false;

            if (restoreFocus && restoreFocus->m_isMapped && !restoreFocus->m_fadingOut && restoreFocus->m_workspace == restoreWorkspace) {
                if (Desktop::focusState()->window() != restoreFocus)
                    Desktop::focusState()->rawWindowFocus(restoreFocus, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
                restoreWorkspace->m_lastFocusedWindow = restoreFocus;
                selectWindowInState(m_state, restoreFocus);
                m_state.focusDuringOverview = restoreFocus;
            } else {
                m_state.selectedIndex.reset();
                m_state.focusDuringOverview = {};
            }

            m_state.ownerMonitor = restoreMonitor;
            m_state.ownerWorkspace = restoreWorkspace;
            refreshWorkspaceLayoutSnapshot(pulseWorkspace);
            if (restoreWorkspace && restoreWorkspace != pulseWorkspace)
                refreshWorkspaceLayoutSnapshot(restoreWorkspace);
            m_stripSnapshotsDirty = true;
            m_stripSnapshotSurfaceFeedbackFrames = std::max<std::size_t>(m_stripSnapshotSurfaceFeedbackFrames, 12);
            scheduleWorkspaceStripSnapshotRefresh();
            damageOwnedMonitors();
        });
    };

    const auto refreshAfterMouseEdit = [&](const char *reason) {
        armMouseEditSnapshotRefresh();
        const auto focus = restoreFocusForMouseEdit();
        refreshVisibleStateMetadata(focus, previousPreviewRects.empty() ? nullptr : &previousPreviewRects, reason);
        armMouseEditSnapshotRefresh();
        // Do not briefly activate the drop workspace after a mouse edit.  The
        // old activation pulse was only a workaround for blank inactive-strip
        // snapshots, but direct Niri now renders inactive client contents through
        // the clipped live-surface fallback.  Pulsing the destination workspace
        // active while the overview still owns the source lane makes newly moved
        // clients render once using native/full-workspace geometry, which shows
        // up as oversized or offset client contents after drag/drop.
        damageOwnedMonitors();
        return true;
    };

    const auto abortMouseEdit = [&]() {
        (void)restoreFocusForMouseEdit();
        return false;
    };

    SP<Layout::Tiled::SColumnData> crossWorkspaceInColumnTargetColumn;
    std::size_t crossWorkspaceInColumnTargetColumnIndex = 0;
    std::size_t crossWorkspaceInColumnTargetTile = 0;
    SP<Layout::Tiled::SColumnData> crossWorkspaceNewColumnLeftNeighbor;
    SP<Layout::Tiled::SColumnData> crossWorkspaceNewColumnRightNeighbor;
    std::size_t crossWorkspaceNewColumnTargetGap = 0;
    if (!target.floating && crossWorkspaceDrop && scrollingBeforeMove && scrollingBeforeMove->m_scrollingData) {
        auto &targetDataBeforeMove = scrollingBeforeMove->m_scrollingData;
        if (target.insertion.kind == overview_drag::InsertKind::InColumn) {
            crossWorkspaceInColumnTargetColumnIndex = std::min(target.insertion.column, targetDataBeforeMove->columns.size());
            if (crossWorkspaceInColumnTargetColumnIndex < targetDataBeforeMove->columns.size()) {
                crossWorkspaceInColumnTargetColumn = targetDataBeforeMove->columns[crossWorkspaceInColumnTargetColumnIndex];
                crossWorkspaceInColumnTargetTile = target.insertion.tile;
            }
        } else if (target.insertion.kind == overview_drag::InsertKind::NewColumn) {
            crossWorkspaceNewColumnTargetGap = std::min(target.insertion.column, targetDataBeforeMove->columns.size());
            if (crossWorkspaceNewColumnTargetGap > 0)
                crossWorkspaceNewColumnLeftNeighbor = targetDataBeforeMove->columns[crossWorkspaceNewColumnTargetGap - 1];
            if (crossWorkspaceNewColumnTargetGap < targetDataBeforeMove->columns.size())
                crossWorkspaceNewColumnRightNeighbor = targetDataBeforeMove->columns[crossWorkspaceNewColumnTargetGap];
        }
    }

    if (!target.floating && crossWorkspaceDrop && target.monitor && m_state.collectionPolicy.onlyActiveWorkspace && usesDirectNiriScrollingOverview(m_state)) {
        if (m_workspaceTransition.active)
            commitActiveNiriWorkspaceTransitionForRetarget();

        // A mouse drag is a layout edit, not a workspace activation.  Keep the
        // current strip as the overview owner; only explicit workspace dispatchers
        // or click-without-drag are allowed to move the focused workspace.
    }

    if (sourceWorkspace && sourceWorkspace != workspace) {
        niri_scrolling_detail::retainDirectNiriWorkspaceLaneForDrag(sourceWorkspace->m_monitor.lock(), sourceWorkspace);
        niri_scrolling_detail::retainDirectNiriWorkspaceLaneForDrag(target.monitor, workspace);
    }

    if (sourceWorkspace != workspace) {
        const bool previousGuard = m_applyingWorkspaceTransitionCommit;
        m_applyingWorkspaceTransitionCommit = true;
        g_pCompositor->moveWindowToWorkspaceSafe(window, workspace);
        m_applyingWorkspaceTransitionCommit = previousGuard;
        niri_scrolling_detail::armDirectNiriWorkspaceTransferRenderGuard(window);
        m_rebuildVisibleStateAfterWorkspaceTransitionCommit = false;
    } else if (dropIntoEmptyWorkspace && target.monitor && target.monitor->m_activeWorkspace != workspace) {
        const bool previousGuard = m_applyingWorkspaceTransitionCommit;
        m_applyingWorkspaceTransitionCommit = true;
        workspace->m_lastFocusedWindow = window;
        target.monitor->changeWorkspace(workspace, true, true, true);
        workspace->m_renderOffset->setValueAndWarp(Vector2D{});
        workspace->m_alpha->setValueAndWarp(1.F);
        if (g_layoutManager)
            g_layoutManager->recalculateMonitor(target.monitor);
        m_applyingWorkspaceTransitionCommit = previousGuard;
        m_rebuildVisibleStateAfterWorkspaceTransitionCommit = false;
    }

    bool appliedCrossWorkspaceInColumn = false;
    bool appliedCrossWorkspaceNewColumnPosition = false;
    if (!target.floating && crossWorkspaceDrop && !dropIntoEmptyWorkspace && target.insertion.kind == overview_drag::InsertKind::InColumn) {
        auto *scrolling = scrollingForWorkspace(workspace);
        const auto layoutTarget = window->layoutTarget();
        if (scrolling && scrolling->m_scrollingData && layoutTarget && !layoutTarget->floating()) {
            auto &data = scrolling->m_scrollingData;
            const auto targetData = scrolling->dataFor(layoutTarget);
            const auto sourceColumn = targetData && targetData->column ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};
            if (sourceColumn) {
                const auto sourceTileIndexRaw = sourceColumn->idx(layoutTarget);
                bool sourceTileUsable = true;
                if constexpr (std::is_signed_v<std::remove_cvref_t<decltype(sourceTileIndexRaw)>>)
                    sourceTileUsable = sourceTileIndexRaw >= 0;

                if (sourceTileUsable) {
                    // Cross-workspace in-column drops need the same exact backend
                    // insertion that same-workspace reorders use, but the workspace
                    // transfer must happen first so Hyprland owns column creation.
                    // After the move, remove only the freshly moved target from its
                    // temporary/default destination and insert it into the hinted
                    // existing column.  If Hyprland rebuilt the target column object
                    // while moving the window, fall back to the pre-move backend
                    // column index after the temporary source column has been erased.
                    sourceColumn->remove(layoutTarget);
                    if (sourceColumn != crossWorkspaceInColumnTargetColumn)
                        eraseColumnIfEmpty(data, sourceColumn);

                    SP<Layout::Tiled::SColumnData> destinationColumn;
                    if (crossWorkspaceInColumnTargetColumn && std::find(data->columns.begin(), data->columns.end(), crossWorkspaceInColumnTargetColumn) != data->columns.end())
                        destinationColumn = crossWorkspaceInColumnTargetColumn;
                    else if (crossWorkspaceInColumnTargetColumnIndex < data->columns.size())
                        destinationColumn = data->columns[crossWorkspaceInColumnTargetColumnIndex];

                    if (destinationColumn) {
                        const std::size_t tileIndex = std::min(crossWorkspaceInColumnTargetTile, destinationColumn->targetDatas.size());
                        addLayoutTargetToColumn(destinationColumn, layoutTarget, tileIndex, false);
                        data->recalculate();
                        appliedCrossWorkspaceInColumn = true;
                    }
                }
            }
        }
    }

    if (!target.floating && crossWorkspaceDrop && !dropIntoEmptyWorkspace && target.insertion.kind == overview_drag::InsertKind::NewColumn) {
        auto *scrolling = scrollingForWorkspace(workspace);
        const auto layoutTarget = window->layoutTarget();
        if (scrolling && scrolling->m_scrollingData && layoutTarget && !layoutTarget->floating()) {
            const auto targetData = scrolling->dataFor(layoutTarget);
            const auto movedColumn = targetData && targetData->column ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};

            // The previous cross-workspace path used native movecol after the
            // workspace transfer.  On inactive/overview workspaces that dispatcher
            // can move relative to Hyprland's current scroll viewport/focus rather
            // than the visible drag hint, so between-column drops often landed on
            // either end of the strip.  Treat the hint's destination gap as
            // authoritative: move the freshly-created column pointer between the
            // same neighbor columns that surrounded the hint before the transfer.
            if (movedColumn && scrollingWindowColumnIsSingleTile(scrolling, window) &&
                moveExistingColumnToNeighborGap(scrolling->m_scrollingData,
                                                movedColumn,
                                                crossWorkspaceNewColumnLeftNeighbor,
                                                crossWorkspaceNewColumnRightNeighbor,
                                                crossWorkspaceNewColumnTargetGap)) {
                appliedCrossWorkspaceNewColumnPosition = true;
            }
        }
    }

    if (!target.floating && !dropIntoEmptyWorkspace && !crossWorkspaceDrop) {
        auto *scrolling = scrollingForWorkspace(workspace);
        const auto layoutTarget = window->layoutTarget();
        if (!scrolling || !scrolling->m_scrollingData || !layoutTarget)
            return abortMouseEdit();
        const auto targetData = scrolling->dataFor(layoutTarget);
        const auto sourceColumn = targetData && targetData->column ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};

        // New side-column drops are the path that kept crashing: the coredumps
        // consistently enter Hyprland's SScrollingData::add() from our manual
        // appendColumnAt(). Do not manufacture scrolling columns from the plugin.
        // Route this case through Hyprland's normal movewindow dispatcher, which
        // owns all target/column bookkeeping for stacked and partial-width columns.
        if (target.insertion.kind == overview_drag::InsertKind::NewColumn) {
            if (!sourceColumn)
                return abortMouseEdit();

            auto &data = scrolling->m_scrollingData;
            if (!data)
                return abortMouseEdit();

            const auto sourceColumnIndexRaw = data->idx(sourceColumn);
            if constexpr (std::is_signed_v<decltype(sourceColumnIndexRaw)>) {
                if (sourceColumnIndexRaw < 0)
                    return abortMouseEdit();
            }
            const std::size_t sourceColumnIndex = nonNegativeIndex(sourceColumnIndexRaw);
            const bool sourceColumnWasSingleTile = scrollingWindowColumnIsSingleTile(scrolling, window);

            if (sourceColumnWasSingleTile) {
                // For a window that is already its own column, the operation is
                // just a column reorder.  Do it by moving the existing column SP
                // inside SScrollingData instead of going through native movecol,
                // which can no-op in overview because the live focused/visible
                // column does not always match the drag hint.
                if (!moveExistingColumnToBackendGap(data, sourceColumn, target.insertion.column))
                    return abortMouseEdit();

                refreshWorkspaceLayoutSnapshot(workspace);
                return refreshAfterMouseEdit("drag-drop-direct-column-reorder");
            }

            const std::size_t normalizedTargetColumn = std::min(target.insertion.column, data->columns.size());
            const bool insertBeforeSource = normalizedTargetColumn <= sourceColumnIndex;
            const auto direction = scrollingLayoutDirection();
            const bool horizontal = direction == ScrollingLayoutDirection::Right || direction == ScrollingLayoutDirection::Left;
            const std::string moveDirection = horizontal ? (insertBeforeSource ? "l" : "r") : (insertBeforeSource ? "u" : "d");

            focusWindowForDirectNiriNativeMove(window);

            const auto runNativeWindowMove = [&](const std::string &args) {
                SDispatchResult result{};
                for (const char *dispatcherName : {"movewindow", "movewindoworgroup", "window.move"}) {
                    auto original = m_overviewEditingDispatchersOriginal.find(dispatcherName);
                    if (original == m_overviewEditingDispatchersOriginal.end())
                        continue;

                    result = runOverviewEditingDispatcher(dispatcherName, &original->second, args);
                    if (result.success)
                        break;
                }
                return result;
            };

            const auto runNativeColumnMove = [&](const std::string &args) {
                SDispatchResult result{};
                for (const char *dispatcherName : {"movecol", "movecolumn"}) {
                    auto original = m_overviewEditingDispatchersOriginal.find(dispatcherName);
                    if (original == m_overviewEditingDispatchersOriginal.end())
                        continue;

                    result = runOverviewEditingDispatcher(dispatcherName, &original->second, args);
                    if (result.success)
                        break;
                }
                return result;
            };

            // If the window is already its own column, do not run movewindow first.
            // Native movewindow is a split/stack operation.  For a single-tile
            // column, the desired operation is just movecol to the hinted gap.
            if (!sourceColumnWasSingleTile) {
                bool splitIntoOwnColumn = false;
                const std::string fallbackDirection = oppositeDirectionToken(moveDirection);
                for (const std::string &candidateDirection : {moveDirection, fallbackDirection}) {
                    for (std::size_t attempt = 0; attempt < 4; ++attempt) {
                        focusWindowForDirectNiriNativeMove(window);
                        SDispatchResult result = runNativeWindowMove(candidateDirection);
                        if (!result.success)
                            break;

                        auto *afterScrolling = scrollingForWorkspace(workspace);
                        if (scrollingWindowColumnIsSingleTile(afterScrolling, window)) {
                            splitIntoOwnColumn = true;
                            break;
                        }
                    }
                    if (splitIntoOwnColumn)
                        break;
                }

                // If native Hyprland did not split the window into an independent
                // column, stop instead of leaving it stacked in the wrong place.
                // This keeps the visual hint authoritative: side-column hints must
                // either become side columns or become no-ops, never vertical stacks.
                if (!splitIntoOwnColumn)
                    return abortMouseEdit();
            }

            for (std::size_t attempt = 0; attempt < 24; ++attempt) {
                auto *afterScrolling = scrollingForWorkspace(workspace);
                if (!afterScrolling || !afterScrolling->m_scrollingData || afterScrolling->m_scrollingData->columns.empty())
                    break;

                const auto currentColumnIndex = scrollingColumnIndexForWindow(afterScrolling, window);
                if (!currentColumnIndex)
                    break;

                const std::size_t desiredColumnIndex = sourceColumnWasSingleTile ?
                    finalColumnIndexForSameWorkspaceGap(target.insertion.column, afterScrolling->m_scrollingData->columns.size(), sourceColumnIndex, true) :
                    std::min(target.insertion.column, afterScrolling->m_scrollingData->columns.size() - 1);
                if (*currentColumnIndex == desiredColumnIndex)
                    break;

                const bool moveTowardPrevious = *currentColumnIndex > desiredColumnIndex;
                const std::string columnDirection = horizontal ? (moveTowardPrevious ? "l" : "r") : (moveTowardPrevious ? "u" : "d");
                focusWindowForDirectNiriNativeMove(window);
                const SDispatchResult columnResult = runNativeColumnMove(columnDirection);
                if (!columnResult.success)
                    break;
            }

            if (auto *finalScrolling = scrollingForWorkspace(workspace); finalScrolling && finalScrolling->m_scrollingData && !finalScrolling->m_scrollingData->columns.empty()) {
                const auto finalColumnIndex = scrollingColumnIndexForWindow(finalScrolling, window);
                const std::size_t finalDesiredColumnIndex = sourceColumnWasSingleTile ?
                    finalColumnIndexForSameWorkspaceGap(target.insertion.column, finalScrolling->m_scrollingData->columns.size(), sourceColumnIndex, true) :
                    std::min(target.insertion.column, finalScrolling->m_scrollingData->columns.size() - 1);
                if (!finalColumnIndex || *finalColumnIndex != finalDesiredColumnIndex)
                    return abortMouseEdit();
            }

            refreshWorkspaceLayoutSnapshot(workspace);
            return refreshAfterMouseEdit("drag-drop-native-positioned-movewindow");
        }

        // In-column reordering does not create a new SScrollingData column, so it
        // can keep using the existing column target list.  Keep this narrow and
        // never fall through to data->add() if the layout unexpectedly has no
        // destination column.
        auto &data = scrolling->m_scrollingData;
        if (!data || data->columns.empty() || !sourceColumn)
            return abortMouseEdit();

        const auto sourceColumnIndexRaw = data->idx(sourceColumn);
        if constexpr (std::is_signed_v<decltype(sourceColumnIndexRaw)>) {
            if (sourceColumnIndexRaw < 0)
                return abortMouseEdit();
        }
        const std::size_t sourceColumnIndex = nonNegativeIndex(sourceColumnIndexRaw);

        const auto sourceTileIndexRaw = sourceColumn->idx(layoutTarget);
        if constexpr (std::is_signed_v<std::remove_cvref_t<decltype(sourceTileIndexRaw)>>) {
            if (sourceTileIndexRaw < 0)
                return abortMouseEdit();
        }
        sourceColumn->remove(layoutTarget);

        const bool erasedSourceColumn = eraseColumnIfEmpty(data, sourceColumn);
        if (data->columns.empty())
            return abortMouseEdit();

        std::size_t columnIndex = std::min(target.insertion.column, data->columns.size() - 1);
        if (erasedSourceColumn && sourceColumnIndex < columnIndex)
            --columnIndex;
        columnIndex = std::min(columnIndex, data->columns.size() - 1);

        auto column = data->columns[columnIndex];
        if (!column)
            return abortMouseEdit();

        // target.insertion.tile is already computed from the preview order with
        // the dragged window omitted, matching the column after sourceColumn->remove().
        // Do not decrement it again for same-column moves; that turns every
        // “between/after” hint into the original slot.
        std::size_t tileIndex = std::min(target.insertion.tile, column->targetDatas.size());
        addLayoutTargetToColumn(column, layoutTarget, tileIndex, false);
        data->recalculate();
    } else if (!target.floating && crossWorkspaceDrop && !dropIntoEmptyWorkspace) {
        // Cross-workspace drops stay native-owned for workspace transfer.  If the
        // drop target was an existing column, the narrow post-move retile above
        // may have stacked the moved window into that column; otherwise Hyprland's
        // default placement remains intact.  Do not touch the source workspace's
        // scrolling data here.
        if (!appliedCrossWorkspaceInColumn && !appliedCrossWorkspaceNewColumnPosition && target.monitor && g_layoutManager)
            g_layoutManager->recalculateMonitor(target.monitor);
    } else if (dropIntoEmptyWorkspace) {
        // Empty-workspace drops are the unstable path: Hyprland's normal
        // moveWindowToWorkspaceSafe() already creates the first scrolling column.
        // Do not remove/re-add the freshly moved target or call SScrollingData::add()
        // here; doing so can leave renderer-visible window state with an invalid
        // variant on the next frame.
        if (auto *scrolling = scrollingForWorkspace(workspace); scrolling && scrolling->m_scrollingData)
            scrolling->m_scrollingData->recalculate();
        if (target.monitor && g_layoutManager)
            g_layoutManager->recalculateMonitor(target.monitor);
    }

    if (sourceWorkspace && !crossWorkspaceDrop)
        refreshWorkspaceLayoutSnapshot(sourceWorkspace);
    refreshWorkspaceLayoutSnapshot(workspace);
    return refreshAfterMouseEdit("drag-drop");
}

bool OverviewController::finishDirectNiriWindowDrag() {
    if (!m_niriDragSession.active)
        return false;

    const NiriDragSession session = m_niriDragSession;
    const auto window = session.window.lock();
    const auto target = session.target;
    const auto previousRects = captureCurrentPreviewRects();
    const Vector2D releasePoint = g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : Vector2D{};

    m_niriDragSession.active = false;
    m_draggedWindowIndex.reset();

    if (!window) {
        m_niriDragSession = {};
        m_draggedWindowIndex.reset();
        return true;
    }

    const auto finishCommit = [this, session, window, target, previousRects, releasePoint] {
        const NiriDragSession previousSession = m_niriDragSession;
        m_niriDragSession = session;
        m_niriDragSession.active = false;

        if (target && directNiriDropReturnsToSourceSlot(session, target, releasePoint)) {
            const auto sourceWorkspace = session.sourceWorkspace.lock();
            refreshWorkspaceLayoutSnapshot(sourceWorkspace);
            refreshVisibleStateMetadata(selectedWindow(), previousRects.empty() ? nullptr : &previousRects, "drag-drop-cancel");
        } else if (target) {
            (void)applyDirectNiriDragTarget(window, *target, previousRects);
        } else if (session.detached) {
            const auto sourceWorkspace = session.sourceWorkspace.lock();
            restoreDetachedDragSource(window, sourceWorkspace, session.sourceColumn, session.sourceTile, session.sourceColumnWidth);
            refreshWorkspaceLayoutSnapshot(sourceWorkspace);
            rebuildVisibleState(window, true);
        } else {
            rebuildVisibleState(window, true);
        }

        m_niriDragSession = previousSession.active ? previousSession : NiriDragSession{};
        damageOwnedMonitors();
    };

    if (g_pEventLoopManager) {
        if (!g_directNiriDragFinishScheduled) {
            g_directNiriDragFinishScheduled = true;
            g_pEventLoopManager->doLater([this, finishCommit] {
                const auto runAfterRender = [this, finishCommit] {
                    if (insideRenderLifecycle() && g_pEventLoopManager) {
                        g_pEventLoopManager->doLater([this, finishCommit] {
                            g_directNiriDragFinishScheduled = false;
                            if (!insideRenderLifecycle())
                                finishCommit();
                        });
                        return;
                    }

                    g_directNiriDragFinishScheduled = false;
                    finishCommit();
                };

                runAfterRender();
            });
        }
        m_niriDragSession = {};
        m_draggedWindowIndex.reset();
        damageOwnedMonitors();
        return true;
    }

    finishCommit();
    m_niriDragSession = {};
    m_draggedWindowIndex.reset();
    damageOwnedMonitors();
    return true;
}

void OverviewController::cancelDirectNiriWindowDrag() {
    if (!m_niriDragSession.active)
        return;

    const auto window = m_niriDragSession.window.lock();
    const auto sourceWorkspace = m_niriDragSession.sourceWorkspace.lock();
    if (window && sourceWorkspace && m_niriDragSession.detached) {
        restoreDetachedDragSource(window, sourceWorkspace, m_niriDragSession.sourceColumn, m_niriDragSession.sourceTile,
                                  m_niriDragSession.sourceColumnWidth);
        refreshWorkspaceLayoutSnapshot(sourceWorkspace);
    }

    m_niriDragSession = {};
    m_draggedWindowIndex.reset();
    if (isVisible() && window)
        rebuildVisibleState(window, true);
}

void OverviewController::renderNiriDragHint() const {
    if (!m_niriDragSession.active || !m_niriDragSession.target || m_niriDragSession.target->floating)
        return;
    const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!renderMonitor || renderMonitor != m_niriDragSession.target->monitor)
        return;

    const Rect hint = overviewRect(m_niriDragSession.target->insertion.hint);
    const double scale = renderMonitor->m_scale > 0.0 ? renderMonitor->m_scale : 1.0;
    const Rect local{
        (hint.x - renderMonitor->m_position.x) * scale,
        (hint.y - renderMonitor->m_position.y) * scale,
        hint.width * scale,
        hint.height * scale,
    };
    Render::GL::g_pHyprOpenGL->renderRect(CBox{local.x, local.y, local.width, local.height}, CHyprColor(127.0 / 255.0, 200.0 / 255.0, 1.0, 0.5), {});
}

} // namespace hymission
