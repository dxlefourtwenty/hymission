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

    const EmptyWorkspacePlaceholder *lane = nullptr;
    Rect laneRect;
    double laneArea = std::numeric_limits<double>::infinity();
    for (const auto &placeholder : m_state.emptyWorkspacePlaceholders) {
        if (!placeholder.monitor || placeholder.workspaceId == WORKSPACE_INVALID)
            continue;
        const Rect current = currentEmptyWorkspacePlaceholderRect(placeholder);
        if (!usableRect(current) || !contains(current, pointer))
            continue;

        const double area = current.width * current.height;
        const bool preferVisibleEmptyViewport = lane && lane->backingOnly && !placeholder.backingOnly;
        const bool preferSmallerSameKind = (!lane || placeholder.backingOnly == lane->backingOnly) && area < laneArea;
        if (!lane || preferVisibleEmptyViewport || preferSmallerSameKind) {
            lane = &placeholder;
            laneRect = current;
            laneArea = area;
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
            .insertion = {.hint = dragRect(directNiriDraggedPreviewRect())},
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

    const auto direction = scrollingLayoutDirection();
    const bool horizontal = direction == ScrollingLayoutDirection::Right || direction == ScrollingLayoutDirection::Left;
    const bool reversed = direction == ScrollingLayoutDirection::Left || direction == ScrollingLayoutDirection::Up;
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

    auto insertion = overview_drag::insertionTarget(dragRect(laneRect), columns, pointer.x, pointer.y,
                                                    horizontal ? overview_drag::Axis::Horizontal : overview_drag::Axis::Vertical, reversed,
                                                    dragRect(directNiriDraggedPreviewRect()));
    if (!insertion)
        return std::nullopt;

    // overview_drag::insertionTarget returns NewColumn indices in visual-column
    // space. The scrolling backend needs real SScrollingData column indices.
    // This especially matters for partial-width columns and skipped/dragged
    // columns, where visual index != backend index.
    if (insertion->kind == overview_drag::InsertKind::NewColumn && !columns.empty()) {
        std::size_t visualIndex = std::min(insertion->column, columns.size());
        if (reversed)
            visualIndex = columns.size() - visualIndex;

        std::size_t backendIndex = 0;
        if (visualIndex == 0)
            backendIndex = columns.front().index;
        else if (visualIndex >= columns.size())
            backendIndex = columns.back().index + 1;
        else
            backendIndex = columns[visualIndex].index;

        insertion->column = backendIndex;
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
    double velocity = 0.0;
    if (m_niriDragSession.target) {
        const auto &target = *m_niriDragSession.target;
        const auto lane = std::find_if(m_state.emptyWorkspacePlaceholders.begin(), m_state.emptyWorkspacePlaceholders.end(), [&](const auto &placeholder) {
            return placeholder.monitor == target.monitor && placeholder.workspaceId == target.workspaceId && usableRect(currentEmptyWorkspacePlaceholderRect(placeholder));
        });
        if (lane != m_state.emptyWorkspacePlaceholders.end()) {
            const auto direction = scrollingLayoutDirection();
            const bool horizontal = direction == ScrollingLayoutDirection::Right || direction == ScrollingLayoutDirection::Left;
            const bool reversed = direction == ScrollingLayoutDirection::Left || direction == ScrollingLayoutDirection::Up;
            velocity = overview_drag::edgeScrollVelocity(dragRect(currentEmptyWorkspacePlaceholderRect(*lane)), pointer.x, pointer.y,
                                                         horizontal ? overview_drag::Axis::Horizontal : overview_drag::Axis::Vertical, reversed,
                                                         configFloat("plugin:hymission:niri_drag_edge_scroll_trigger", 30.0),
                                                         configFloat("plugin:hymission:niri_drag_edge_scroll_max_speed", 1500.0));
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (std::abs(velocity) <= 0.001) {
        m_niriDragSession.edgeEnteredAt = {};
        m_niriDragSession.edgeVelocity = 0.0;
    } else {
        if (m_niriDragSession.edgeEnteredAt == std::chrono::steady_clock::time_point{})
            m_niriDragSession.edgeEnteredAt = now;
        m_niriDragSession.edgeVelocity = velocity;
    }
    m_niriDragSession.lastEdgeTick = now;
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

    std::optional<State> crossWorkspaceTransitionSource;
    if (!target.floating && crossWorkspaceDrop && target.monitor && m_state.collectionPolicy.onlyActiveWorkspace && usesDirectNiriScrollingOverview(m_state)) {
        if (m_workspaceTransition.active)
            commitActiveNiriWorkspaceTransitionForRetarget();

        State sourceState = captureOverviewWorkspaceTransitionSourceState();
        if (!previousPreviewRects.empty()) {
            for (auto &managed : sourceState.windows) {
                const auto rectIt = std::find_if(previousPreviewRects.begin(), previousPreviewRects.end(), [&](const auto &entry) {
                    return entry.first == managed.window;
                });
                if (rectIt == previousPreviewRects.end())
                    continue;

                managed.targetGlobal = rectIt->second;
                managed.relayoutFromGlobal = rectIt->second;
            }
        }
        selectWindowInState(sourceState, window);
        sourceState.focusDuringOverview = window;
        crossWorkspaceTransitionSource = std::move(sourceState);
    }

    if (sourceWorkspace && sourceWorkspace != workspace)
        niri_scrolling_detail::retainDirectNiriWorkspaceLaneForDrag(sourceWorkspace->m_monitor.lock(), sourceWorkspace);

    if (sourceWorkspace != workspace) {
        const bool previousGuard = m_applyingWorkspaceTransitionCommit;
        m_applyingWorkspaceTransitionCommit = true;
        g_pCompositor->moveWindowToWorkspaceSafe(window, workspace);
        m_applyingWorkspaceTransitionCommit = previousGuard;
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

    if (!target.floating && !dropIntoEmptyWorkspace && !crossWorkspaceDrop) {
        auto *scrolling = scrollingForWorkspace(workspace);
        const auto layoutTarget = window->layoutTarget();
        if (!scrolling || !scrolling->m_scrollingData || !layoutTarget)
            return false;
        const auto targetData = scrolling->dataFor(layoutTarget);
        const auto sourceColumn = targetData && targetData->column ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};

        // New side-column drops are the path that kept crashing: the coredumps
        // consistently enter Hyprland's SScrollingData::add() from our manual
        // appendColumnAt(). Do not manufacture scrolling columns from the plugin.
        // Route this case through Hyprland's normal movewindow dispatcher, which
        // owns all target/column bookkeeping for stacked and partial-width columns.
        if (target.insertion.kind == overview_drag::InsertKind::NewColumn) {
            if (!sourceColumn)
                return false;

            auto &data = scrolling->m_scrollingData;
            if (!data)
                return false;

            const auto sourceColumnIndexRaw = data->idx(sourceColumn);
            if constexpr (std::is_signed_v<decltype(sourceColumnIndexRaw)>) {
                if (sourceColumnIndexRaw < 0)
                    return false;
            }
            const std::size_t sourceColumnIndex = nonNegativeIndex(sourceColumnIndexRaw);

            const bool insertBeforeSource = std::min(target.insertion.column, data->columns.size()) <= sourceColumnIndex;
            const auto direction = scrollingLayoutDirection();
            const bool horizontal = direction == ScrollingLayoutDirection::Right || direction == ScrollingLayoutDirection::Left;
            const std::string moveDirection = horizontal ? (insertBeforeSource ? "l" : "r") : (insertBeforeSource ? "u" : "d");

            selectWindowInState(m_state, window);
            m_state.focusDuringOverview = window;

            SDispatchResult result{};
            for (const char *dispatcherName : {"window.move", "movewindow", "movewindoworgroup"}) {
                auto original = m_overviewEditingDispatchersOriginal.find(dispatcherName);
                if (original == m_overviewEditingDispatchersOriginal.end())
                    continue;

                result = runOverviewEditingDispatcher(dispatcherName, &original->second, moveDirection);
                if (result.success)
                    break;
            }

            if (!result.success)
                return false;

            workspace->m_lastFocusedWindow = window;
            refreshWorkspaceLayoutSnapshot(workspace);
            selectWindowInState(m_state, window);
            rebuildVisibleState(window, true);
            if (!previousPreviewRects.empty())
                refreshVisibleStateMetadata(window, &previousPreviewRects, "drag-drop-native-movewindow");
            return true;
        }

        // In-column reordering does not create a new SScrollingData column, so it
        // can keep using the existing column target list.  Keep this narrow and
        // never fall through to data->add() if the layout unexpectedly has no
        // destination column.
        auto &data = scrolling->m_scrollingData;
        if (!data || data->columns.empty() || !sourceColumn)
            return false;

        const auto sourceColumnIndexRaw = data->idx(sourceColumn);
        if constexpr (std::is_signed_v<decltype(sourceColumnIndexRaw)>) {
            if (sourceColumnIndexRaw < 0)
                return false;
        }
        const std::size_t sourceColumnIndex = nonNegativeIndex(sourceColumnIndexRaw);

        const auto sourceTileIndexRaw = sourceColumn->idx(layoutTarget);
        if constexpr (std::is_signed_v<decltype(sourceTileIndexRaw)>) {
            if (sourceTileIndexRaw < 0)
                return false;
        }
        const std::size_t sourceTileIndex = nonNegativeIndex(sourceTileIndexRaw);
        sourceColumn->remove(layoutTarget);

        const bool erasedSourceColumn = eraseColumnIfEmpty(data, sourceColumn);
        if (data->columns.empty())
            return false;

        std::size_t columnIndex = std::min(target.insertion.column, data->columns.size() - 1);
        if (erasedSourceColumn && sourceColumnIndex < columnIndex)
            --columnIndex;
        columnIndex = std::min(columnIndex, data->columns.size() - 1);

        auto column = data->columns[columnIndex];
        if (!column)
            return false;

        std::size_t tileIndex = std::min(target.insertion.tile, column->targetDatas.size());
        if (!erasedSourceColumn && column == sourceColumn && sourceTileIndex < tileIndex)
            --tileIndex;
        tileIndex = std::min(tileIndex, column->targetDatas.size());
        addLayoutTargetToColumn(column, layoutTarget, tileIndex, false);
        data->recalculate();
    } else if (!target.floating && crossWorkspaceDrop) {
        // Cross-workspace drops must stay native-owned. Niri routes cross-workspace
        // movement through its interactive move transaction; it does not remove a
        // live target from one workspace's scrolling data and reinsert it into
        // another after the move. Doing that here can duplicate target bookkeeping
        // or leave stale targetDatas that render as ghost columns. Let Hyprland's
        // moveWindowToWorkspaceSafe() own the target transfer, then rebuild from
        // the compositor's real window/workspace ownership.
        // Do not force-recalculate the source scrolling data immediately after a
        // native cross-workspace transfer. Hyprland can still carry old weak
        // target entries for a short time; recalculating that source workspace here
        // can revive those entries as ghost partial columns or crash the next render.
        if (target.monitor && g_layoutManager)
            g_layoutManager->recalculateMonitor(target.monitor);
    } else if (dropIntoEmptyWorkspace) {
        // Empty-workspace drops are the unstable path: Hyprland's normal
        // moveWindowToWorkspaceSafe() already creates the first scrolling column.
        // Do not remove/re-add the freshly moved target or call SScrollingData::add()
        // here; doing so can leave renderer-visible window state with an invalid
        // variant on the next frame. Also make the drop workspace the real active
        // workspace before rebuilding the overview: otherwise Hyprland hides the
        // moved window because it now belongs to an inactive workspace, leaving
        // only Hymission's selection border visible.
        if (auto *scrolling = scrollingForWorkspace(workspace); scrolling && scrolling->m_scrollingData)
            scrolling->m_scrollingData->recalculate();
        if (target.monitor && g_layoutManager)
            g_layoutManager->recalculateMonitor(target.monitor);
    }

    workspace->m_lastFocusedWindow = window;

    if (crossWorkspaceTransitionSource && target.monitor) {
        selectWindowInState(m_state, window);
        m_state.focusDuringOverview = window;

        const std::string targetName = workspace->m_name.empty() ? std::to_string(workspace->m_id) : workspace->m_name;
        const bool startedTransition = beginOverviewWorkspaceTransition(
            target.monitor,
            workspace->m_id,
            targetName,
            workspace,
            false,
            WorkspaceTransitionMode::TimedCommit,
            std::move(crossWorkspaceTransitionSource),
            window);
        if (startedTransition) {
            damageOwnedMonitors();
            return true;
        }
    }

    // Dropping into another Niri single-workspace viewport is a workspace-focus
    // handoff. Keep the overview's authoritative owner on the drop workspace
    // before rebuilding, otherwise buildState() can keep the old source viewport
    // centered while the moved window is already on the target workspace. That
    // mismatch is what produced duplicated/ghost wallpaper viewports after
    // dragging into an empty workspace.
    if (m_state.collectionPolicy.onlyActiveWorkspace && usesDirectNiriScrollingOverview(m_state)) {
        m_state.ownerWorkspace = workspace;
        m_state.ownerMonitor = target.monitor;
    }

    if (sourceWorkspace && !crossWorkspaceDrop)
        refreshWorkspaceLayoutSnapshot(sourceWorkspace);
    refreshWorkspaceLayoutSnapshot(workspace);
    selectWindowInState(m_state, window);
    rebuildVisibleState(window, true);
    if (!previousPreviewRects.empty() && !dropIntoEmptyWorkspace && !crossWorkspaceDrop)
        refreshVisibleStateMetadata(window, &previousPreviewRects, "drag-drop");
    return true;
}

bool OverviewController::finishDirectNiriWindowDrag() {
    if (!m_niriDragSession.active)
        return false;

    const NiriDragSession session = m_niriDragSession;
    const auto window = session.window.lock();
    const auto target = session.target;
    const auto previousRects = captureCurrentPreviewRects();

    m_niriDragSession.active = false;
    m_draggedWindowIndex.reset();

    if (!window) {
        m_niriDragSession = {};
        m_draggedWindowIndex.reset();
        return true;
    }

    const auto finishCommit = [this, session, window, target, previousRects] {
        const NiriDragSession previousSession = m_niriDragSession;
        m_niriDragSession = session;
        m_niriDragSession.active = false;

        if (target) {
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
