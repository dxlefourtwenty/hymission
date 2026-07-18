#include <any>
#include <sstream>

#define private public
#include <hyprland/src/layout/supplementary/DragController.hpp>
#undef private

#include "overview_controller.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

namespace hymission {

namespace {

Layout::eRectCorner resizeCornerForPreview(const Rect& preview, const Vector2D& pointer, bool floating) {
    if (floating) {
        static auto PRESIZECORNER = CConfigValue<Config::INTEGER>("general:resize_corner");
        switch (*PRESIZECORNER) {
            case 1: return Layout::CORNER_TOPLEFT;
            case 2: return Layout::CORNER_TOPRIGHT;
            case 3: return Layout::CORNER_BOTTOMRIGHT;
            case 4: return Layout::CORNER_BOTTOMLEFT;
            default: break;
        }
    }

    const bool left = pointer.x < preview.centerX();
    const bool top = pointer.y < preview.centerY();
    if (left)
        return top ? Layout::CORNER_TOPLEFT : Layout::CORNER_BOTTOMLEFT;
    return top ? Layout::CORNER_TOPRIGHT : Layout::CORNER_BOTTOMRIGHT;
}

const char* resizeCursorName(Layout::eRectCorner corner) {
    switch (corner) {
        case Layout::CORNER_TOPLEFT: return "nw-resize";
        case Layout::CORNER_TOPRIGHT: return "ne-resize";
        case Layout::CORNER_BOTTOMLEFT: return "sw-resize";
        default: return "se-resize";
    }
}

Vector2D resizePreviewScale(const Rect& preview, const CBox& nativeBox) {
    return {
        nativeBox.width > 1.0 ? std::max(0.01, preview.width / nativeBox.width) : 1.0,
        nativeBox.height > 1.0 ? std::max(0.01, preview.height / nativeBox.height) : 1.0,
    };
}

Vector2D nativeResizePointer(const Vector2D& start, const Vector2D& pointer, const Vector2D& scale) {
    const Vector2D delta = pointer - start;
    return start + Vector2D{delta.x / scale.x, delta.y / scale.y};
}

void resizeFloatingTargetWithoutWorkspaceTransfer(const PHLWINDOW& window, const SP<Layout::ITarget>& target,
                                                  Layout::Supplementary::CDragStateController& dragController, const Vector2D& pointer) {
    Vector2D newSize = dragController.m_beginDragSizeXY;
    Vector2D newPosition = dragController.m_beginDragPositionXY;
    const Vector2D delta = pointer - dragController.m_beginDragXY;

    if (dragController.m_grabbedCorner == Layout::CORNER_BOTTOMRIGHT)
        newSize += delta;
    else if (dragController.m_grabbedCorner == Layout::CORNER_TOPLEFT)
        newSize -= delta;
    else if (dragController.m_grabbedCorner == Layout::CORNER_TOPRIGHT)
        newSize += Vector2D{delta.x, -delta.y};
    else if (dragController.m_grabbedCorner == Layout::CORNER_BOTTOMLEFT)
        newSize += Vector2D{-delta.x, delta.y};

    Vector2D minimumSize = target->minSize().value_or(Vector2D{MIN_WINDOW_SIZE, MIN_WINDOW_SIZE});
    Vector2D maximumSize = target->maxSize().value_or(Math::VECTOR2D_MAX);
    auto mode = dragController.m_dragMode;
    if (window->m_ruleApplicator->keepAspectRatio().valueOrDefault() && mode != MBIND_RESIZE_BLOCK_RATIO)
        mode = MBIND_RESIZE_FORCE_RATIO;
    if (dragController.m_beginDragSizeXY.x >= 1.0 && dragController.m_beginDragSizeXY.y >= 1.0 && mode == MBIND_RESIZE_FORCE_RATIO) {
        const double ratio = dragController.m_beginDragSizeXY.y / dragController.m_beginDragSizeXY.x;
        minimumSize = minimumSize.x * ratio > minimumSize.y ? Vector2D{minimumSize.x, minimumSize.x * ratio} :
                                                                Vector2D{minimumSize.y / ratio, minimumSize.y};
        maximumSize = maximumSize.x * ratio < maximumSize.y ? Vector2D{maximumSize.x, maximumSize.x * ratio} :
                                                                Vector2D{maximumSize.y / ratio, maximumSize.y};
        newSize = newSize.x * ratio > newSize.y ? Vector2D{newSize.x, newSize.x * ratio} : Vector2D{newSize.y / ratio, newSize.y};
    }

    newSize = newSize.clamp(minimumSize, maximumSize);
    if (dragController.m_grabbedCorner == Layout::CORNER_TOPLEFT)
        newPosition = newPosition - newSize + dragController.m_beginDragSizeXY;
    else if (dragController.m_grabbedCorner == Layout::CORNER_TOPRIGHT)
        newPosition += Vector2D{0.0, (dragController.m_beginDragSizeXY - newSize).y};
    else if (dragController.m_grabbedCorner == Layout::CORNER_BOTTOMLEFT)
        newPosition += Vector2D{(dragController.m_beginDragSizeXY - newSize).x, 0.0};

    static auto SNAPENABLED = CConfigValue<Config::INTEGER>("general:snap:enabled");
    if (*SNAPENABLED) {
        g_layoutManager->performSnap(newPosition, newSize, target, mode, dragController.m_grabbedCorner, dragController.m_beginDragSizeXY);
        newSize = newSize.clamp(minimumSize, maximumSize);
    }

    CBox geometry{newPosition, newSize};
    geometry.round();
    target->setPositionGlobal(geometry);
    target->warpPositionSize();
    dragController.m_lastDragXY = pointer;
}

} // namespace

bool OverviewController::directNiriMouseResizeOwnsWindow(const PHLWINDOW& window) const {
    return window && m_directNiriMouseResizeWindow.lock() == window;
}

bool OverviewController::directNiriAdjacentTiledMouseResizeActive(const PHLWINDOW& window) const {
    const auto target = window ? window->layoutTarget() : nullptr;
    return m_directNiriMouseResizePreservesWorkspace && directNiriMouseResizeOwnsWindow(window) && target && !target->floating() &&
        activeDirectNiriSingleWorkspaceOverview() && m_state.phase == Phase::Active;
}

bool OverviewController::suppressWorkspaceChangeForDirectNiriMouseResize(const PHLWORKSPACE& workspace) const {
    const auto window = m_directNiriMouseResizeWindow.lock();
    return m_directNiriMouseResizePreservesWorkspace && window && workspace && window->m_workspace == workspace &&
        activeDirectNiriSingleWorkspaceOverview() && m_state.phase == Phase::Active;
}

void OverviewController::logDirectNiriMouseResizeGeometry(const PHLWINDOW& window, const SP<Layout::ITarget>& target) const {
    if (!debugLogsEnabled() || !window || !target)
        return;

    const auto* managed = managedWindowFor(m_state, window, true);
    if (!managed)
        return;

    const Rect preview = currentPreviewRect(*managed);
    const CBox layoutBox = target->position();
    const Vector2D livePosition = window->m_realPosition->value();
    const Vector2D liveSize = window->m_realSize->value();
    const Vector2D goalPosition = window->m_realPosition->goal();
    const Vector2D goalSize = window->m_realSize->goal();
    const Vector2D reportedSize = window->getReportedSize();
    const auto root = window->wlSurface() ? window->wlSurface()->resource() : nullptr;
    const Vector2D bufferSize = root ? root->m_current.size : Vector2D{};
    const double scaleX = liveSize.x > 1.0 ? preview.width / liveSize.x : 0.0;
    const double scaleY = liveSize.y > 1.0 ? preview.height / liveSize.y : 0.0;

    std::ostringstream out;
    out << "[hymission] direct niri mouse resize geometry"
        << " target=" << debugWindowLabel(window)
        << " preserveWorkspace=" << (m_directNiriMouseResizePreservesWorkspace ? 1 : 0)
        << " layout=" << layoutBox.x << ',' << layoutBox.y << ' ' << layoutBox.width << 'x' << layoutBox.height
        << " live=" << livePosition.x << ',' << livePosition.y << ' ' << liveSize.x << 'x' << liveSize.y
        << " goal=" << goalPosition.x << ',' << goalPosition.y << ' ' << goalSize.x << 'x' << goalSize.y
        << " preview=" << preview.x << ',' << preview.y << ' ' << preview.width << 'x' << preview.height
        << " previewScale=(" << scaleX << ',' << scaleY << ')'
        << " reported=" << reportedSize.x << 'x' << reportedSize.y
        << " buffer=" << bufferSize.x << 'x' << bufferSize.y;
    debugLog(out.str());
}

std::optional<std::pair<PHLWINDOW, Rect>> OverviewController::directNiriMouseResizeTargetAtPointer() const {
    const Vector2D pointer = g_pInputManager->getMouseCoordsInternal();
    const auto eligible = [&](const PHLWINDOW& window) {
        return window && window->m_isMapped && !window->m_fadingOut && !window->m_pinned && !window->onSpecialWorkspace() && window->m_workspace &&
            isScrollingWorkspace(window->m_workspace) && hasManagedWindow(window) && window->layoutTarget();
    };

    if (const auto stripIndex = hitTestStripTarget(pointer.x, pointer.y); stripIndex && *stripIndex < m_state.stripEntries.size()) {
        const auto& entry = m_state.stripEntries[*stripIndex];
        if (entry.monitor && !entry.newWorkspaceSlot) {
            const Rect stripRect = animatedWorkspaceStripRect(currentWorkspaceStripRect(entry), entry.monitor);
            std::optional<std::pair<PHLWINDOW, Rect>> bestTarget;
            double bestDistance = std::numeric_limits<double>::infinity();
            for (const auto& candidate : entry.windows) {
                if (!eligible(candidate.window))
                    continue;

                const auto preview = workspaceStripWindowPreviewRect(entry, stripRect, candidate.window);
                if (!preview || pointer.x < preview->x || pointer.y < preview->y || pointer.x > preview->x + preview->width ||
                    pointer.y > preview->y + preview->height)
                    continue;

                const double dx = preview->centerX() - pointer.x;
                const double dy = preview->centerY() - pointer.y;
                const double distance = dx * dx + dy * dy;
                if (!bestTarget || distance < bestDistance) {
                    bestTarget = std::pair{candidate.window, *preview};
                    bestDistance = distance;
                }
            }

            if (bestTarget)
                return bestTarget;
        }
    }

    const auto index = hitTestTarget(pointer.x, pointer.y);
    if (!index || *index >= m_state.windows.size())
        return std::nullopt;

    const auto& managed = m_state.windows[*index];
    if (!eligible(managed.window))
        return std::nullopt;

    return std::pair{managed.window, currentPreviewRect(managed)};
}

void OverviewController::beginDirectNiriMouseResize(const PHLWINDOW& window, const Rect& preview, eMouseBindMode mode) {
    if (!window || !window->layoutTarget() || preview.width <= 1.0 || preview.height <= 1.0 || g_layoutManager->dragController()->target())
        return;

    clearStripWindowDragState();
    if (m_state.relayoutActive)
        (void)commitActiveNiriRelayoutForRetarget();

    const bool preserveWorkspace = window->m_workspace != activeLayoutWorkspace();
    const bool preserveFocus = preserveWorkspace || Desktop::focusState()->window() != window;
    if (!preserveFocus) {
        selectWindowInState(m_state, window);
        m_state.focusDuringOverview = window;
        m_queuedOverviewSelectionTarget.reset();
        m_queuedOverviewSelectionSyncScrollingSpot = false;
        m_queuedOverviewSelectionCenterCursor = false;
        m_queuedOverviewLiveFocusTarget.reset();
        m_queuedOverviewLiveFocusSyncScrollingSpot = false;
        m_queuedOverviewLiveFocusCenterCursor = false;
    }

    const Vector2D pointer = g_pInputManager->getMouseCoordsInternal();
    m_directNiriMouseResizePointer = pointer;
    m_directNiriMouseResizeScale = resizePreviewScale(preview, window->layoutTarget()->position());
    m_directNiriMouseResizeThresholdReached = nativeWindowDragThreshold() <= 0.0;
    m_directNiriMouseResizePreservesFocus = preserveFocus;
    m_directNiriMouseResizePreservesWorkspace = preserveWorkspace;
    m_directNiriMouseResizeWindow = window;

    const auto& dragController = g_layoutManager->dragController();
    if (preserveFocus) {
        const auto target = window->layoutTarget();
        dragController->m_target = target;
        dragController->m_dragMode = mode;
        dragController->m_draggingTiled = false;
        dragController->m_wasDraggingWindow = false;
        dragController->m_mouseMoveEventCount = 1;
        dragController->m_beginDragXY = pointer;
        dragController->m_lastDragXY = pointer;
        dragController->m_beginDragPositionXY = target->position().pos();
        dragController->m_beginDragSizeXY = target->position().size();
        dragController->m_draggingWindowOriginalFloatSize = target->lastFloatingSize();
    } else {
        g_layoutManager->beginDragTarget(window->layoutTarget(), mode);
    }
    dragController->m_dragThresholdReached = true;
    dragController->m_grabbedCorner = resizeCornerForPreview(preview, pointer, window->layoutTarget()->floating());
    Cursor::overrideController->setOverride(resizeCursorName(dragController->m_grabbedCorner), Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    damageOwnedMonitors();

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] direct niri mouse resize begin"
            << " target=" << debugWindowLabel(window)
            << " workspace=" << debugWorkspaceLabel(window->m_workspace)
            << " mode=" << static_cast<int>(mode)
            << " corner=" << static_cast<int>(dragController->m_grabbedCorner)
            << " preserveFocus=" << (preserveFocus ? 1 : 0)
            << " preserveWorkspace=" << (preserveWorkspace ? 1 : 0)
            << " scale=(" << m_directNiriMouseResizeScale.x << ',' << m_directNiriMouseResizeScale.y << ')';
        debugLog(out.str());
    }
}

bool OverviewController::updateDirectNiriMouseResize(const Vector2D& pointer) {
    const auto window = m_directNiriMouseResizeWindow.lock();
    if (!window)
        return false;

    const auto& dragController = g_layoutManager->dragController();
    if (!dragController->target() || dragController->target() != window->layoutTarget()) {
        finishDirectNiriMouseResize(false);
        return false;
    }

    if (!m_directNiriMouseResizeThresholdReached) {
        const double distance = std::hypot(pointer.x - m_directNiriMouseResizePointer.x, pointer.y - m_directNiriMouseResizePointer.y);
        if (distance < nativeWindowDragThreshold())
            return true;

        m_directNiriMouseResizeThresholdReached = true;
        m_directNiriMouseResizePointer = pointer;
        dragController->m_beginDragXY = pointer;
        dragController->m_lastDragXY = pointer;
        dragController->m_beginDragPositionXY = window->layoutTarget()->position().pos();
        dragController->m_beginDragSizeXY = window->layoutTarget()->position().size();
        return true;
    }

    const Vector2D nativePointer = nativeResizePointer(m_directNiriMouseResizePointer, pointer, m_directNiriMouseResizeScale);
    const auto target = window->layoutTarget();
    if (m_directNiriMouseResizePreservesWorkspace && target->floating()) {
        resizeFloatingTargetWithoutWorkspaceTransfer(window, target, *dragController, nativePointer);
    } else {
        g_layoutManager->moveMouse(nativePointer);
    }
    logDirectNiriMouseResizeGeometry(window, target);
    damageOwnedMonitors();
    return true;
}

void OverviewController::finishDirectNiriMouseResize(bool refreshLayout) {
    const auto window = m_directNiriMouseResizeWindow.lock();
    const bool preserveFocus = m_directNiriMouseResizePreservesFocus;
    if (window) {
        const auto dragTarget = g_layoutManager->dragController()->target();
        if (dragTarget && dragTarget == window->layoutTarget()) {
            if (preserveFocus) {
                Cursor::overrideController->unsetOverride(Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
                dragTarget->damageEntire();
                g_layoutManager->setTargetGeom(dragTarget->position(), dragTarget);
                const auto& dragController = g_layoutManager->dragController();
                dragController->m_target.reset();
                dragController->m_wasDraggingWindow = false;
                dragController->m_dragMode = MBIND_INVALID;
            } else {
                g_layoutManager->endDragTarget();
            }
        }
    }

    std::optional<PreviewRectSnapshot> previewRects;
    if (window && refreshLayout && isVisible() && m_state.phase == Phase::Active && window->m_isMapped && hasManagedWindow(window))
        previewRects = captureCurrentPreviewRects();

    m_directNiriMouseResizeWindow.reset();
    m_directNiriMouseResizePointer = {};
    m_directNiriMouseResizeScale = {1.0, 1.0};
    m_directNiriMouseResizeThresholdReached = false;
    m_directNiriMouseResizePreservesFocus = false;
    m_directNiriMouseResizePreservesWorkspace = false;
    if (!previewRects)
        return;

    if (window->m_workspace)
        refreshWorkspaceLayoutSnapshot(window->m_workspace);
    refreshNiriScrollingOverviewAfterLayoutScroll("mouse-resize", &*previewRects);
    if (!preserveFocus) {
        selectWindowInState(m_state, window);
        m_state.focusDuringOverview = window;
    }
    damageOwnedMonitors();

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] direct niri mouse resize end"
            << " target=" << debugWindowLabel(window)
            << " workspace=" << debugWorkspaceLabel(window->m_workspace);
        debugLog(out.str());
    }
}

std::optional<Config::Actions::ActionResult> OverviewController::mouseActionHook(const std::string& action) {
    if (!m_forwardingOverviewMouseBind || !activeDirectNiriSingleWorkspaceOverview() || m_state.phase != Phase::Active)
        return std::nullopt;

    if (!action.starts_with("resizewindow"))
        return Config::Actions::SActionResult{};

    const int actionState = Config::Actions::state()->m_passPressed;
    if (actionState == 0) {
        finishDirectNiriMouseResize(true);
        return Config::Actions::SActionResult{};
    }

    if (actionState != 1)
        return Config::Actions::SActionResult{};

    const auto target = directNiriMouseResizeTargetAtPointer();
    if (!target)
        return Config::Actions::SActionResult{};

    eMouseBindMode mode = MBIND_RESIZE;
    const auto separator = action.find(' ');
    if (separator != std::string::npos) {
        const auto ratioMode = action.substr(separator + 1);
        if (ratioMode == "1")
            mode = MBIND_RESIZE_FORCE_RATIO;
        else if (ratioMode == "2")
            mode = MBIND_RESIZE_BLOCK_RATIO;
    }

    beginDirectNiriMouseResize(target->first, target->second, mode);
    return Config::Actions::SActionResult{};
}

} // namespace hymission
