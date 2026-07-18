#include <any>
#include <sstream>

#define private public
#include <hyprland/src/layout/supplementary/DragController.hpp>
#undef private

#include "overview_controller.hpp"

#include <algorithm>
#include <cmath>

#include <hyprland/src/config/ConfigValue.hpp>
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

} // namespace

bool OverviewController::directNiriMouseResizeOwnsWindow(const PHLWINDOW& window) const {
    return window && m_directNiriMouseResizeWindow.lock() == window;
}

PHLWINDOW OverviewController::directNiriMouseResizeTargetAtPointer() const {
    const Vector2D pointer = g_pInputManager->getMouseCoordsInternal();
    const auto index = hitTestTarget(pointer.x, pointer.y);
    if (!index || *index >= m_state.windows.size())
        return {};

    const auto window = m_state.windows[*index].window;
    if (!window || !window->m_isMapped || window->m_fadingOut || window->m_pinned || !window->m_workspace ||
        window->m_workspace != activeLayoutWorkspace() || !isScrollingWorkspace(window->m_workspace) || !hasManagedWindow(window) || !window->layoutTarget())
        return {};

    return window;
}

void OverviewController::beginDirectNiriMouseResize(const PHLWINDOW& window, eMouseBindMode mode) {
    if (!window || !window->layoutTarget() || g_layoutManager->dragController()->target())
        return;

    clearStripWindowDragState();
    if (m_state.relayoutActive)
        (void)commitActiveNiriRelayoutForRetarget();

    selectWindowInState(m_state, window);
    m_state.focusDuringOverview = window;
    m_queuedOverviewSelectionTarget.reset();
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewSelectionCenterCursor = false;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusCenterCursor = false;

    const Vector2D pointer = g_pInputManager->getMouseCoordsInternal();
    const auto* managed = managedWindowFor(m_state, window, true);
    const Rect preview = managed ? currentPreviewRect(*managed) : liveGlobalRectForWindow(window);
    m_directNiriMouseResizePointer = pointer;
    m_directNiriMouseResizeScale = resizePreviewScale(preview, window->layoutTarget()->position());
    m_directNiriMouseResizeThresholdReached = nativeWindowDragThreshold() <= 0.0;
    m_directNiriMouseResizeWindow = window;
    g_layoutManager->beginDragTarget(window->layoutTarget(), mode);

    const auto& dragController = g_layoutManager->dragController();
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

    g_layoutManager->moveMouse(nativeResizePointer(m_directNiriMouseResizePointer, pointer, m_directNiriMouseResizeScale));
    damageOwnedMonitors();
    return true;
}

void OverviewController::finishDirectNiriMouseResize(bool refreshLayout) {
    const auto window = m_directNiriMouseResizeWindow.lock();
    if (window) {
        const auto dragTarget = g_layoutManager->dragController()->target();
        if (dragTarget && dragTarget == window->layoutTarget())
            g_layoutManager->endDragTarget();
    }

    m_directNiriMouseResizeWindow.reset();
    m_directNiriMouseResizePointer = {};
    m_directNiriMouseResizeScale = {1.0, 1.0};
    m_directNiriMouseResizeThresholdReached = false;
    if (!window || !refreshLayout || !isVisible() || m_state.phase != Phase::Active || !window->m_isMapped || !hasManagedWindow(window))
        return;

    const auto previewRects = captureCurrentPreviewRects();
    if (window->m_workspace)
        refreshWorkspaceLayoutSnapshot(window->m_workspace);
    refreshNiriScrollingOverviewAfterLayoutScroll("mouse-resize", &previewRects);
    selectWindowInState(m_state, window);
    m_state.focusDuringOverview = window;
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

    const auto window = directNiriMouseResizeTargetAtPointer();
    if (!window)
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

    beginDirectNiriMouseResize(window, mode);
    return Config::Actions::SActionResult{};
}

} // namespace hymission
