#include "overview_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
#include <typeinfo>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/core/DataDevice.hpp>

namespace hymission {
namespace {

constexpr auto DND_TICK_INTERVAL = std::chrono::milliseconds(16);
constexpr double WORKSPACE_DND_EDGE_SCROLL_MOVEMENT = 1500.0;

long configInt(const char* name, long fallback) {
    const auto value = Config::mgr()->getConfigValue(name);
    if (!value.dataptr || !value.type)
        return fallback;
    if (*value.type == typeid(bool))
        return **reinterpret_cast<bool* const*>(value.dataptr) ? 1L : 0L;
    if (*value.type == typeid(Config::INTEGER))
        return static_cast<long>(**reinterpret_cast<Config::INTEGER* const*>(value.dataptr));
    return fallback;
}

double configFloat(const char* name, double fallback) {
    const auto value = Config::mgr()->getConfigValue(name);
    if (!value.dataptr || !value.type)
        return fallback;
    if (*value.type == typeid(Config::FLOAT))
        return static_cast<double>(**reinterpret_cast<Config::FLOAT* const*>(value.dataptr));
    if (*value.type == typeid(Config::INTEGER))
        return static_cast<double>(**reinterpret_cast<Config::INTEGER* const*>(value.dataptr));
    return fallback;
}

bool contains(const Rect& rect, const Vector2D& point) {
    return rect.width > 1.0 && rect.height > 1.0 && point.x >= rect.x && point.x <= rect.x + rect.width && point.y >= rect.y && point.y <= rect.y + rect.height;
}

double area(const Rect& rect) { return std::max(0.0, rect.width) * std::max(0.0, rect.height); }

SP<CWLSurfaceResource> dndSurfaceForWindow(const PHLWINDOW& window) {
    if (!window || !window->m_isMapped || window->m_fadingOut || !window->wlSurface() || !window->wlSurface()->exists())
        return {};

    return window->wlSurface()->resource();
}

Layout::Tiled::CScrollingAlgorithm* scrollingForWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space)
        return nullptr;

    const auto algorithm = workspace->m_space->algorithm();
    if (!algorithm || !algorithm->tiledAlgo())
        return nullptr;

    return dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(algorithm->tiledAlgo().get());
}

} // namespace

bool OverviewController::directNiriDndProtocolActive() const { return PROTO::data && PROTO::data->dndActive(); }

bool OverviewController::directNiriDndApplies() const { return activeDirectNiriSingleWorkspaceOverview() && !m_niriDragSession.active; }

void OverviewController::suppressDirectNiriDndSurfaceFocus() const {
    if (!g_pSeatManager || !g_pSeatManager->m_state.dndPointerFocus)
        return;

    g_pSeatManager->m_state.dndPointerFocus.reset();
    g_pSeatManager->m_events.dndPointerFocusChange.emit();
}

bool OverviewController::updateDirectNiriDndSurfaceFocus(const std::optional<NiriDndTarget>& target) const {
    if (!g_pSeatManager)
        return false;

    const auto window = target ? target->window : PHLWINDOW{};
    const auto surface = dndSurfaceForWindow(window);
    const auto previousSurface = g_pSeatManager->m_state.dndPointerFocus.lock();
    if (!surface) {
        suppressDirectNiriDndSurfaceFocus();
        if (previousSurface && debugLogsEnabled())
            debugLog("[hymission] direct niri dnd target cleared");
        return false;
    }

    if (previousSurface == surface)
        return true;

    g_pSeatManager->setPointerFocus(surface, surface->m_current.size / 2.0);
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] direct niri dnd target"
            << " window=" << debugWindowLabel(window)
            << " workspace=" << debugWorkspaceLabel(window->m_workspace);
        debugLog(out.str());
    }
    return true;
}

bool OverviewController::handleDirectNiriDndMotion(const Vector2D& pointer) {
    if (m_niriDndExitInProgress) {
        if (!directNiriDndProtocolActive()) {
            m_niriDndExitInProgress = false;
        } else if (isVisible()) {
            suppressDirectNiriDndSurfaceFocus();
            return true;
        } else {
            m_niriDndExitInProgress = false;
            return false;
        }
    }

    if (!directNiriDndProtocolActive() || !directNiriDndApplies()) {
        if (m_niriDndSession.active)
            clearDirectNiriDndState();
        return false;
    }

    if (!m_niriDndSession.active) {
        m_niriDndSession.active = true;
        m_niriDndSession.lastEdgeTick = std::chrono::steady_clock::now();
    }

    updateHoveredFromPointer(false, false, false, false, "dnd-motion");
    updateDirectNiriDnd(pointer);
    return true;
}

bool OverviewController::handleDirectNiriDndButton(const IPointer::SButtonEvent& event) {
    if (m_niriDndExitInProgress && directNiriDndProtocolActive()) {
        if (event.state == WL_POINTER_BUTTON_STATE_RELEASED)
            PROTO::data->abortDndIfPresent();
        return event.state != WL_POINTER_BUTTON_STATE_RELEASED;
    }

    if (!m_niriDndSession.active && !(directNiriDndProtocolActive() && directNiriDndApplies()))
        return false;

    if (event.state != WL_POINTER_BUTTON_STATE_RELEASED)
        return true;

    const auto target = m_state.phase == Phase::Active && !m_workspaceTransition.active ?
        directNiriDndTargetAt(g_pInputManager->getMouseCoordsInternal()) : std::nullopt;
    const bool validSurface = updateDirectNiriDndSurfaceFocus(target);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] direct niri dnd release"
            << " targetWindow=" << (target && target->window ? debugWindowLabel(target->window) : "<none>")
            << " surface=" << (validSurface ? "valid" : "invalid")
            << " protocolActive=" << (directNiriDndProtocolActive() ? 1 : 0);
        debugLog(out.str());
    }

    clearDirectNiriDndState();
    return false;
}

void OverviewController::updateDirectNiriDnd(const Vector2D& pointer) {
    if (!m_niriDndSession.active)
        return;

    if (m_state.phase != Phase::Active || m_workspaceTransition.active) {
        const auto now = std::chrono::steady_clock::now();
        suppressDirectNiriDndSurfaceFocus();
        clearDirectNiriDndHold();
        clearDirectNiriDndViewEdge(now);
        clearDirectNiriDndWorkspaceEdge(now);
        tickDirectNiriDnd();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto workspaceEdge = directNiriDndWorkspaceEdgeAt(pointer);
    const auto target = directNiriDndTargetAt(pointer);
    (void)updateDirectNiriDndSurfaceFocus(target);
    const double edgeVelocity = target ? directNiriDndEdgeVelocity(*target, pointer) : 0.0;
    if (workspaceEdge)
        updateDirectNiriDndWorkspaceEdge(workspaceEdge->first, workspaceEdge->second, now);
    else
        clearDirectNiriDndWorkspaceEdge(now);

    if (target && std::abs(edgeVelocity) > 0.001)
        updateDirectNiriDndEdge(*target, edgeVelocity, now);
    else
        clearDirectNiriDndViewEdge(now);

    if (!workspaceEdge && std::abs(edgeVelocity) <= 0.001)
        updateDirectNiriDndHold(target, now);
    else
        clearDirectNiriDndHold();

    tickDirectNiriDnd();
}

std::optional<OverviewController::NiriDndTarget> OverviewController::directNiriDndTargetAt(const Vector2D& pointer) const {
    PHLWINDOW window;
    if (const auto hoveredIndex = hitTestTarget(pointer.x, pointer.y); hoveredIndex && *hoveredIndex < m_state.windows.size()) {
        const auto candidate = m_state.windows[*hoveredIndex].window;
        if (candidate && candidate->m_isMapped && !candidate->m_fadingOut)
            window = candidate;
    }

    const EmptyWorkspacePlaceholder* lane = nullptr;
    Rect laneRect;
    double laneArea = std::numeric_limits<double>::infinity();
    const auto preferredWorkspace = window ? window->m_workspace : PHLWORKSPACE{};
    for (const auto& placeholder : m_state.emptyWorkspacePlaceholders) {
        if (!placeholder.monitor || placeholder.workspaceId == WORKSPACE_INVALID)
            continue;

        const Rect current = currentEmptyWorkspacePlaceholderRect(placeholder);
        if (!contains(current, pointer))
            continue;

        const bool preferred = preferredWorkspace && (placeholder.workspace == preferredWorkspace || placeholder.workspaceId == preferredWorkspace->m_id);
        const bool lanePreferred = lane && preferredWorkspace && (lane->workspace == preferredWorkspace || lane->workspaceId == preferredWorkspace->m_id);
        const double currentArea = area(current);
        if (!lane || (preferred && !lanePreferred) || (preferred == lanePreferred && currentArea < laneArea)) {
            lane = &placeholder;
            laneRect = current;
            laneArea = currentArea;
        }
    }

    const auto workspace = window ? window->m_workspace : (lane ? lane->workspace : PHLWORKSPACE{});
    const auto monitor = lane ? lane->monitor : (workspace ? workspace->m_monitor.lock() : PHLMONITOR{});
    const WORKSPACEID workspaceId = workspace ? workspace->m_id : (lane ? lane->workspaceId : WORKSPACE_INVALID);
    if (!window && workspaceId == WORKSPACE_INVALID)
        return std::nullopt;

    return NiriDndTarget{
        .window = window,
        .workspace = workspace,
        .monitor = monitor,
        .workspaceId = workspaceId,
        .laneGlobal = laneRect,
        .hasLane = lane != nullptr,
    };
}

double OverviewController::directNiriDndEdgeVelocity(const NiriDndTarget& target, const Vector2D& pointer) const {
    if (!target.hasLane || !target.workspace || !scrollingForWorkspace(target.workspace))
        return 0.0;

    const auto direction = scrollingLayoutDirection();
    const bool horizontal = direction == ScrollingLayoutDirection::Right || direction == ScrollingLayoutDirection::Left;
    const bool reversed = direction == ScrollingLayoutDirection::Left || direction == ScrollingLayoutDirection::Up;
    return overview_drag::edgeScrollVelocity({target.laneGlobal.x, target.laneGlobal.y, target.laneGlobal.width, target.laneGlobal.height}, pointer.x,
                                             pointer.y, horizontal ? overview_drag::Axis::Horizontal : overview_drag::Axis::Vertical, reversed,
                                             std::max(0.0, configFloat("plugin:hymission:niri_dnd_edge_view_scroll_trigger", 30.0)),
                                             std::max(0.0, configFloat("plugin:hymission:niri_dnd_edge_view_scroll_max_speed", 1500.0)));
}

std::optional<std::pair<PHLMONITOR, double>> OverviewController::directNiriDndWorkspaceEdgeAt(const Vector2D& pointer) const {
    const auto monitor = g_pCompositor->getMonitorFromVector(pointer);
    if (!monitor || !ownsMonitor(monitor))
        return std::nullopt;

    const Rect content = overviewContentRectForMonitor(monitor, m_state);
    const Rect global = {monitor->m_position.x + content.x, monitor->m_position.y + content.y, content.width, content.height};
    if (global.width <= 1.0 || global.height <= 1.0 || pointer.x < global.x || pointer.x >= global.x + global.width)
        return std::nullopt;

    const double velocity =
        overview_drag::edgeScrollVelocity({global.x, global.y, global.width, global.height}, pointer.x, pointer.y, overview_drag::Axis::Vertical, false,
                                          std::max(0.0, configFloat("plugin:hymission:niri_dnd_edge_workspace_switch_trigger", 50.0)),
                                          std::max(0.0, configFloat("plugin:hymission:niri_dnd_edge_workspace_switch_max_speed", 1500.0)));
    if (std::abs(velocity) <= 0.001)
        return std::nullopt;

    return std::pair{monitor, velocity};
}

void OverviewController::updateDirectNiriDndEdge(const NiriDndTarget& target, double velocity, std::chrono::steady_clock::time_point now) {
    const auto previousWorkspace = m_niriDndSession.edgeWorkspace.lock();
    if (previousWorkspace != target.workspace || m_niriDndSession.edgeVelocity * velocity <= 0.0) {
        m_niriDndSession.edgeEnteredAt = now;
        m_niriDndSession.lastEdgeTick = now;
    }

    m_niriDndSession.edgeWorkspace = target.workspace;
    m_niriDndSession.edgeVelocity = velocity;
}

void OverviewController::updateDirectNiriDndWorkspaceEdge(const PHLMONITOR& monitor, double velocity, std::chrono::steady_clock::time_point now) {
    if (m_niriDndSession.workspaceEdgeMonitor != monitor || m_niriDndSession.workspaceEdgeVelocity * velocity <= 0.0) {
        m_niriDndSession.workspaceEdgeEnteredAt = now;
        m_niriDndSession.lastWorkspaceEdgeTick = now;
        m_niriDndSession.workspaceEdgeTravel = 0.0;
    }

    m_niriDndSession.workspaceEdgeMonitor = monitor;
    m_niriDndSession.workspaceEdgeVelocity = velocity;
}

void OverviewController::updateDirectNiriDndHold(const std::optional<NiriDndTarget>& target, std::chrono::steady_clock::time_point now) {
    clearDirectNiriDndViewEdge(now);
    clearDirectNiriDndWorkspaceEdge(now);

    const auto window = target ? target->window : PHLWINDOW{};
    const auto workspace = target ? target->workspace : PHLWORKSPACE{};
    const auto monitor = target ? target->monitor : PHLMONITOR{};
    const WORKSPACEID workspaceId = target ? target->workspaceId : WORKSPACE_INVALID;
    const bool targetChanged = m_niriDndSession.holdWindow.lock() != window || m_niriDndSession.holdWorkspace.lock() != workspace ||
                               m_niriDndSession.holdMonitor != monitor || m_niriDndSession.holdWorkspaceId != workspaceId;
    if (targetChanged)
        m_niriDndSession.holdStartedAt = now;

    m_niriDndSession.holdWindow = window;
    m_niriDndSession.holdWorkspace = workspace;
    m_niriDndSession.holdMonitor = monitor;
    m_niriDndSession.holdWorkspaceId = workspaceId;
    if (!target)
        m_niriDndSession.holdStartedAt = {};
}

void OverviewController::clearDirectNiriDndHold() {
    m_niriDndSession.holdWindow.reset();
    m_niriDndSession.holdWorkspace.reset();
    m_niriDndSession.holdMonitor.reset();
    m_niriDndSession.holdWorkspaceId = WORKSPACE_INVALID;
    m_niriDndSession.holdStartedAt = {};
}

void OverviewController::clearDirectNiriDndViewEdge(std::chrono::steady_clock::time_point now) {
    m_niriDndSession.edgeWorkspace.reset();
    m_niriDndSession.edgeEnteredAt = {};
    m_niriDndSession.lastEdgeTick = now;
    m_niriDndSession.edgeVelocity = 0.0;
}

void OverviewController::clearDirectNiriDndWorkspaceEdge(std::chrono::steady_clock::time_point now) {
    m_niriDndSession.workspaceEdgeMonitor.reset();
    m_niriDndSession.workspaceEdgeEnteredAt = {};
    m_niriDndSession.lastWorkspaceEdgeTick = now;
    m_niriDndSession.workspaceEdgeVelocity = 0.0;
    m_niriDndSession.workspaceEdgeTravel = 0.0;
}

void OverviewController::tickDirectNiriDnd() {
    if (!m_niriDndSession.active)
        return;

    if (!directNiriDndProtocolActive() || !directNiriDndApplies()) {
        clearDirectNiriDndState();
        return;
    }

    if (m_state.phase != Phase::Active || m_workspaceTransition.active) {
        armDirectNiriDndTimer();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto workspaceEdgeMonitor = m_niriDndSession.workspaceEdgeMonitor;
    const auto edgeWorkspace = m_niriDndSession.edgeWorkspace.lock();
    bool edgeScrolling = false;
    if (workspaceEdgeMonitor && std::abs(m_niriDndSession.workspaceEdgeVelocity) > 0.001) {
        edgeScrolling = true;
        const auto delay = std::chrono::milliseconds(std::max(0L, configInt("plugin:hymission:niri_dnd_edge_workspace_switch_delay_ms", 100)));
        const double elapsed = std::clamp(std::chrono::duration<double>(now - m_niriDndSession.lastWorkspaceEdgeTick).count(), 0.0, 0.05);
        m_niriDndSession.lastWorkspaceEdgeTick = now;
        if (m_niriDndSession.workspaceEdgeEnteredAt != std::chrono::steady_clock::time_point{} && now - m_niriDndSession.workspaceEdgeEnteredAt >= delay &&
            elapsed > 0.0001) {
            m_niriDndSession.workspaceEdgeTravel += m_niriDndSession.workspaceEdgeVelocity * elapsed;
            if (std::abs(m_niriDndSession.workspaceEdgeTravel) >= WORKSPACE_DND_EDGE_SCROLL_MOVEMENT) {
                const int step = m_niriDndSession.workspaceEdgeTravel < 0.0 ? -1 : 1;
                m_niriDndSession.workspaceEdgeTravel = 0.0;
                m_niriDndSession.workspaceEdgeEnteredAt = now;
                if (startOverviewWorkspaceTransitionByStep(workspaceEdgeMonitor, step, WorkspaceTransitionMode::TimedCommit)) {
                    armDirectNiriDndTimer();
                    return;
                }
            }
        }
    }

    if (edgeWorkspace && std::abs(m_niriDndSession.edgeVelocity) > 0.001) {
        edgeScrolling = true;
        const auto delay = std::chrono::milliseconds(std::max(0L, configInt("plugin:hymission:niri_dnd_edge_view_scroll_delay_ms", 100)));
        const double elapsed = std::clamp(std::chrono::duration<double>(now - m_niriDndSession.lastEdgeTick).count(), 0.0, 0.05);
        m_niriDndSession.lastEdgeTick = now;
        if (m_niriDndSession.edgeEnteredAt != std::chrono::steady_clock::time_point{} && now - m_niriDndSession.edgeEnteredAt >= delay && elapsed > 0.0001) {
            if (auto* scrolling = scrollingForWorkspace(edgeWorkspace)) {
                scrolling->moveTape(static_cast<float>(-m_niriDndSession.edgeVelocity * elapsed));
                refreshWorkspaceLayoutSnapshot(edgeWorkspace);
                refreshNiriScrollingOverviewAfterLayoutScroll("dnd-edge-view-scroll");
            }
        }
    }

    if (!edgeScrolling && m_niriDndSession.holdStartedAt != std::chrono::steady_clock::time_point{}) {
        const auto delay = std::chrono::milliseconds(std::max(0L, configInt("plugin:hymission:niri_dnd_hold_to_activate_delay_ms", 750)));
        if (now - m_niriDndSession.holdStartedAt >= delay) {
            activateDirectNiriDndTarget();
            return;
        }
    }

    armDirectNiriDndTimer();
}

void OverviewController::activateDirectNiriDndTarget() {
    const auto window = m_niriDndSession.holdWindow.lock();
    auto workspace = window ? window->m_workspace : m_niriDndSession.holdWorkspace.lock();
    auto monitor = m_niriDndSession.holdMonitor;
    const WORKSPACEID workspaceId = workspace ? workspace->m_id : m_niriDndSession.holdWorkspaceId;

    if (window && window->m_isMapped) {
        const auto managed =
            std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& candidate) { return candidate.window == window; });
        if (managed == m_state.windows.end()) {
            clearDirectNiriDndState();
            return;
        }

        const auto currentWorkspace = m_state.ownerWorkspace ? m_state.ownerWorkspace : activeLayoutWorkspace();
        const bool crossWorkspace = window->m_workspace && currentWorkspace && window->m_workspace != currentWorkspace;
        if (crossWorkspace) {
            auto targetMonitor = window->m_workspace->m_monitor.lock();
            if (!targetMonitor)
                targetMonitor = monitor;
            if (!targetMonitor || !beginOverviewWorkspaceTransition(targetMonitor, window->m_workspace->m_id, window->m_workspace->m_name, window->m_workspace,
                                                                    false, WorkspaceTransitionMode::TimedCommit, std::nullopt, window)) {
                clearDirectNiriDndState();
                return;
            }

            m_niriDndExitInProgress = true;
            clearDirectNiriDndState();
            beginClose(CloseMode::ActivateSelection);
            return;
        }

        m_state.selectedIndex = static_cast<std::size_t>(std::distance(m_state.windows.begin(), managed));
        m_state.focusDuringOverview = window;
        m_niriDndExitInProgress = true;
        clearDirectNiriDndState();
        activateSelection();
        return;
    }

    if (!monitor || workspaceId == WORKSPACE_INVALID) {
        clearDirectNiriDndState();
        return;
    }

    const bool syntheticWorkspace = !workspace;
    const std::string workspaceName = workspace ? workspace->m_name : std::to_string(workspaceId);
    if (m_state.ownerWorkspace && workspaceId == m_state.ownerWorkspace->m_id) {
        m_niriDndExitInProgress = true;
        clearDirectNiriDndState();
        beginClose(CloseMode::ActivateSelection);
        return;
    }

    if (!beginOverviewWorkspaceTransition(monitor, workspaceId, workspaceName, workspace, syntheticWorkspace, WorkspaceTransitionMode::TimedCommit)) {
        clearDirectNiriDndState();
        return;
    }

    m_niriDndExitInProgress = true;
    clearDirectNiriDndState();
    beginClose(CloseMode::ActivateSelection);
}

void OverviewController::armDirectNiriDndTimer() {
    if (!g_pEventLoopManager)
        return;

    if (!m_niriDndTimer) {
        m_niriDndTimer = makeShared<CEventLoopTimer>(
            DND_TICK_INTERVAL,
            [this](SP<CEventLoopTimer> self, void*) {
                if (!m_niriDndSession.active) {
                    self->updateTimeout(std::nullopt);
                    return;
                }

                (void)handleDirectNiriDndMotion(g_pInputManager->getMouseCoordsInternal());
                self->updateTimeout(m_niriDndSession.active ? std::optional{DND_TICK_INTERVAL} : std::nullopt);
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_niriDndTimer);
        return;
    }

    m_niriDndTimer->updateTimeout(DND_TICK_INTERVAL);
}

void OverviewController::clearDirectNiriDndState() {
    m_niriDndSession = {};
    if (m_niriDndTimer)
        m_niriDndTimer->updateTimeout(std::nullopt);
}

void OverviewController::clearDirectNiriDndTimer() {
    m_niriDndSession = {};
    if (!m_niriDndTimer)
        return;

    m_niriDndTimer->cancel();
    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_niriDndTimer);
    m_niriDndTimer.reset();
}

} // namespace hymission
