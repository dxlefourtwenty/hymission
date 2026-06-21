#include "overview_controller_niri_scrolling.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <limits>
#include <ranges>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#define private public
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#undef private

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/parserUtils/ParserUtils.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

namespace hymission {

using Render::GL::g_pHyprOpenGL;

namespace {

constexpr double RELAYOUT_DURATION_MS = 140.0;
bool&            g_niriStripSnapshotSingleWorkspaceOnly = niri_scrolling_detail::stripSnapshotSingleWorkspaceOnly;
bool             g_forceScrollingFinalLayoutBoxForOverview = false;
std::chrono::steady_clock::time_point g_multiColumnEdgeFocusOverrideUntil;

bool multiColumnEdgeFocusOverrideActive() {
    if (g_multiColumnEdgeFocusOverrideUntil == std::chrono::steady_clock::time_point{})
        return false;

    if (std::chrono::steady_clock::now() <= g_multiColumnEdgeFocusOverrideUntil)
        return true;

    g_multiColumnEdgeFocusOverrideUntil = {};
    return false;
}

class ScopedFlag {
  public:
    explicit ScopedFlag(bool& flag) : m_flag(flag), m_previous(flag) {
        m_flag = true;
    }

    ~ScopedFlag() {
        m_flag = m_previous;
    }

  private:
    bool& m_flag;
    bool  m_previous;
};

long getConfigInt(HANDLE handle, const char* name, long fallback) {
    (void)handle;

    const auto value = Config::mgr()->getConfigValue(name);
    if (!value.dataptr || !value.type)
        return fallback;

    if (*value.type == typeid(bool))
        return **reinterpret_cast<bool* const*>(value.dataptr) ? 1L : 0L;

    if (*value.type == typeid(Config::INTEGER))
        return static_cast<long>(**reinterpret_cast<Config::INTEGER* const*>(value.dataptr));

    return fallback;
}

double getConfigFloat(HANDLE handle, const char* name, double fallback) {
    (void)handle;

    const auto value = Config::mgr()->getConfigValue(name);
    if (!value.dataptr || !value.type)
        return fallback;

    if (*value.type == typeid(Config::FLOAT))
        return static_cast<double>(**reinterpret_cast<Config::FLOAT* const*>(value.dataptr));

    if (*value.type == typeid(Config::INTEGER))
        return static_cast<double>(**reinterpret_cast<Config::INTEGER* const*>(value.dataptr));

    return fallback;
}

std::string getConfigString(HANDLE handle, const char* name, std::string fallback) {
    (void)handle;

    const auto value = Config::mgr()->getConfigValue(name);
    if (!value.dataptr || !value.type)
        return fallback;

    if (*value.type == typeid(Config::STRING))
        return **reinterpret_cast<Config::STRING* const*>(value.dataptr);

    if (*value.type == typeid(Hyprlang::STRING)) {
        const auto* data = reinterpret_cast<Hyprlang::STRING const*>(value.dataptr);
        return data && *data ? std::string(*data) : fallback;
    }

    return fallback;
}

std::string asciiLowerCopy(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string trimCopy(std::string value) {
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) { return !isSpace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) { return !isSpace(ch); }).base(), value.end());
    return value;
}

Rect makeRect(double x, double y, double width, double height) {
    return {
        x,
        y,
        std::max(1.0, width),
        std::max(1.0, height),
    };
}

Rect translateRect(const Rect& rect, double dx, double dy) {
    return makeRect(rect.x + dx, rect.y + dy, rect.width, rect.height);
}

Layout::Tiled::CScrollingAlgorithm* scrollingAlgorithmForWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space)
        return nullptr;

    const auto algorithm = workspace->m_space->algorithm();
    if (!algorithm || !algorithm->tiledAlgo())
        return nullptr;

    return dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(algorithm->tiledAlgo().get());
}

bool isFloatingOverviewWindow(const PHLWINDOW& window) {
    if (!window)
        return false;

    if (window->m_isFloating)
        return true;

    const auto target = window->layoutTarget();
    return target && target->floating();
}

template <typename TargetDataPtr>
bool scrollingTargetDataBelongsToWorkspace(const TargetDataPtr& targetData, const PHLWORKSPACE& workspace) {
    if (!targetData || !targetData->target || !workspace)
        return false;

    const auto candidateWindow = targetData->target->window();
    if (!candidateWindow || !candidateWindow->m_isMapped || candidateWindow->m_fadingOut || candidateWindow->m_pinned ||
        candidateWindow->onSpecialWorkspace() || candidateWindow->m_workspace != workspace)
        return false;

    const auto liveTarget = candidateWindow->layoutTarget();
    return liveTarget && liveTarget == targetData->target && !targetData->target->floating();
}

bool scrollingDataHasStaleWorkspaceTargets(Layout::Tiled::CScrollingAlgorithm* scrolling, const PHLWORKSPACE& workspace) {
    if (!scrolling || !scrolling->m_scrollingData || !workspace)
        return false;

    for (const auto& column : scrolling->m_scrollingData->columns) {
        if (!column)
            continue;
        if (column->targetDatas.empty())
            return true;

        for (const auto& targetData : column->targetDatas) {
            if (!targetData || !targetData->target)
                return true;

            const auto candidateWindow = targetData->target->window();
            if (!candidateWindow)
                return true;

            const auto liveTarget = candidateWindow->layoutTarget();
            if (!candidateWindow->m_isMapped || candidateWindow->m_fadingOut || candidateWindow->m_pinned ||
                candidateWindow->onSpecialWorkspace() || candidateWindow->m_workspace != workspace || !liveTarget || liveTarget != targetData->target)
                return true;
        }
    }

    return false;
}



struct HydrateScrollingSnapshotResult {
    std::size_t columnsBefore = 0;
    std::size_t columnsAfter = 0;
    std::size_t spaceTargetRefs = 0;
    std::size_t acceptedSpaceTargets = 0;
    std::size_t acceptedWindowTargets = 0;
    std::size_t alreadyPresent = 0;
    std::size_t added = 0;
};

HydrateScrollingSnapshotResult hydrateScrollingDataFromWorkspaceTargets(Layout::Tiled::CScrollingAlgorithm* scrolling, const PHLWORKSPACE& workspace) {
    HydrateScrollingSnapshotResult result;
    if (!scrolling || !scrolling->m_scrollingData || !workspace || !workspace->m_space)
        return result;

    result.columnsBefore = scrolling->m_scrollingData->columns.size();

    std::vector<SP<Layout::ITarget>> targets;

    const auto alreadyQueued = [&](const SP<Layout::ITarget>& target) {
        return std::ranges::find(targets, target) != targets.end();
    };

    const auto acceptTarget = [&](const SP<Layout::ITarget>& target, bool fromSpaceTargetList) {
        if (!target || alreadyQueued(target))
            return false;

        if (target->floating())
            return false;

        const auto window = target->window();
        if (!window || !window->m_isMapped || window->m_fadingOut || window->m_pinned || window->onSpecialWorkspace() || window->m_workspace != workspace)
            return false;

        const auto liveTarget = window->layoutTarget();
        if (!liveTarget || liveTarget != target)
            return false;

        if (target->space() && target->space() != workspace->m_space)
            return false;

        targets.push_back(target);
        if (fromSpaceTargetList)
            ++result.acceptedSpaceTargets;
        else
            ++result.acceptedWindowTargets;
        return true;
    };

    for (const auto& targetRef : workspace->m_space->targets()) {
        ++result.spaceTargetRefs;
        acceptTarget(targetRef.lock(), true);
    }

    // Some inactive/unvisited workspaces can have mapped windows whose layout
    // targets are already assigned to the workspace but whose scrolling columns
    // were never materialized yet.  Fall back to the compositor window list so
    // the overview can seed the scrolling snapshot without a real workspace switch.
    for (const auto& window : g_pCompositor->m_windows) {
        if (!window || !window->m_isMapped || window->m_fadingOut || window->m_pinned || window->onSpecialWorkspace() || window->m_workspace != workspace)
            continue;

        const auto target = window->layoutTarget();
        if (!target)
            continue;

        acceptTarget(target, false);
    }

    for (const auto& target : targets) {
        if (scrolling->dataFor(target)) {
            ++result.alreadyPresent;
            continue;
        }

        const auto column = scrolling->m_scrollingData->add(std::nullopt);
        if (!column)
            continue;

        column->add(target);
        ++result.added;
    }

    result.columnsAfter = scrolling->m_scrollingData->columns.size();
    return result;
}


std::string vectorToString(const Vector2D& value) {
    std::ostringstream out;
    out << value.x << ',' << value.y;
    return out.str();
}

std::string rectToString(const Rect& rect) {
    std::ostringstream out;
    out << rect.x << ',' << rect.y << ' ' << rect.width << 'x' << rect.height;
    return out.str();
}

std::string boxToString(const CBox& box) {
    std::ostringstream out;
    out << box.x << ',' << box.y << ' ' << box.width << 'x' << box.height;
    return out.str();
}

template <typename T>
bool containsHandle(const std::vector<T>& values, const T& value) {
    return std::ranges::find(values, value) != values.end();
}

bool rectApproxEqual(const Rect& lhs, const Rect& rhs, double epsilon) {
    return std::abs(lhs.x - rhs.x) <= epsilon && std::abs(lhs.y - rhs.y) <= epsilon && std::abs(lhs.width - rhs.width) <= epsilon &&
        std::abs(lhs.height - rhs.height) <= epsilon;
}

void focusWindowCompat(const PHLWINDOW& window, bool raw = false, Desktop::eFocusReason reason = Desktop::FOCUS_REASON_OTHER) {
    if (!window)
        return;

    if (raw) {
        Desktop::focusState()->rawWindowFocus(window, reason);
        return;
    }

    Desktop::focusState()->fullWindowFocus(window, reason);
}

float animationMovePercent(std::string style) {
    std::ranges::transform(style, style.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    std::istringstream stream(style);
    std::string        token;
    float              movePercent = 100.F;
    while (stream >> token) {
        if (token.empty() || token.back() != '%')
            continue;

        try {
            movePercent = std::stof(token.substr(0, token.size() - 1));
        } catch (...) {
        }
    }

    return movePercent;
}

Vector2D predictedWorkspaceAnimationOffset(HANDLE handle, const PHLMONITOR& monitor, const PHLWORKSPACE& workspace, bool left, bool incoming) {
    if (!monitor || !workspace)
        return {};

    std::string style = workspace->m_renderOffset->getStyle();
    std::ranges::transform(style, style.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    bool vert = style.starts_with("slidevert") || style.starts_with("slidefadevert");
    if (style.find(" top") != std::string::npos) {
        left = false;
        vert = true;
    } else if (style.find(" bottom") != std::string::npos) {
        left = true;
        vert = true;
    } else if (style.find(" left") != std::string::npos) {
        left = false;
        vert = false;
    } else if (style.find(" right") != std::string::npos) {
        left = true;
        vert = false;
    }

    const float movePercent = animationMovePercent(style) / 100.F;

    if (style == "fade")
        return {};

    if (style.starts_with("slidefade")) {
        const double primaryDistance = incoming ? (left ? 1.0 : -1.0) : (left ? -1.0 : 1.0);
        if (vert)
            return Vector2D{0.0, primaryDistance * monitor->m_size.y * movePercent};

        return Vector2D{primaryDistance * monitor->m_size.x * movePercent, 0.0};
    }

    if (vert) {
        const double distance = (static_cast<double>(monitor->m_size.y) + static_cast<double>(getConfigInt(handle, "general:gaps_workspaces", 0))) * movePercent;
        return Vector2D{0.0, incoming ? (left ? distance : -distance) : (left ? -distance : distance)};
    }

    const double distance = (static_cast<double>(monitor->m_size.x) + static_cast<double>(getConfigInt(handle, "general:gaps_workspaces", 0))) * movePercent;
    return Vector2D{incoming ? (left ? distance : -distance) : (left ? -distance : distance), 0.0};
}

const char* trackpadDirectionName(eTrackpadGestureDirection direction) {
    switch (direction) {
        case TRACKPAD_GESTURE_DIR_HORIZONTAL: return "horizontal";
        case TRACKPAD_GESTURE_DIR_VERTICAL: return "vertical";
        case TRACKPAD_GESTURE_DIR_LEFT: return "left";
        case TRACKPAD_GESTURE_DIR_RIGHT: return "right";
        case TRACKPAD_GESTURE_DIR_UP: return "up";
        case TRACKPAD_GESTURE_DIR_DOWN: return "down";
        case TRACKPAD_GESTURE_DIR_SWIPE: return "swipe";
        case TRACKPAD_GESTURE_DIR_PINCH: return "pinch";
        case TRACKPAD_GESTURE_DIR_PINCH_IN: return "pinch_in";
        case TRACKPAD_GESTURE_DIR_PINCH_OUT: return "pinch_out";
        default: return "none";
    }
}

const char* gestureAxisName(GestureAxis axis) {
    return axis == GestureAxis::Vertical ? "vertical" : "horizontal";
}

const char* scrollingDirectionName(ScrollingLayoutDirection direction) {
    switch (direction) {
        case ScrollingLayoutDirection::Left: return "left";
        case ScrollingLayoutDirection::Down: return "down";
        case ScrollingLayoutDirection::Up: return "up";
        case ScrollingLayoutDirection::Right:
        default: return "right";
    }
}

struct EdgeMotionAffinity {
    double verticalEdge = 0.0;
    double horizontalEdge = 0.0;
    double corner = 0.0;
};

struct ScrollingOverviewGeometry {
    Rect        sourceGlobal;
    Rect        anchorGlobal;
    Rect        baseGlobal;
    GestureAxis primaryAxis = GestureAxis::Horizontal;
};

template <typename TargetPtr>
CBox liveScrollingLayoutBoxForTarget(const TargetPtr& target, const CBox& snapshotBox) {
    if (!target)
        return snapshotBox;

    if (g_forceScrollingFinalLayoutBoxForOverview)
        return snapshotBox;

    const CBox liveBox = target->position();
    if (liveBox.width > 1.0 && liveBox.height > 1.0)
        return liveBox;

    return snapshotBox;
}


enum class ScrollingEdgeCameraSide {
    None,
    BeforeFirst,
    AfterLast,
};

ScrollingEdgeCameraSide scrollingEdgeCameraSide(Layout::Tiled::CScrollingAlgorithm* scrolling) {
    if (!scrolling || !scrolling->m_scrollingData || !scrolling->m_scrollingData->controller)
        return ScrollingEdgeCameraSide::None;

    auto* const controller = scrolling->m_scrollingData->controller.get();
    const CBox usable = scrolling->usableArea();
    const bool fullscreenOnOne = getConfigInt(nullptr, "scrolling:fullscreen_on_one_column", 1) != 0;
    const double viewportLength = controller->isPrimaryHorizontal() ? static_cast<double>(usable.w) : static_cast<double>(usable.h);
    const double maxExtent = controller->calculateMaxExtent(usable, fullscreenOnOne);
    const double maxNormalOffset = std::max(0.0, maxExtent - std::max(1.0, viewportLength));
    const double offset = controller->getOffset();

    if (offset < -0.5)
        return ScrollingEdgeCameraSide::BeforeFirst;
    if (offset > maxNormalOffset + 0.5)
        return ScrollingEdgeCameraSide::AfterLast;

    return ScrollingEdgeCameraSide::None;
}

bool scrollingEdgeCameraActive(Layout::Tiled::CScrollingAlgorithm* scrolling) {
    return scrollingEdgeCameraSide(scrolling) != ScrollingEdgeCameraSide::None;
}

bool moveColumnCommandPrefersPrevious(std::string_view dispatcherNameLower, std::string_view dispatcherArgsLower) {
    if (dispatcherArgsLower.find("-col") != std::string_view::npos || dispatcherArgsLower.find("prev") != std::string_view::npos ||
        dispatcherArgsLower.find("left") != std::string_view::npos || dispatcherArgsLower == "l" || dispatcherArgsLower == "-1")
        return true;

    return dispatcherNameLower.find("left") != std::string_view::npos || dispatcherNameLower.find("prev") != std::string_view::npos;
}

bool moveColumnCommandPrefersNext(std::string_view dispatcherNameLower, std::string_view dispatcherArgsLower) {
    if (dispatcherArgsLower.empty())
        return true;

    if (dispatcherArgsLower.find("+col") != std::string_view::npos || dispatcherArgsLower == "col" || dispatcherArgsLower.find(" col") != std::string_view::npos ||
        dispatcherArgsLower.find("next") != std::string_view::npos || dispatcherArgsLower.find("right") != std::string_view::npos ||
        dispatcherArgsLower == "r" || dispatcherArgsLower == "+1" || dispatcherArgsLower == "1")
        return true;

    return dispatcherNameLower.find("right") != std::string_view::npos || dispatcherNameLower.find("next") != std::string_view::npos;
}

bool moveColumnCommandTargetsEdge(Layout::Tiled::CScrollingAlgorithm* scrolling, std::string_view dispatcherNameLower, std::string_view dispatcherArgsLower) {
    const auto side = scrollingEdgeCameraSide(scrolling);
    if (side == ScrollingEdgeCameraSide::None)
        return false;

    const bool prefersPrevious = moveColumnCommandPrefersPrevious(dispatcherNameLower, dispatcherArgsLower);
    const bool prefersNext = moveColumnCommandPrefersNext(dispatcherNameLower, dispatcherArgsLower);

    if (side == ScrollingEdgeCameraSide::BeforeFirst)
        return prefersPrevious && !prefersNext;

    return prefersNext && !prefersPrevious;
}

bool moveColumnCommandLeavesFocusedColumn(Layout::Tiled::CScrollingAlgorithm* scrolling, const PHLWINDOW& window,
                                          std::string_view dispatcherNameLower, std::string_view dispatcherArgsLower) {
    if (!scrolling || !scrolling->m_scrollingData || scrolling->m_scrollingData->columns.empty() || !window)
        return false;

    const auto target = window->layoutTarget();
    const auto targetData = target ? scrolling->dataFor(target) : nullptr;
    const auto column = targetData ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};
    if (!column)
        return false;

    const bool prefersPrevious = moveColumnCommandPrefersPrevious(dispatcherNameLower, dispatcherArgsLower);
    const bool prefersNext = moveColumnCommandPrefersNext(dispatcherNameLower, dispatcherArgsLower);
    if (prefersPrevious && !prefersNext)
        return column == scrolling->m_scrollingData->columns.front();

    return prefersNext && !prefersPrevious && column == scrolling->m_scrollingData->columns.back();
}

bool scrollingNativeGeometryInFlight(Layout::Tiled::CScrollingAlgorithm* scrolling) {
    if (!scrolling || !scrolling->m_scrollingData)
        return false;

    for (const auto& column : scrolling->m_scrollingData->columns) {
        if (!column)
            continue;

        for (const auto& targetData : column->targetDatas) {
            if (!targetData || !targetData->target || targetData->layoutBox.width <= 1.0 || targetData->layoutBox.height <= 1.0)
                continue;

            const auto window = targetData->target->window();
            if (!window || !window->m_realPosition || !window->m_realSize)
                continue;

            const Vector2D livePosition = window->m_realPosition->value();
            const Vector2D goalPosition = window->m_realPosition->goal();
            const Vector2D liveSize = window->m_realSize->value();
            const Vector2D goalSize = window->m_realSize->goal();
            if (std::abs(livePosition.x - goalPosition.x) > 0.5 || std::abs(livePosition.y - goalPosition.y) > 0.5 ||
                std::abs(liveSize.x - goalSize.x) > 0.5 || std::abs(liveSize.y - goalSize.y) > 0.5)
                return true;
        }
    }

    return false;
}

bool scrollingLiveCameraOwnsOverviewGeometry(Layout::Tiled::CScrollingAlgorithm* scrolling) {
    return scrollingEdgeCameraActive(scrolling) || scrollingNativeGeometryInFlight(scrolling);
}

double clampUnit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

Rect clampRectInside(const Rect& rect, const Rect& bounds) {
    const double width = std::min(rect.width, bounds.width);
    const double height = std::min(rect.height, bounds.height);
    double x = rect.x;
    double y = rect.y;

    if (x < bounds.x)
        x = bounds.x;
    if (x + width > bounds.x + bounds.width)
        x = bounds.x + bounds.width - width;

    if (y < bounds.y)
        y = bounds.y;
    if (y + height > bounds.y + bounds.height)
        y = bounds.y + bounds.height - height;

    return makeRect(x, y, width, height);
}

Rect clampRectInsidePreservingAspect(const Rect& rect, const Rect& bounds, double& scale) {
    if (bounds.width <= 1.0 || bounds.height <= 1.0)
        return rect;

    Rect result = rect;
    if (result.width > bounds.width || result.height > bounds.height) {
        const double fitScale = std::min(bounds.width / std::max(1.0, result.width), bounds.height / std::max(1.0, result.height));
        const double clampedFitScale = std::clamp(fitScale, 0.0, 1.0);
        const double width = std::max(1.0, result.width * clampedFitScale);
        const double height = std::max(1.0, result.height * clampedFitScale);
        result = makeRect(result.centerX() - width * 0.5, result.centerY() - height * 0.5, width, height);
        scale *= clampedFitScale;
    }

    return clampRectInside(result, bounds);
}




EdgeMotionAffinity edgeMotionAffinityForRect(const Rect& rect, const Rect& bounds) {
    if (bounds.width <= 1.0 || bounds.height <= 1.0)
        return {};

    const double centerX = rect.centerX();
    const double centerY = rect.centerY();
    const double leftDistance = std::max(0.0, centerX - bounds.x);
    const double rightDistance = std::max(0.0, bounds.x + bounds.width - centerX);
    const double topDistance = std::max(0.0, centerY - bounds.y);
    const double bottomDistance = std::max(0.0, bounds.y + bounds.height - centerY);

    EdgeMotionAffinity affinity;
    affinity.verticalEdge = clampUnit(1.0 - std::min(leftDistance, rightDistance) / std::max(1.0, bounds.width * 0.24));
    affinity.horizontalEdge = clampUnit(1.0 - std::min(topDistance, bottomDistance) / std::max(1.0, bounds.height * 0.24));
    affinity.corner = affinity.verticalEdge * affinity.horizontalEdge;
    return affinity;
}

double edgeAwareMotionDistance2(const Rect& source, const Rect& target, const Rect& bounds) {
    const EdgeMotionAffinity affinity = edgeMotionAffinityForRect(source, bounds);
    const double             dx = target.centerX() - source.centerX();
    const double             dy = target.centerY() - source.centerY();
    const double             xWeight = 1.0 + affinity.verticalEdge * 7.0 + affinity.corner * 10.0;
    const double             yWeight = 1.0 + affinity.horizontalEdge * 7.0 + affinity.corner * 10.0;
    return dx * dx * xWeight + dy * dy * yWeight;
}

Rect inflateRect(const Rect& rect, double amountX, double amountY) {
    return makeRect(rect.x - amountX, rect.y - amountY, rect.width + amountX * 2.0, rect.height + amountY * 2.0);
}

bool rectsOverlap(const Rect& lhs, const Rect& rhs) {
    return lhs.x < rhs.x + rhs.width && lhs.x + lhs.width > rhs.x && lhs.y < rhs.y + rhs.height && lhs.y + lhs.height > rhs.y;
}


CBox toBox(const Rect& rect) {
    return {
        rect.x,
        rect.y,
        rect.width,
        rect.height,
    };
}

Rect rectToMonitorLocal(const Rect& rect, const PHLMONITOR& monitor) {
    if (!monitor)
        return rect;

    return makeRect(rect.x - monitor->m_position.x, rect.y - monitor->m_position.y, rect.width, rect.height);
}

double renderScaleForMonitor(const PHLMONITOR& monitor) {
    if (!monitor || monitor->m_scale <= 0.0)
        return 1.0;

    return monitor->m_scale;
}

Rect scaleRectForRender(const Rect& rect, const PHLMONITOR& monitor) {
    const double scale = renderScaleForMonitor(monitor);
    return makeRect(rect.x * scale, rect.y * scale, rect.width * scale, rect.height * scale);
}

Vector2D renderedWindowPosition(const PHLWINDOW& window, bool goal = false) {
    if (!window)
        return {};

    // Hyprland's realPosition is already expressed in global compositor coordinates.
    // Adding workspace render offsets or floating offsets here double-counts them and
    // pushes overview open/close geometry toward off-screen workspace animation space.
    return goal ? window->m_realPosition->goal() : window->m_realPosition->value();
}

Rect stateSnapshotGlobalRectForWindow(const PHLWINDOW& window, bool goal = false) {
    if (!window)
        return {};

    Vector2D position = renderedWindowPosition(window, goal);
    const Vector2D size = goal ? window->m_realSize->goal() : window->m_realSize->value();
    return makeRect(position.x, position.y, size.x, size.y);
}

Rect layoutAnchorGlobalRectForWindow(const PHLWINDOW& window, bool goal = false) {
    if (!window)
        return {};

    const Vector2D position = renderedWindowPosition(window, goal);
    const Vector2D size = goal ? window->m_realSize->goal() : window->m_realSize->value();
    return makeRect(position.x, position.y, size.x, size.y);
}

Rect sceneGlobalRectForWindow(const PHLWINDOW& window, bool goal = false) {
    if (!window)
        return {};

    Vector2D position = renderedWindowPosition(window, goal);
    if (window->m_workspace && !window->m_pinned)
        position += goal ? window->m_workspace->m_renderOffset->goal() : window->m_workspace->m_renderOffset->value();

    const Vector2D size = goal ? window->m_realSize->goal() : window->m_realSize->value();
    return makeRect(position.x, position.y, size.x, size.y);
}

Rect renderGlobalRectForWindow(const PHLWINDOW& window, bool goal = false) {
    Rect rect = sceneGlobalRectForWindow(window, goal);
    if (window && window->m_isFloating)
        rect = translateRect(rect, window->m_floatingOffset.x, window->m_floatingOffset.y);
    return rect;
}

Rect centeredSurfaceRectInLayoutBox(const CBox& layoutBox, const Rect& surfaceGlobal) {
    const double width = surfaceGlobal.width > 1.0 ? surfaceGlobal.width : layoutBox.width;
    const double height = surfaceGlobal.height > 1.0 ? surfaceGlobal.height : layoutBox.height;
    return makeRect(layoutBox.x + (layoutBox.width - width) * 0.5, layoutBox.y + (layoutBox.height - height) * 0.5, width, height);
}

Rect scrollingOverviewSourceGlobalRectForWindow(const PHLWINDOW& window, const Rect& fallbackGlobal) {
    if (!window)
        return fallbackGlobal;

    const auto target = window->layoutTarget();
    if (!target)
        return fallbackGlobal;

    const CBox targetBox = target->position();
    if (targetBox.width <= 1.0 || targetBox.height <= 1.0)
        return fallbackGlobal;

    if (target->floating())
        return fallbackGlobal;

    CBox layoutBox = targetBox;
    if (auto* scrolling = scrollingAlgorithmForWorkspace(window->m_workspace); scrolling) {
        if (const auto targetData = scrolling->dataFor(target); targetData && targetData->layoutBox.width > 1.0 && targetData->layoutBox.height > 1.0) {
            layoutBox = liveScrollingLayoutBoxForTarget(targetData->target, targetData->layoutBox);
        }
    }

    return centeredSurfaceRectInLayoutBox(layoutBox, fallbackGlobal);
}

std::optional<ScrollingOverviewGeometry> scrollingOverviewTapeRowGeometryForWindow(const PHLWINDOW& window, const Rect& fallbackGlobal, PHLWINDOW anchorWindow = {}) {
    if (!window || !window->m_workspace || !window->m_workspace->m_space)
        return std::nullopt;

    const auto target = window->layoutTarget();
    if (!target || target->floating())
        return std::nullopt;

    auto* const scrolling = scrollingAlgorithmForWorkspace(window->m_workspace);
    if (!scrolling || !scrolling->m_scrollingData || !scrolling->m_scrollingData->controller)
        return std::nullopt;
    const auto targetData = scrolling->dataFor(target);
    if (!targetData || targetData->layoutBox.width <= 1.0 || targetData->layoutBox.height <= 1.0)
        return std::nullopt;

    if (!anchorWindow || !anchorWindow->m_isMapped || anchorWindow->m_workspace != window->m_workspace || anchorWindow->m_pinned || isFloatingOverviewWindow(anchorWindow))
        anchorWindow = window;

    auto anchorTarget = anchorWindow ? anchorWindow->layoutTarget() : nullptr;
    if (!anchorTarget || anchorTarget->floating())
        anchorTarget = target;

    const auto anchorTargetData = scrolling->dataFor(anchorTarget);

    const CBox workAreaBox = window->m_workspace->m_space->workArea();
    if (workAreaBox.width <= 1.0 || workAreaBox.height <= 1.0)
        return std::nullopt;

    const auto* const controller = scrolling->m_scrollingData->controller.get();
    const bool        horizontal = controller->isPrimaryHorizontal();
    const Rect        baseGlobal = makeRect(workAreaBox.x, workAreaBox.y, workAreaBox.width, workAreaBox.height);

    struct ColumnEntry {
        SP<Layout::Tiled::SColumnData> column;
        Rect                           bounds;
        Rect                           virtualBounds;
        double                         primary = 0.0;
    };

    std::vector<ColumnEntry> columns;
    columns.reserve(scrolling->m_scrollingData->columns.size());
    for (const auto& column : scrolling->m_scrollingData->columns) {
        if (!column || column->targetDatas.empty())
            continue;

        Rect columnBounds{};
        bool hasColumnBounds = false;
        for (const auto& candidate : column->targetDatas) {
            if (!scrollingTargetDataBelongsToWorkspace(candidate, window->m_workspace) || candidate->layoutBox.width <= 1.0 || candidate->layoutBox.height <= 1.0)
                continue;

            const CBox candidateLayoutBox = liveScrollingLayoutBoxForTarget(candidate->target, candidate->layoutBox);
            if (candidateLayoutBox.width <= 1.0 || candidateLayoutBox.height <= 1.0)
                continue;

            const Rect candidateBounds = makeRect(candidateLayoutBox.x, candidateLayoutBox.y, candidateLayoutBox.width, candidateLayoutBox.height);
            if (!hasColumnBounds) {
                columnBounds = candidateBounds;
                hasColumnBounds = true;
            } else {
                const double minX = std::min(columnBounds.x, candidateBounds.x);
                const double minY = std::min(columnBounds.y, candidateBounds.y);
                const double maxX = std::max(columnBounds.x + columnBounds.width, candidateBounds.x + candidateBounds.width);
                const double maxY = std::max(columnBounds.y + columnBounds.height, candidateBounds.y + candidateBounds.height);
                columnBounds = makeRect(minX, minY, maxX - minX, maxY - minY);
            }
        }

        if (!hasColumnBounds)
            continue;

        columns.push_back({
            .column = column,
            .bounds = columnBounds,
            .virtualBounds = columnBounds,
            .primary = horizontal ? columnBounds.x : columnBounds.y,
        });
    }

    if (columns.empty())
        return std::nullopt;

    if (columns.size() > 1)
        std::stable_sort(columns.begin(), columns.end(), [](const ColumnEntry& lhs, const ColumnEntry& rhs) { return lhs.primary < rhs.primary; });

    std::optional<std::size_t> targetColumnIndex;
    std::optional<std::size_t> anchorColumnIndex;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        for (const auto& candidate : columns[index].column->targetDatas) {
            if (!scrollingTargetDataBelongsToWorkspace(candidate, window->m_workspace))
                continue;
            if (candidate == targetData)
                targetColumnIndex = index;
            if (anchorTargetData && candidate == anchorTargetData)
                anchorColumnIndex = index;
        }
    }

    if (!targetColumnIndex)
        return std::nullopt;
    if (!anchorColumnIndex)
        anchorColumnIndex = targetColumnIndex;

    const double configuredGap = static_cast<double>(std::max(0L, getConfigInt(nullptr, "general:gaps_in", 0)));
    double       gap = configuredGap;
    if (gap <= 0.0 && columns.size() > 1) {
        std::vector<double> positiveGaps;
        positiveGaps.reserve(columns.size() - 1);
        for (std::size_t index = 1; index < columns.size(); ++index) {
            const Rect& previous = columns[index - 1].bounds;
            const Rect& current = columns[index].bounds;
            const double previousEnd = horizontal ? previous.x + previous.width : previous.y + previous.height;
            const double currentStart = horizontal ? current.x : current.y;
            const double candidateGap = currentStart - previousEnd;
            if (candidateGap > 0.0)
                positiveGaps.push_back(candidateGap);
        }
        if (!positiveGaps.empty()) {
            std::sort(positiveGaps.begin(), positiveGaps.end());
            gap = positiveGaps[positiveGaps.size() / 2];
        }
    }

    const auto primarySize = [&](const Rect& rect) { return horizontal ? rect.width : rect.height; };
    const auto setPrimaryStart = [&](Rect& rect, double start) {
        if (horizontal)
            rect.x = start;
        else
            rect.y = start;
    };
    const auto primaryStart = [&](const Rect& rect) { return horizontal ? rect.x : rect.y; };
    const auto primaryEnd = [&](const Rect& rect) { return primaryStart(rect) + primarySize(rect); };
    const bool fitFocusMethod = getConfigInt(nullptr, "scrolling:focus_fit_method", 0) == 1;
    const bool liveCameraOwnsGeometry = scrollingLiveCameraOwnsOverviewGeometry(scrolling);

    if (liveCameraOwnsGeometry) {
        // Hyprland is still animating the scroll camera or is doing its edge
        // camera-only pan past the first/last column. Do not synthesize a
        // centered/fit virtual tape here; that fights Hyprland's animated
        // target boxes and causes the overview to hang then snap.
    } else if (!fitFocusMethod) {
        const double anchorCenter = horizontal ? baseGlobal.centerX() : baseGlobal.centerY();
        setPrimaryStart(columns[*anchorColumnIndex].virtualBounds, anchorCenter - primarySize(columns[*anchorColumnIndex].bounds) * 0.5);

        if (*anchorColumnIndex > 0) {
            for (std::size_t index = *anchorColumnIndex; index > 0; --index) {
                const std::size_t leftIndex = index - 1;
                const double start = primaryStart(columns[index].virtualBounds) - gap - primarySize(columns[leftIndex].bounds);
                setPrimaryStart(columns[leftIndex].virtualBounds, start);
            }
        }

        for (std::size_t index = *anchorColumnIndex + 1; index < columns.size(); ++index) {
            const Rect& previous = columns[index - 1].virtualBounds;
            const double start = primaryStart(previous) + primarySize(previous) + gap;
            setPrimaryStart(columns[index].virtualBounds, start);
        }
    } else {
        const double viewportStart = primaryStart(baseGlobal);
        const double viewportEnd = primaryEnd(baseGlobal);
        const double viewportSize = primarySize(baseGlobal);
        constexpr double EDGE_EPSILON = 0.5;

        Rect visibleBounds{};
        bool hasVisibleBounds = false;
        bool hasPartialVisibleColumn = false;
        const auto expandVisibleBounds = [&](const Rect& rect) {
            if (!hasVisibleBounds) {
                visibleBounds = rect;
                hasVisibleBounds = true;
                return;
            }

            const double minX = std::min(visibleBounds.x, rect.x);
            const double minY = std::min(visibleBounds.y, rect.y);
            const double maxX = std::max(visibleBounds.x + visibleBounds.width, rect.x + rect.width);
            const double maxY = std::max(visibleBounds.y + visibleBounds.height, rect.y + rect.height);
            visibleBounds = makeRect(minX, minY, maxX - minX, maxY - minY);
        };

        for (const auto& column : columns) {
            const double start = primaryStart(column.virtualBounds);
            const double end = primaryEnd(column.virtualBounds);
            if (start >= viewportEnd - EDGE_EPSILON || end <= viewportStart + EDGE_EPSILON)
                continue;

            expandVisibleBounds(column.virtualBounds);
            if (start < viewportStart - EDGE_EPSILON || end > viewportEnd + EDGE_EPSILON)
                hasPartialVisibleColumn = true;
        }

        if (hasVisibleBounds && hasPartialVisibleColumn && primarySize(visibleBounds) <= viewportSize + EDGE_EPSILON) {
            const double minShift = viewportStart - primaryStart(visibleBounds);
            const double maxShift = viewportEnd - primaryEnd(visibleBounds);
            const double shift = minShift <= maxShift ? std::clamp(0.0, minShift, maxShift) : (minShift + maxShift) * 0.5;
            if (std::abs(shift) > EDGE_EPSILON) {
                for (auto& column : columns)
                    setPrimaryStart(column.virtualBounds, primaryStart(column.virtualBounds) + shift);
            }
        }
    }

    std::optional<Rect> targetSource;
    std::optional<Rect> targetAnchor;
    const auto& targetColumn = columns[*targetColumnIndex];
    for (const auto& candidate : targetColumn.column->targetDatas) {
        if (!scrollingTargetDataBelongsToWorkspace(candidate, window->m_workspace) || candidate->layoutBox.width <= 1.0 || candidate->layoutBox.height <= 1.0)
            continue;

        if (candidate != targetData)
            continue;

        const CBox candidateLayoutBox = liveScrollingLayoutBoxForTarget(candidate->target, candidate->layoutBox);
        Rect virtualCandidateLayoutBox = makeRect(candidateLayoutBox.x, candidateLayoutBox.y, candidateLayoutBox.width, candidateLayoutBox.height);
        const double candidateOffset = primaryStart(virtualCandidateLayoutBox) - primaryStart(targetColumn.bounds);
        setPrimaryStart(virtualCandidateLayoutBox, primaryStart(targetColumn.virtualBounds) + candidateOffset);

        CBox virtualBox{virtualCandidateLayoutBox.x, virtualCandidateLayoutBox.y, virtualCandidateLayoutBox.width, virtualCandidateLayoutBox.height};
        targetSource = centeredSurfaceRectInLayoutBox(virtualBox, fallbackGlobal);

        double anchorX = targetColumn.virtualBounds.centerX();
        double anchorY = targetColumn.virtualBounds.centerY();
        if (liveCameraOwnsGeometry) {
            anchorX = baseGlobal.centerX();
            anchorY = baseGlobal.centerY();
        }
        targetAnchor = makeRect(anchorX - targetSource->width * 0.5, anchorY - targetSource->height * 0.5, targetSource->width, targetSource->height);
        break;
    }

    if (!targetSource)
        return std::nullopt;

    return ScrollingOverviewGeometry{
        .sourceGlobal = *targetSource,
        .anchorGlobal = targetAnchor.value_or(*targetSource),
        .baseGlobal = baseGlobal,
        .primaryAxis = horizontal ? GestureAxis::Horizontal : GestureAxis::Vertical,
    };
}

Rect floatingOverviewSourceGlobalRectForWindow(const PHLWINDOW& window, const Rect& fallbackGlobal) {
    if (!window)
        return fallbackGlobal;

    if (!isFloatingOverviewWindow(window))
        return fallbackGlobal;

    // Floating layout positions are not part of the scrolling tape. Anchor the
    // overview preview from the live compositor rect so a window on the right
    // side of the workspace remains on the right after the workspace-scale map.
    return fallbackGlobal;
}

Rect niriFloatingOverviewBaseGlobalRect(const PHLMONITOR& monitor) {
    if (!monitor)
        return {};

    CBox box = monitor->logicalBoxMinusReserved();
    if (box.width <= 1.0 || box.height <= 1.0)
        box = CBox{monitor->m_position.x, monitor->m_position.y, monitor->m_size.x, monitor->m_size.y};

    return makeRect(box.x, box.y, box.width, box.height);
}

bool workspaceRowsEnabled(HANDLE handle) {
    return getConfigInt(handle, "plugin:hymission:one_workspace_per_row", 0) != 0;
}

} // namespace

namespace niri_scrolling_detail {

bool stripSnapshotSingleWorkspaceOnly = false;
std::chrono::steady_clock::time_point workspaceSwitchDispatcherBlockUntil;
std::chrono::steady_clock::time_point overviewOpenInputBlockUntil;
std::chrono::steady_clock::time_point overviewHeavyEditInputBlockUntil;
bool workspaceSwitchDispatcherBlockRelayout = false;

namespace {

struct TwoColumnSwapTraceState {
    std::chrono::steady_clock::time_point until;
    std::size_t                          remainingPreviewLogs = 384;
};

struct PendingTwoColumnSwapRepairState {
    std::chrono::steady_clock::time_point until;
};

std::unordered_map<const void*, TwoColumnSwapTraceState>     g_twoColumnSwapTraceStates;
std::unordered_map<const void*, PendingTwoColumnSwapRepairState> g_pendingTwoColumnSwapRepairs;

struct RetainedDirectNiriWorkspaceLane {
    WORKSPACEID workspaceId = WORKSPACE_INVALID;
    std::string workspaceName;
};

std::unordered_map<MONITORID, std::vector<RetainedDirectNiriWorkspaceLane>> g_retainedDirectNiriWorkspaceLanes;

void retainDirectNiriWorkspaceLaneId(const PHLMONITOR& monitor, WORKSPACEID workspaceId, std::string workspaceName) {
    if (!monitor || workspaceId == WORKSPACE_INVALID)
        return;

    auto& lanes = g_retainedDirectNiriWorkspaceLanes[monitor->m_id];
    const auto it = std::find_if(lanes.begin(), lanes.end(), [&](const RetainedDirectNiriWorkspaceLane& lane) {
        return lane.workspaceId == workspaceId;
    });

    if (it != lanes.end()) {
        if (!workspaceName.empty())
            it->workspaceName = std::move(workspaceName);
        return;
    }

    lanes.push_back({
        .workspaceId = workspaceId,
        .workspaceName = std::move(workspaceName),
    });
}

void retainDirectNiriWorkspaceLane(const PHLMONITOR& monitor, const PHLWORKSPACE& workspace) {
    if (!workspace || workspace->m_isSpecialWorkspace)
        return;

    retainDirectNiriWorkspaceLaneId(monitor, workspace->m_id, workspace->m_name);
}

std::vector<WORKSPACEID> retainedDirectNiriWorkspaceLaneIds(MONITORID monitorId) {
    std::vector<WORKSPACEID> ids;
    const auto it = g_retainedDirectNiriWorkspaceLanes.find(monitorId);
    if (it == g_retainedDirectNiriWorkspaceLanes.end())
        return ids;

    auto& lanes = it->second;
    lanes.erase(std::remove_if(lanes.begin(), lanes.end(), [](const RetainedDirectNiriWorkspaceLane& lane) {
                    return lane.workspaceId == WORKSPACE_INVALID;
                }),
                lanes.end());

    ids.reserve(lanes.size());
    for (const auto& lane : lanes)
        ids.push_back(lane.workspaceId);

    if (lanes.empty())
        g_retainedDirectNiriWorkspaceLanes.erase(it);

    return ids;
}

void clearRetainedDirectNiriWorkspaceLanes() {
    g_retainedDirectNiriWorkspaceLanes.clear();
}

const void* workspaceIdentityKey(const PHLWORKSPACE& workspace) {
    return workspace ? workspace.get() : nullptr;
}


} // namespace

void retainDirectNiriWorkspaceLaneForDrag(const PHLMONITOR& monitor, const PHLWORKSPACE& workspace) {
    retainDirectNiriWorkspaceLane(monitor, workspace);
}

bool overviewOpenInputBarrierActive() {
    return overviewOpenInputBlockUntil != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() < overviewOpenInputBlockUntil;
}

bool overviewHeavyEditInputBarrierActive() {
    return overviewHeavyEditInputBlockUntil != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() < overviewHeavyEditInputBlockUntil;
}

void clearExpiredOverviewOpenInputBarrier() {
    if (overviewOpenInputBlockUntil != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= overviewOpenInputBlockUntil)
        overviewOpenInputBlockUntil = {};
}

void clearExpiredOverviewHeavyEditInputBarrier() {
    if (overviewHeavyEditInputBlockUntil != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= overviewHeavyEditInputBlockUntil)
        overviewHeavyEditInputBlockUntil = {};
}

SP<Hyprutils::Animation::SAnimationPropertyConfig> windowsMoveAnimationConfig() {
    const auto& tree = Config::animationTree();
    if (!tree)
        return {};

    return tree->getAnimationPropertyConfig("windowsMove");
}

SP<Hyprutils::Animation::SAnimationPropertyConfig> workspaceAnimationConfig() {
    const auto& tree = Config::animationTree();
    if (!tree)
        return {};

    return tree->getAnimationPropertyConfig("workspaces");
}

bool shouldWrapWorkspaceIds(const WORKSPACEID targetId, const WORKSPACEID currentId) {
    static auto PWORKSPACEWRAPAROUND = CConfigValue<Hyprlang::INT>("animations:workspace_wraparound");

    if (!*PWORKSPACEWRAPAROUND)
        return false;

    WORKSPACEID lowestID = INT64_MAX;
    WORKSPACEID highestID = INT64_MIN;

    for (const auto& workspace : g_pCompositor->getWorkspaces()) {
        if (!workspace || workspace->m_id < 0 || workspace->m_isSpecialWorkspace)
            continue;

        lowestID = std::min(lowestID, workspace->m_id);
        highestID = std::max(highestID, workspace->m_id);
    }

    return std::min(targetId, currentId) == lowestID && std::max(targetId, currentId) == highestID;
}

void armTwoColumnSwapTrace(const PHLWORKSPACE& workspace) {
    const void* const key = workspaceIdentityKey(workspace);
    if (!key)
        return;

    g_twoColumnSwapTraceStates[key] = TwoColumnSwapTraceState{
        .until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500),
        .remainingPreviewLogs = 384,
    };
}

bool twoColumnSwapTraceActive(const PHLWORKSPACE& workspace) {
    const void* const key = workspaceIdentityKey(workspace);
    if (!key)
        return false;

    const auto it = g_twoColumnSwapTraceStates.find(key);
    if (it == g_twoColumnSwapTraceStates.end())
        return false;

    if (std::chrono::steady_clock::now() > it->second.until) {
        g_twoColumnSwapTraceStates.erase(it);
        return false;
    }

    return true;
}

bool consumeTwoColumnSwapPreviewTrace(const PHLWORKSPACE& workspace) {
    const void* const key = workspaceIdentityKey(workspace);
    if (!key)
        return false;

    const auto it = g_twoColumnSwapTraceStates.find(key);
    if (it == g_twoColumnSwapTraceStates.end())
        return false;

    if (std::chrono::steady_clock::now() > it->second.until) {
        g_twoColumnSwapTraceStates.erase(it);
        return false;
    }

    if (it->second.remainingPreviewLogs == 0)
        return false;

    --it->second.remainingPreviewLogs;
    return true;
}

void armPendingTwoColumnSwapRepair(const PHLWORKSPACE& workspace) {
    const void* const key = workspaceIdentityKey(workspace);
    if (!key)
        return;

    g_pendingTwoColumnSwapRepairs[key] = PendingTwoColumnSwapRepairState{
        .until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000),
    };
}

void clearPendingTwoColumnSwapRepair(const PHLWORKSPACE& workspace) {
    const void* const key = workspaceIdentityKey(workspace);
    if (!key)
        return;

    g_pendingTwoColumnSwapRepairs.erase(key);
}

bool consumePendingTwoColumnSwapRepair(const PHLWORKSPACE& workspace) {
    const void* const key = workspaceIdentityKey(workspace);
    if (!key)
        return false;

    const auto it = g_pendingTwoColumnSwapRepairs.find(key);
    if (it == g_pendingTwoColumnSwapRepairs.end())
        return false;

    const bool active = std::chrono::steady_clock::now() <= it->second.until;
    g_pendingTwoColumnSwapRepairs.erase(it);
    return active;
}

} // namespace niri_scrolling_detail

using niri_scrolling_detail::armTwoColumnSwapTrace;
using niri_scrolling_detail::armPendingTwoColumnSwapRepair;
using niri_scrolling_detail::clearExpiredOverviewHeavyEditInputBarrier;
using niri_scrolling_detail::clearPendingTwoColumnSwapRepair;
using niri_scrolling_detail::consumePendingTwoColumnSwapRepair;
using niri_scrolling_detail::clearExpiredOverviewOpenInputBarrier;
using niri_scrolling_detail::consumeTwoColumnSwapPreviewTrace;
using niri_scrolling_detail::overviewOpenInputBarrierActive;
using niri_scrolling_detail::overviewHeavyEditInputBarrierActive;
using niri_scrolling_detail::shouldWrapWorkspaceIds;
using niri_scrolling_detail::twoColumnSwapTraceActive;

// Exact-mode OverviewController member implementations are kept below.

bool OverviewController::niriModeEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:niri_mode", 0) != 0;
}
bool OverviewController::niriModeAppliesToState(const State& state) const {
    if (!niriModeEnabled() || !state.collectionPolicy.onlyActiveWorkspace)
        return false;

    if (state.ownerWorkspace && isScrollingWorkspace(state.ownerWorkspace))
        return true;

    if (state.focusDuringOverview && !state.focusDuringOverview->m_pinned && state.focusDuringOverview->m_workspace &&
        isScrollingWorkspace(state.focusDuringOverview->m_workspace))
        return true;

    return std::ranges::any_of(state.managedWorkspaces, [this](const PHLWORKSPACE& workspace) { return isScrollingWorkspace(workspace); });
}
double OverviewController::niriScrollPixelsPerDelta() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:niri_scroll_pixels_per_delta", 1.0), 0.0, 20.0);
}
double OverviewController::niriLayoutScale() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:niri_layout_scale", 1.0), 0.50, 2.0);
}
double OverviewController::niriOverviewScale() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:niri_overview_scale", 0.65), 0.05, 1.0);
}
double OverviewController::niriWindowGaps() const {
    const double gapsIn = static_cast<double>(std::max(0L, getConfigInt(m_handle, "general:gaps_in", 0)));
    const double configured = getConfigFloat(m_handle, "plugin:hymission:niri_window_gaps", -1.0);
    if (configured >= 0.0)
        return std::clamp(configured, 0.0, 160.0);

    const double legacyPixels = getConfigFloat(m_handle, "plugin:hymission:niri_single_ws_gap_pixels", -1.0);
    if (legacyPixels >= 0.0)
        return std::clamp(legacyPixels, 0.0, 160.0);

    const double legacyMultiplier = getConfigFloat(m_handle, "plugin:hymission:niri_single_ws_gap_multiplier", -1.0);
    if (legacyMultiplier >= 0.0)
        return std::clamp(gapsIn * std::max(0.0, std::clamp(legacyMultiplier, 1.0, 8.0) - 1.0), 0.0, 160.0);

    return std::clamp(gapsIn, 0.0, 160.0);
}
double OverviewController::niriWindowGapsForWorkspace(const PHLWORKSPACE& workspace, GestureAxis axis) const {
    const double configured = getConfigFloat(m_handle, "plugin:hymission:niri_window_gaps", -1.0);
    if (configured >= 0.0)
        return std::clamp(configured, 0.0, 160.0);

    const double legacyPixels = getConfigFloat(m_handle, "plugin:hymission:niri_single_ws_gap_pixels", -1.0);
    if (legacyPixels >= 0.0)
        return std::clamp(legacyPixels, 0.0, 160.0);

    const double legacyMultiplier = getConfigFloat(m_handle, "plugin:hymission:niri_single_ws_gap_multiplier", -1.0);
    if (legacyMultiplier >= 0.0)
        return niriWindowGaps();

    if (workspace) {
        const auto workspaceRule = Config::workspaceRuleMgr()->getWorkspaceRuleFor(workspace).value_or(Config::CWorkspaceRule{});
        if (workspaceRule.m_gapsIn) {
            const auto& gaps = *workspaceRule.m_gapsIn;
            const double axisGap =
                axis == GestureAxis::Horizontal ? (static_cast<double>(gaps.m_left) + static_cast<double>(gaps.m_right)) * 0.5 :
                                                  (static_cast<double>(gaps.m_top) + static_cast<double>(gaps.m_bottom)) * 0.5;
            return std::clamp(axisGap, 0.0, 160.0);
        }
    }

    return niriWindowGaps();
}
double OverviewController::niriMultiWorkspaceScale() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:niri_multi_ws_scale", 0.18), 0.05, 0.24);
}
double OverviewController::niriWorkspaceGap() const {
    const double fallback = static_cast<double>(getConfigInt(m_handle, "general:gaps_out", 0));
    double configured = getConfigFloat(m_handle, "plugin:hymission:niri_workspace_gap", -1.0);
    if (configured < 0.0)
        configured = getConfigFloat(m_handle, "plugin:hymission:niri_multi_ws_gap", -1.0);
    return configured < 0.0 ? std::max(0.0, fallback) : std::max(0.0, configured);
}
double OverviewController::niriWorkspaceScale() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:niri_workspace_scale", 1.0), 0.05, 1.0);
}
bool OverviewController::niriModeShowEmptyWorkspacesBetweenEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:niri_mode_show_empty_workspaces_btwn", 1) != 0;
}
bool OverviewController::niriModeWallpaperZoomEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:niri_mode_wallpaper_zoom", 0) != 0;
}
CHyprColor OverviewController::niriModeWallpaperZoomBackgroundColor() const {
    constexpr uint64_t FALLBACK_COLOR = 0xFF0D0F14;
    const auto configured =
        getConfigString(m_handle, "plugin:hymission:niri_mode_wallpaper_zoom_background_color", "#0D0F14FF");
    const auto parsed = Config::ParserUtils::parseColor(configured);
    return CHyprColor(static_cast<uint64_t>(parsed.value_or(FALLBACK_COLOR)));
}
std::string OverviewController::niriModeWallpaperZoomLayerNamespaces() const {
    return getConfigString(m_handle, "plugin:hymission:niri_mode_wallpaper_zoom_layer_namespaces", "awww-daemon");
}
std::chrono::milliseconds OverviewController::niriModeWallpaperZoomLayerRefreshInterval() const {
    const long configured = getConfigInt(m_handle, "plugin:hymission:niri_mode_wallpaper_zoom_layer_refresh_ms", 100);
    return configured <= 0 ? std::chrono::milliseconds{0} : std::chrono::milliseconds{std::clamp<long>(configured, 16, 5000)};
}
bool OverviewController::niriWallpaperZoomAppliesToState(const State& state) const {
    return niriModeWallpaperZoomEnabled() && niriModeAppliesToState(state) && state.collectionPolicy.onlyActiveWorkspace;
}
bool OverviewController::niriWallpaperZoomAppliesToMonitor(const State& state, const PHLMONITOR& monitor) const {
    if (!monitor || !niriWallpaperZoomAppliesToState(state))
        return false;

    const auto isScrollingWorkspaceOnMonitor = [&](const PHLWORKSPACE& workspace) {
        return workspace && workspace->m_monitor.lock() == monitor && isScrollingWorkspace(workspace);
    };
    if (isScrollingWorkspaceOnMonitor(state.ownerWorkspace))
        return true;
    if (state.focusDuringOverview && !state.focusDuringOverview->m_pinned &&
        isScrollingWorkspaceOnMonitor(state.focusDuringOverview->m_workspace))
        return true;

    return std::ranges::any_of(state.managedWorkspaces, isScrollingWorkspaceOnMonitor);
}
bool OverviewController::niriPreviewDisabled() const {
    return getConfigInt(m_handle, "plugin:hymission:niri_preview_disabled", 0) != 0;
}
bool OverviewController::niriOverviewAnimationsEnabled() const {
    const bool pluginAnimationsEnabled = getConfigInt(m_handle, "plugin:hymission:niri_overview_animations", 1) != 0;
    if (!pluginAnimationsEnabled)
        return false;

    if (getConfigInt(m_handle, "animations:enabled", 1) != 0)
        return true;

    return m_animationsEnabledOverridden && m_animationsEnabledBackup != 0;
}
double OverviewController::niriOverviewOpenCloseSpeedMultiplier() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:niri_overview_open_close_speed_multiplier", 1.5), 0.1, 10.0);
}
PHLWORKSPACE OverviewController::activeLayoutWorkspace() const {
    const auto workspaceOnMonitor = [](const PHLWORKSPACE& workspace, const PHLMONITOR& monitor) {
        return workspace && monitor && workspace->m_monitor.lock() == monitor;
    };
    const auto scrollingWorkspaceOnMonitor = [&](const PHLWORKSPACE& workspace, const PHLMONITOR& monitor) {
        return workspaceOnMonitor(workspace, monitor) && isScrollingWorkspace(workspace);
    };

    if (isVisible() && niriModeAppliesToState(m_state)) {
        PHLMONITOR ownerMonitor = m_state.ownerMonitor;
        if (!ownerMonitor)
            ownerMonitor = g_pCompositor->getMonitorFromCursor();

        // When the overview is launched from a layer surface (for example Waybar),
        // Hyprland's focused monitor/window can still belong to another monitor for
        // the first few frames.  In direct single-workspace Niri mode, the overview
        // geometry must stay tied to the monitor that opened it; otherwise an empty
        // scrolling workspace can borrow a foreign dwindle workspace and feed that
        // workspace into snapshot/render paths that expect the owner monitor.
        if (const auto centeredEmptyWorkspace = centeredEmptyPlaceholderWorkspace(m_state, ownerMonitor);
            scrollingWorkspaceOnMonitor(centeredEmptyWorkspace, ownerMonitor))
            return centeredEmptyWorkspace;

        if (m_state.collectionPolicy.onlyActiveWorkspace) {
            if (scrollingWorkspaceOnMonitor(m_state.ownerWorkspace, ownerMonitor))
                return m_state.ownerWorkspace;

            if (ownerMonitor && scrollingWorkspaceOnMonitor(ownerMonitor->m_activeWorkspace, ownerMonitor))
                return ownerMonitor->m_activeWorkspace;
        }

        if (m_state.focusDuringOverview && !m_state.focusDuringOverview->m_pinned && m_state.focusDuringOverview->m_workspace &&
            (!ownerMonitor || workspaceOnMonitor(m_state.focusDuringOverview->m_workspace, ownerMonitor)))
            return m_state.focusDuringOverview->m_workspace;

        if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size()) {
            const auto selected = m_state.windows[*m_state.selectedIndex].window;
            if (selected && !selected->m_pinned && selected->m_workspace && (!ownerMonitor || workspaceOnMonitor(selected->m_workspace, ownerMonitor)))
                return selected->m_workspace;
        }

        if (ownerMonitor) {
            if (ownerMonitor->m_activeSpecialWorkspace)
                return ownerMonitor->m_activeSpecialWorkspace;
            return ownerMonitor->m_activeWorkspace;
        }
    }

    PHLMONITOR monitor = Desktop::focusState()->monitor();
    if (!monitor)
        monitor = m_state.ownerMonitor;
    if (!monitor)
        monitor = g_pCompositor->getMonitorFromCursor();
    if (!monitor)
        return {};

    if (monitor->m_activeSpecialWorkspace)
        return monitor->m_activeSpecialWorkspace;

    return monitor->m_activeWorkspace;
}
bool OverviewController::isScrollingWorkspace(const PHLWORKSPACE& workspace) const {
    if (!workspace || !workspace->m_space)
        return false;

    const auto algorithm = workspace->m_space->algorithm();
    if (!algorithm || !algorithm->tiledAlgo())
        return false;

    return Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(&typeid(*algorithm->tiledAlgo())) == "scrolling";
}
bool OverviewController::hasScrollingWorkspace() const {
    return std::ranges::any_of(m_state.managedWorkspaces, [this](const PHLWORKSPACE& workspace) { return isScrollingWorkspace(workspace); });
}
GestureAxis OverviewController::gestureAxisForDirection(eTrackpadGestureDirection direction) const {
    switch (direction) {
        case TRACKPAD_GESTURE_DIR_UP:
        case TRACKPAD_GESTURE_DIR_DOWN:
        case TRACKPAD_GESTURE_DIR_VERTICAL:
            return GestureAxis::Vertical;
        case TRACKPAD_GESTURE_DIR_LEFT:
        case TRACKPAD_GESTURE_DIR_RIGHT:
        case TRACKPAD_GESTURE_DIR_HORIZONTAL:
        case TRACKPAD_GESTURE_DIR_SWIPE:
        default:
            return GestureAxis::Horizontal;
    }
}
ScrollingLayoutDirection OverviewController::scrollingLayoutDirection() const {
    std::string direction = getConfigString(m_handle, "scrolling:direction", "right");

    if (const auto workspace = activeLayoutWorkspace(); workspace) {
        const auto workspaceRule = Config::workspaceRuleMgr()->getWorkspaceRuleFor(workspace).value_or(Config::CWorkspaceRule{});
        if (workspaceRule.m_layoutopts.contains("direction") && !workspaceRule.m_layoutopts.at("direction").empty())
            direction = workspaceRule.m_layoutopts.at("direction");
    }

    return parseScrollingLayoutDirection(direction);
}
bool OverviewController::canScrollActiveLayoutWithGesture(eTrackpadGestureDirection direction) const {
    return scrollingLayoutGestureAxisMatches(scrollingLayoutDirection(), gestureAxisForDirection(direction));
}
double OverviewController::scrollLayoutPixelsPerGestureDelta(ScrollingLayoutDirection direction) const {
    const double swipeDistance = gestureSwipeDistance();
    if (swipeDistance <= 0.0)
        return std::max(0.0, niriScrollPixelsPerDelta());

    double viewportLength = swipeDistance;
    if (const auto monitor = Desktop::focusState()->monitor(); monitor)
        viewportLength = axisForScrollingLayoutDirection(direction) == GestureAxis::Vertical ? static_cast<double>(monitor->m_size.y) :
                                                                                               static_cast<double>(monitor->m_size.x);

    return std::max(0.0, niriScrollPixelsPerDelta()) * std::max(1.0, viewportLength) / swipeDistance;
}
double OverviewController::scrollLayoutPrimaryDelta(const IPointer::SSwipeUpdateEvent& event, eTrackpadGestureDirection direction, float deltaScale) const {
    bool vertical = false;
    switch (direction) {
        case TRACKPAD_GESTURE_DIR_UP:
        case TRACKPAD_GESTURE_DIR_DOWN:
        case TRACKPAD_GESTURE_DIR_VERTICAL:
            vertical = true;
            break;
        case TRACKPAD_GESTURE_DIR_SWIPE:
            vertical = std::abs(event.delta.y) > std::abs(event.delta.x);
            break;
        case TRACKPAD_GESTURE_DIR_LEFT:
        case TRACKPAD_GESTURE_DIR_RIGHT:
        case TRACKPAD_GESTURE_DIR_HORIZONTAL:
        default:
            vertical = false;
            break;
    }

    return static_cast<double>(vertical ? event.delta.y : event.delta.x) * static_cast<double>(deltaScale);
}
bool OverviewController::scrollActiveLayoutByGestureDelta(const IPointer::SSwipeUpdateEvent& event, eTrackpadGestureDirection direction, float deltaScale) {
    if (!canScrollActiveLayoutWithGesture(direction)) {
        if (debugLogsEnabled()) {
            const auto layoutDirection = scrollingLayoutDirection();
            std::ostringstream out;
            out << "[hymission] niri layout scroll skipped: axis mismatch gestureDir=" << trackpadDirectionName(direction)
                << " gestureAxis=" << gestureAxisName(gestureAxisForDirection(direction)) << " layoutDir=" << scrollingDirectionName(layoutDirection)
                << " layoutAxis=" << gestureAxisName(axisForScrollingLayoutDirection(layoutDirection));
            debugLog(out.str());
        }
        return false;
    }

    const auto workspace = activeLayoutWorkspace();
    auto* const scrolling = scrollingAlgorithmForWorkspace(workspace);
    if (!scrolling || !scrolling->m_scrollingData || !scrolling->m_scrollingData->controller) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] niri layout scroll skipped: scrolling algorithm unavailable workspace="
                << (workspace ? workspace->m_name : std::string{"<none>"});
            debugLog(out.str());
        }
        return false;
    }

    const auto scrollingDirection = scrollingLayoutDirection();
    const double primaryDelta = scrollLayoutPrimaryDelta(event, direction, deltaScale);
    const double amount = scrollingLayoutMoveAmount(scrollingDirection, primaryDelta, scrollLayoutPixelsPerGestureDelta(scrollingDirection));
    const bool traceMove = debugLogsEnabled() && m_scrollGestureSession.active && m_scrollGestureSession.debugSamples < 16;
    if (traceMove)
        ++m_scrollGestureSession.debugSamples;

    if (std::abs(amount) < 0.001) {
        if (traceMove) {
            std::ostringstream out;
            out << "[hymission] niri layout scroll delta too small dir=" << trackpadDirectionName(direction) << " delta=" << vectorToString(event.delta)
                << " primary=" << primaryDelta << " amount=" << amount;
            debugLog(out.str());
        }
        return true;
    }

    auto& data = scrolling->m_scrollingData;
    auto* const controller = data->controller.get();
    const CBox usable = scrolling->usableArea();
    const bool fullscreenOnOne = getConfigInt(m_handle, "scrolling:fullscreen_on_one_column", 1) != 0;
    const double viewportLength =
        axisForScrollingLayoutDirection(scrollingDirection) == GestureAxis::Vertical ? static_cast<double>(usable.h) : static_cast<double>(usable.w);
    const double maxExtent = controller->calculateMaxExtent(usable, fullscreenOnOne);
    const double maxOffset = std::max(0.0, maxExtent - std::max(1.0, viewportLength));
    const double offsetBefore = controller->getOffset();
    const double requestedOffset = offsetBefore - amount;
    const double offsetAfter = std::clamp(requestedOffset, 0.0, maxOffset);

    if (std::abs(offsetAfter - offsetBefore) >= 0.001) {
        controller->setOffset(offsetAfter);
        data->recalculate(true);
        if (g_pAnimationManager)
            g_pAnimationManager->frameTick();
    }

    if (traceMove) {
        std::ostringstream out;
        out << "[hymission] niri layout direct scroll dir=" << trackpadDirectionName(direction)
            << " layoutDir=" << scrollingDirectionName(scrollingDirection) << " delta=" << vectorToString(event.delta) << " primary=" << primaryDelta
            << " amount=" << amount << " offsetBefore=" << offsetBefore << " requested=" << requestedOffset << " offsetAfter=" << offsetAfter
            << " maxOffset=" << maxOffset << " maxExtent=" << maxExtent << " viewport=" << viewportLength
            << " result=" << (std::abs(offsetAfter - offsetBefore) >= 0.001 ? "moved" : "clamped");
        debugLog(out.str());
    }

    return true;
}
void OverviewController::refreshNiriScrollingOverviewAfterLayoutScroll(const char* source, const PreviewRectSnapshot* previousPreviewRects) {
    if (!isVisible() || (m_state.phase != Phase::Opening && m_state.phase != Phase::Active) || !niriModeAppliesToState(m_state) || !m_state.ownerMonitor ||
        !isScrollingWorkspace(activeLayoutWorkspace()))
        return;

    if (usesDirectNiriScrollingOverview(m_state) && !previousPreviewRects) {
        if (m_overviewEditingDispatcherInProgress) {
            damageOwnedMonitors();
            return;
        }
        if (insideRenderLifecycle()) {
            scheduleVisibleStateRebuild();
            damageOwnedMonitors();
            return;
        }

        PHLWINDOW preferred = Desktop::focusState()->window();
        const bool liveFocusValid = preferred && preferred->m_isMapped && !preferred->m_pinned && preferred->m_workspace &&
            m_state.ownerWorkspace && preferred->m_workspace == m_state.ownerWorkspace;
        if (directNiriEdgeCameraActive() && !liveFocusValid) {
            // Native scrolling layout intentionally drops focused_window when movecol
            // walks past the last column.  Preserve that no-focus edge-camera state
            // instead of falling back to the previous selected window.
            m_state.selectedIndex.reset();
            m_state.focusDuringOverview.reset();
            m_queuedOverviewSelectionTarget.reset();
            m_queuedOverviewSelectionSyncScrollingSpot = false;
            m_queuedOverviewSelectionCenterCursor = false;
            m_queuedOverviewLiveFocusTarget.reset();
            m_queuedOverviewLiveFocusSyncScrollingSpot = false;
            m_queuedOverviewLiveFocusCenterCursor = false;

            if (debugLogsEnabled())
                debugLog("[hymission] niri direct refresh preserved native edge-camera focus release");
        } else {
            if (!preferred || !preferred->m_isMapped)
                preferred = selectedWindow();
        }

        const auto preferredWorkspace = preferred && !preferred->m_pinned ? preferred->m_workspace : PHLWORKSPACE{};
        if (m_state.collectionPolicy.onlyActiveWorkspace && preferredWorkspace && isScrollingWorkspace(preferredWorkspace) &&
            preferredWorkspace != m_state.ownerWorkspace) {
            m_state.ownerWorkspace = preferredWorkspace;
            rebuildVisibleState(preferred, true);
            damageOwnedMonitors();
            return;
        }

        refreshVisibleStateMetadata(preferred);
        damageOwnedMonitors();
        return;
    }

    const auto workspace = activeLayoutWorkspace();
    auto* const scrolling = workspace ? scrollingAlgorithmForWorkspace(workspace) : nullptr;
    const std::size_t columnCount = scrolling && scrolling->m_scrollingData ? scrolling->m_scrollingData->columns.size() : 0;
    const std::string_view sourceView = source ? std::string_view{source} : std::string_view{};
    const bool traceColumnRefresh = debugLogsEnabled() && usesDirectNiriScrollingOverview(m_state) && columnCount >= 2 && columnCount <= 3 &&
        sourceView.find("opening-complete") == std::string_view::npos;
    if (traceColumnRefresh) {
        std::ostringstream out;
        out << "[hymission] niri refresh begin"
            << " source=" << (source ? source : "?")
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " columns=" << columnCount
            << " offset=" << (scrolling && scrolling->m_scrollingData && scrolling->m_scrollingData->controller ? scrolling->m_scrollingData->controller->getOffset() : 0.0)
            << " selected=" << debugWindowLabel(selectedWindow())
            << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview)
            << " relayoutActive=" << (m_state.relayoutActive ? 1 : 0);
        debugLog(out.str());
        logScrollingWorkspaceSpotState("niri-refresh-before", workspace, selectedWindow());
        if (columnCount == 2)
            armTwoColumnSwapTrace(workspace);
    }
    if (debugLogsEnabled() && usesDirectNiriScrollingOverview(m_state))
        logSwapColumnFollowupState("niri-refresh-begin", workspace, source, selectedWindow());

    const bool animateRefresh = usesDirectNiriScrollingOverview(m_state) && niriOverviewAnimationsEnabled();
    struct TwoColumnRefreshOrigin {
        Rect        rect;
        std::size_t groupIndex = 0;
    };
    struct TwoColumnRefreshGroup {
        SP<Layout::Tiled::SColumnData> column;
        Rect                           bounds;
        bool                           hasBounds = false;
        double                         previousPrimary = 0.0;
        double                         nextPrimary = 0.0;
        bool                           hasPreviousPrimary = false;
        bool                           hasNextPrimary = false;
        bool                           hasSizeChangingOrigin = false;
    };
    std::array<TwoColumnRefreshGroup, 2> refreshGroups{};
    std::unordered_map<PHLWINDOW, TwoColumnRefreshOrigin> refreshOrigins;
    const bool swapColumnFreezeActive = swapColumnBackendPreviewFreezeActiveFor(workspace);
    const bool captureTwoColumnRefresh = !previousPreviewRects && !swapColumnFreezeActive && usesDirectNiriScrollingOverview(m_state) && columnCount == 2 &&
        scrolling && scrolling->m_scrollingData && scrolling->m_scrollingData->columns.size() == 2;
    const bool forceSameFocusTwoColumnSwap = captureTwoColumnRefresh && consumePendingTwoColumnSwapRepair(workspace);
    const auto expandRefreshGroupBounds = [](TwoColumnRefreshGroup& group, const Rect& rect) {
        if (!group.hasBounds) {
            group.bounds = rect;
            group.hasBounds = true;
            return;
        }

        const double minX = std::min(group.bounds.x, rect.x);
        const double minY = std::min(group.bounds.y, rect.y);
        const double maxX = std::max(group.bounds.x + group.bounds.width, rect.x + rect.width);
        const double maxY = std::max(group.bounds.y + group.bounds.height, rect.y + rect.height);
        group.bounds = makeRect(minX, minY, maxX - minX, maxY - minY);
    };

    if (captureTwoColumnRefresh) {
        const bool horizontal =
            scrolling->m_scrollingData->controller ? scrolling->m_scrollingData->controller->isPrimaryHorizontal() : true;
        refreshGroups[0].column = scrolling->m_scrollingData->columns[0];
        refreshGroups[1].column = scrolling->m_scrollingData->columns[1];
        for (const auto& managed : m_state.windows) {
            const auto window = managed.window;
            if (!window || !window->m_isMapped || window->m_workspace != workspace || window->m_pinned || isFloatingOverviewWindow(window))
                continue;

            const auto target = window->layoutTarget();
            if (!target || target->floating())
                continue;

            const auto targetData = scrolling->dataFor(target);
            const auto column = targetData ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};
            if (!column)
                continue;

            for (std::size_t index = 0; index < refreshGroups.size(); ++index) {
                if (refreshGroups[index].column != column)
                    continue;

                const Rect rect = currentPreviewRect(managed);
                expandRefreshGroupBounds(refreshGroups[index], rect);
                if (std::abs(rect.width - managed.targetGlobal.width) > 1.0 || std::abs(rect.height - managed.targetGlobal.height) > 1.0)
                    refreshGroups[index].hasSizeChangingOrigin = true;
                const double previousPrimary = horizontal ? managed.naturalGlobal.centerX() : managed.naturalGlobal.centerY();
                if (!refreshGroups[index].hasPreviousPrimary || previousPrimary < refreshGroups[index].previousPrimary) {
                    refreshGroups[index].previousPrimary = previousPrimary;
                    refreshGroups[index].hasPreviousPrimary = true;
                }
                refreshOrigins.emplace(window, TwoColumnRefreshOrigin{.rect = rect, .groupIndex = index});
                break;
            }
        }
    }

    const bool forceFinalScrollingLayoutBox = previousPreviewRects && usesDirectNiriScrollingOverview(m_state) &&
        (sourceView.find("movecol-edge") != std::string_view::npos || sourceView.find("edge-release") != std::string_view::npos);

    State next;
    {
        std::optional<ScopedFlag> forceFinalLayoutBoxScope;
        if (forceFinalScrollingLayoutBox)
            forceFinalLayoutBoxScope.emplace(g_forceScrollingFinalLayoutBoxForOverview);

        next = buildState(m_state.ownerMonitor, m_state.collectionPolicy.requestedScope, {}, false, m_state.suppressWorkspaceStrip, m_state.focusDuringOverview);
    }
    if (next.windows.empty())
        return;
    const bool carriedFrozenSwapColumnLayout = carryFrozenSwapColumnBackendPreviewLayout(next, workspace, source);

    std::array<std::size_t, 2> refreshPreviousRankToGroup{0, 1};
    std::array<std::size_t, 2> refreshGroupToNextRank{0, 1};
    if (captureTwoColumnRefresh && refreshGroups[0].hasBounds && refreshGroups[1].hasBounds && refreshGroups[0].hasPreviousPrimary &&
        refreshGroups[1].hasPreviousPrimary) {
        const bool horizontal =
            scrolling->m_scrollingData->controller ? scrolling->m_scrollingData->controller->isPrimaryHorizontal() : true;
        for (const auto& nextManaged : next.windows) {
            const auto originIt = refreshOrigins.find(nextManaged.window);
            if (originIt == refreshOrigins.end())
                continue;

            auto& group = refreshGroups[originIt->second.groupIndex];
            const double nextPrimary = horizontal ? nextManaged.naturalGlobal.centerX() : nextManaged.naturalGlobal.centerY();
            if (!group.hasNextPrimary || nextPrimary < group.nextPrimary) {
                group.nextPrimary = nextPrimary;
                group.hasNextPrimary = true;
            }
        }

        if (refreshGroups[1].previousPrimary < refreshGroups[0].previousPrimary)
            std::swap(refreshPreviousRankToGroup[0], refreshPreviousRankToGroup[1]);

        std::array<std::size_t, 2> nextRankToGroup{0, 1};
        if (refreshGroups[1].hasNextPrimary && refreshGroups[1].nextPrimary < refreshGroups[0].nextPrimary)
            std::swap(nextRankToGroup[0], nextRankToGroup[1]);

        for (std::size_t rank = 0; rank < nextRankToGroup.size(); ++rank)
            refreshGroupToNextRank[nextRankToGroup[rank]] = rank;
    }

    std::size_t updated = 0;
    bool        targetChanged = false;
    const auto previousPreviewRectFor = [&](const PHLWINDOW& window, const ManagedWindow& fallback) {
        if (previousPreviewRects) {
            const auto previous = std::find_if(previousPreviewRects->begin(), previousPreviewRects->end(),
                                               [&](const auto& entry) { return entry.first == window; });
            if (previous != previousPreviewRects->end())
                return previous->second;
        }

        return currentPreviewRect(fallback);
    };
    m_state.slots.clear();
    for (auto& managed : m_state.windows) {
        auto it = std::find_if(next.windows.begin(), next.windows.end(), [&](const ManagedWindow& candidate) { return candidate.window == managed.window; });
        if (it == next.windows.end()) {
            m_state.slots.push_back(managed.slot);
            continue;
        }

        const Rect currentRect = previousPreviewRectFor(managed.window, managed);
        const Rect previousTarget = managed.targetGlobal;
        const Rect previousRelayoutFrom = managed.relayoutFromGlobal;
        managed.naturalGlobal = it->naturalGlobal;
        managed.slot = it->slot;
        managed.targetGlobal = it->targetGlobal;
        managed.relayoutFromGlobal = animateRefresh ? currentRect : managed.targetGlobal;
        managed.isNiriFloatingOverlay = it->isNiriFloatingOverlay;
        if (carriedFrozenSwapColumnLayout)
            (void)carryFrozenSwapColumnBackendPreviewLayout(managed, static_cast<std::size_t>(&managed - m_state.windows.data()), workspace);
        if (captureTwoColumnRefresh && refreshGroups[0].hasBounds && refreshGroups[1].hasBounds && refreshGroups[0].hasPreviousPrimary &&
            refreshGroups[1].hasPreviousPrimary && refreshGroups[0].hasNextPrimary && refreshGroups[1].hasNextPrimary && managed.window &&
            managed.window->m_workspace == workspace) {
            const auto originIt = refreshOrigins.find(managed.window);
            if (originIt != refreshOrigins.end()) {
                const std::size_t previousGroupIndex = originIt->second.groupIndex;
                const std::size_t previousRank = refreshPreviousRankToGroup[0] == previousGroupIndex ? 0 : 1;
                const std::size_t nextRank = refreshGroupToNextRank[previousGroupIndex];
                const bool sameFocusSwapRepairAllowed =
                    forceSameFocusTwoColumnSwap && !refreshGroups[0].hasSizeChangingOrigin && !refreshGroups[1].hasSizeChangingOrigin;
                if (nextRank != previousRank || sameFocusSwapRepairAllowed) {
                    const std::size_t targetGroupIndex = nextRank != previousRank ? refreshPreviousRankToGroup[nextRank] : (previousGroupIndex == 0 ? 1 : 0);
                    const Rect& fromGroup = refreshGroups[previousGroupIndex].bounds;
                    const Rect& toGroup = refreshGroups[targetGroupIndex].bounds;
                    const Rect targetRect = translateRect(originIt->second.rect, toGroup.centerX() - fromGroup.centerX(), toGroup.centerY() - fromGroup.centerY());
                    managed.relayoutFromGlobal = originIt->second.rect;
                    managed.targetGlobal = targetRect;
                    if (managed.targetMonitor) {
                        managed.slot.target = makeRect(targetRect.x - managed.targetMonitor->m_position.x, targetRect.y - managed.targetMonitor->m_position.y,
                                                       targetRect.width, targetRect.height);
                        if (managed.slot.natural.width > 1.0)
                            managed.slot.scale = targetRect.width / managed.slot.natural.width;
                        else if (managed.slot.natural.height > 1.0)
                            managed.slot.scale = targetRect.height / managed.slot.natural.height;
                    }
                    targetChanged = true;
                    if (traceColumnRefresh) {
                        std::ostringstream out;
                        out << "[hymission] niri refresh exact-two repair"
                            << " reason=" << (nextRank != previousRank ? "rank-change" : "same-focus")
                            << " window=" << debugWindowLabel(managed.window)
                            << " fromGroup=" << previousGroupIndex
                            << " toGroup=" << targetGroupIndex
                            << " sizeChangingOrigin=" << (refreshGroups[previousGroupIndex].hasSizeChangingOrigin ? 1 : 0)
                            << " previousRank=" << previousRank
                            << " nextRank=" << nextRank
                            << " from=" << rectToString(originIt->second.rect)
                            << " target=" << rectToString(targetRect);
                        debugLog(out.str());
                    }
                } else if (forceSameFocusTwoColumnSwap && traceColumnRefresh && (refreshGroups[0].hasSizeChangingOrigin || refreshGroups[1].hasSizeChangingOrigin)) {
                    std::ostringstream out;
                    out << "[hymission] niri refresh exact-two repair skip"
                        << " reason=size-changing-origin"
                        << " window=" << debugWindowLabel(managed.window)
                        << " group0SizeChanging=" << (refreshGroups[0].hasSizeChangingOrigin ? 1 : 0)
                        << " group1SizeChanging=" << (refreshGroups[1].hasSizeChangingOrigin ? 1 : 0)
                        << " previousRank=" << previousRank
                        << " nextRank=" << nextRank;
                    debugLog(out.str());
                }
            }
        }
        m_state.slots.push_back(managed.slot);
        if (!rectApproxEqual(managed.relayoutFromGlobal, managed.targetGlobal, 0.5))
            targetChanged = true;
        if (traceColumnRefresh && managed.window && managed.window->m_workspace == workspace) {
            std::ostringstream out;
            out << "[hymission] niri refresh window"
                << " source=" << (source ? source : "?")
                << " window=" << debugWindowLabel(managed.window)
                << " current=" << rectToString(currentRect)
                << " oldTarget=" << rectToString(previousTarget)
                << " oldRelayoutFrom=" << rectToString(previousRelayoutFrom)
                << " nextNatural=" << rectToString(it->naturalGlobal)
                << " nextTarget=" << rectToString(it->targetGlobal)
                << " relayoutFrom=" << rectToString(managed.relayoutFromGlobal)
                << " targetChanged=" << (!rectApproxEqual(previousTarget, managed.targetGlobal, 0.5) ? 1 : 0);
            debugLog(out.str());
        }
        ++updated;
    }

    if (updated == 0)
        return;

    m_state.emptyWorkspacePlaceholders = next.emptyWorkspacePlaceholders;
    const bool forcePreviousRectRelayout = previousPreviewRects && usesDirectNiriScrollingOverview(m_state) && animateRefresh;
    m_state.relayoutActive = (targetChanged || forcePreviousRectRelayout) && (animateRefresh || captureTwoColumnRefresh);
    m_state.relayoutProgress = m_state.relayoutActive ? 0.0 : 1.0;
    m_state.relayoutStart = {};
    if (m_state.relayoutActive)
        beginOverviewRelayoutAnimation(source ? source : "niri-refresh");
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] niri scrolling overview refresh source=" << (source ? source : "?") << " updated=" << updated
            << " animate=" << (m_state.relayoutActive ? 1 : 0)
            << " targetChanged=" << (targetChanged ? 1 : 0)
            << " forcePreviousRectRelayout=" << (forcePreviousRectRelayout ? 1 : 0)
            << " forceFinalLayoutBox=" << (forceFinalScrollingLayoutBox ? 1 : 0)
            << " columns=" << columnCount
            << " pendingSwapcol=" << (hasPendingSwapColumnRelayoutCommit(workspace) ? 1 : 0)
            << " freezeActive=" << (swapColumnBackendPreviewFreezeActiveFor(workspace) ? 1 : 0)
            << " frozenWindows=" << m_swapColumnBackendPreviewFrozenLayout.size();
        debugLog(out.str());
        logSwapColumnFollowupState("niri-refresh-result", workspace, source, selectedWindow());
    }
    if (traceColumnRefresh) {
        logScrollingWorkspaceSpotState("niri-refresh-after", workspace, selectedWindow());
        logOverviewLayoutState("niri-refresh-after", m_state);
    }

    // After rebuilding the state, ensure the scrolling layout's focus matches
    // the overview's selection. This fixes a bug where the visual selection border
    // would not match the centered window after workspace transitions or queued
    // dispatchers that rebuild the layout state.
    if (usesDirectNiriScrollingOverview(m_state) && isScrollingWorkspace(activeLayoutWorkspace())) {
        const auto focusedWindow = m_state.focusDuringOverview;
        const bool cameraOwnsGeometry = scrollingLiveCameraOwnsOverviewGeometry(scrollingAlgorithmForWorkspace(workspace));
        if (!cameraOwnsGeometry && focusedWindow && focusedWindow->m_isMapped && hasManagedWindow(focusedWindow)) {
            (void)syncScrollingWorkspaceSpotOnWindow(focusedWindow);
        }
    }

    updateHoveredFromPointer(false, false, false, false, source ? source : "niri-scroll");
    damageOwnedMonitors();
}
void OverviewController::refreshNiriScrollingOverviewAfterFocusDispatcher(const char* source, PHLWINDOW preferredWindow, bool syncScrollingSpot) {
    if (!isVisible() || (m_state.phase != Phase::Opening && m_state.phase != Phase::Active) || !usesDirectNiriScrollingOverview(m_state))
        return;

    const std::string_view sourceView = source ? std::string_view{source} : std::string_view{};
    const auto liveFocus = Desktop::focusState()->window();
    const bool liveFocusValid = liveFocus && liveFocus->m_isMapped && hasManagedWindow(liveFocus);
    if (sourceView.starts_with("window-active") && directNiriEdgeCameraActive() && !liveFocusValid) {
        // A delayed window.active event from the leaf window can arrive after the
        // native scrolling layout has already released focus for leaf -> scroll-past.
        // Do not let that stale event re-center the leaf and truncate the edge pan.
        m_state.selectedIndex.reset();
        m_state.focusDuringOverview.reset();
        m_queuedOverviewSelectionTarget.reset();
        m_queuedOverviewSelectionSyncScrollingSpot = false;
        m_queuedOverviewSelectionCenterCursor = false;
        m_queuedOverviewLiveFocusTarget.reset();
        m_queuedOverviewLiveFocusSyncScrollingSpot = false;
        m_queuedOverviewLiveFocusCenterCursor = false;
        damageOwnedMonitors();
        return;
    }

    const bool forceEdgeCameraFocusRelayout = sourceView.starts_with("window-active") && directNiriEdgeCameraActive() && liveFocusValid;
    const auto previousPreviewRects = (syncScrollingSpot || forceEdgeCameraFocusRelayout) ? captureCurrentPreviewRects() : PreviewRectSnapshot{};
    PHLWINDOW focusTarget;
    if (preferredWindow && hasManagedWindow(preferredWindow))
        focusTarget = preferredWindow;
    else if (const auto focused = Desktop::focusState()->window(); focused && hasManagedWindow(focused))
        focusTarget = focused;

    if (focusTarget) {
        const auto focusWorkspace = focusTarget->m_pinned ? activeLayoutWorkspace() : focusTarget->m_workspace;
        if (!focusWorkspace || !isScrollingWorkspace(focusWorkspace)) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] skip niri focus refresh source=" << (source ? source : "?")
                    << " target=" << debugWindowLabel(focusTarget)
                    << " workspace=" << debugWorkspaceLabel(focusWorkspace);
                debugLog(out.str());
            }
            return;
        }

        if (m_state.collectionPolicy.onlyActiveWorkspace && focusWorkspace != m_state.ownerWorkspace) {
            m_state.ownerWorkspace = focusWorkspace;
            rebuildVisibleState(focusTarget, true);
            damageOwnedMonitors();
            return;
        }

        selectWindowInState(m_state, focusTarget);
        auto* const scrolling = scrollingAlgorithmForWorkspace(focusWorkspace);
        auto* const controller =
            scrolling && scrolling->m_scrollingData && scrolling->m_scrollingData->controller ? scrolling->m_scrollingData->controller.get() : nullptr;
        const double offsetBefore = controller ? controller->getOffset() : 0.0;
        const bool preserveEdgeCamera = source && std::string_view{source} == "window-active-same" && shouldPreserveDirectNiriEdgeCamera(focusTarget);
        if (debugLogsEnabled())
            logSwapColumnFollowupState("focus-refresh-before-spot-sync", focusWorkspace, source, focusTarget);
        if (syncScrollingSpot && !preserveEdgeCamera)
            (void)syncScrollingWorkspaceSpotOnWindow(
                focusTarget, ScrollingSpotTargeting::Configured, ScrollingSpotSyncIntent::FocusChange);
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] niri focus refresh spot sync"
                << " source=" << (source ? source : "?")
                << " target=" << debugWindowLabel(focusTarget)
                << " workspace=" << debugWorkspaceLabel(focusWorkspace)
                << " syncScrollingSpot=" << (syncScrollingSpot ? 1 : 0)
                << " preserveEdgeCamera=" << (preserveEdgeCamera ? 1 : 0)
                << " focusFit=" << getConfigInt(m_handle, "scrolling:focus_fit_method", 0)
                << " offsetBefore=" << offsetBefore
                << " offsetAfter=" << (controller ? controller->getOffset() : offsetBefore);
            debugLog(out.str());
            logSwapColumnFollowupState("focus-refresh-after-spot-sync", focusWorkspace, source, focusTarget);
        }
        latchHoverSelectionAnchor(g_pInputManager->getMouseCoordsInternal());
    }

    refreshNiriScrollingOverviewAfterLayoutScroll(source, (syncScrollingSpot || forceEdgeCameraFocusRelayout) ? &previousPreviewRects : nullptr);
}
void OverviewController::refreshAfterOfficialScrollMove(const char* source) {
    refreshNiriScrollingOverviewAfterLayoutScroll(source);
}
bool OverviewController::shouldDisableWorkspaceStripForNiriPreview(const State& state) const {
    const bool ownerWorkspaceScrolling = state.ownerWorkspace && isScrollingWorkspace(state.ownerWorkspace);
    const bool focusedWorkspaceScrolling = state.focusDuringOverview && state.focusDuringOverview->m_workspace &&
        isScrollingWorkspace(state.focusDuringOverview->m_workspace);
    return directNiriScrollingOverviewDisablesWorkspaceStrip(niriModeEnabled(), state.collectionPolicy.onlyActiveWorkspace,
                                                             ownerWorkspaceScrolling, focusedWorkspaceScrolling);
}
PHLWORKSPACE OverviewController::centeredEmptyPlaceholderWorkspace(const State& state, const PHLMONITOR& monitor) const {
    if (!monitor)
        return {};

    const Rect content = overviewContentRectForMonitor(monitor, state);
    if (content.width <= 0.0 || content.height <= 0.0)
        return {};

    const double centerX = monitor->m_position.x + content.centerX();
    const double centerY = monitor->m_position.y + content.centerY();
    const auto   placeholderIt = std::find_if(state.emptyWorkspacePlaceholders.begin(), state.emptyWorkspacePlaceholders.end(),
                                              [&](const EmptyWorkspacePlaceholder& placeholder) {
                                                  return !placeholder.backingOnly && placeholder.monitor == monitor && placeholder.workspace &&
                                                      std::abs(placeholder.targetGlobal.centerX() - centerX) <= 1.0 &&
                                                      std::abs(placeholder.targetGlobal.centerY() - centerY) <= 1.0;
                                              });

    return placeholderIt == state.emptyWorkspacePlaceholders.end() ? PHLWORKSPACE{} : placeholderIt->workspace;
}
bool OverviewController::workspaceSwipeUsesVerticalAxis(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return false;

    auto style = workspace->m_renderOffset->getStyle();
    std::ranges::transform(style, style.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return style.starts_with("slidevert") || style.starts_with("slidefadevert");
}
bool OverviewController::shouldSyncScrollingLayoutDuringOverviewFocus() const {
    return m_state.collectionPolicy.onlyActiveWorkspace && usesDirectNiriScrollingOverview(m_state);
}
bool OverviewController::handleNiriOverviewArrowKeybind(xkb_keysym_t keysym, uint32_t modifiers) {
    if (!isVisible() || (m_state.phase != Phase::Opening && m_state.phase != Phase::Active) || !niriModeAppliesToState(m_state))
        return false;

    std::string direction;
    switch (keysym) {
        case XKB_KEY_Left:
            direction = "l";
            break;
        case XKB_KEY_Right:
            direction = "r";
            break;
        case XKB_KEY_Up:
            direction = "u";
            break;
        case XKB_KEY_Down:
            direction = "d";
            break;
        default:
            return false;
    }

    const bool hasSuper = (modifiers & HL_MODIFIER_META) != 0;
    const bool hasShift = (modifiers & HL_MODIFIER_SHIFT) != 0;
    const bool hasCtrl = (modifiers & HL_MODIFIER_CTRL) != 0;
    const bool hasAlt = (modifiers & HL_MODIFIER_ALT) != 0;
    if (!hasSuper)
        return false;

    const bool editAction = hasShift || hasCtrl || hasAlt;
    const bool openDispatcherCooldownActive = overviewOpenInputBarrierActive();
    const bool openVisibilityAnimationActive =
        m_overviewVisibilityAnimation && m_overviewVisibilityAnimation->isBeingAnimated();
    const bool openingEditKeybindGate = editAction && m_state.collectionPolicy.onlyActiveWorkspace &&
        (m_state.phase == Phase::Opening || openVisibilityAnimationActive || m_postOpenRefreshFrames > 0 || openDispatcherCooldownActive);
    if (openingEditKeybindGate) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] consume niri edit arrow during overview open"
                << " direction=" << direction
                << " shift=" << (hasShift ? 1 : 0)
                << " ctrl=" << (hasCtrl ? 1 : 0)
                << " alt=" << (hasAlt ? 1 : 0)
                << " phase=" << static_cast<int>(m_state.phase)
                << " openAnim=" << (openVisibilityAnimationActive ? 1 : 0)
                << " openCooldown=" << (openDispatcherCooldownActive ? 1 : 0);
            debugLog(out.str());
        }
        return true;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] niri arrow keybind"
            << " direction=" << direction
            << " super=" << (hasSuper ? 1 : 0)
            << " shift=" << (hasShift ? 1 : 0)
            << " ctrl=" << (hasCtrl ? 1 : 0)
            << " alt=" << (hasAlt ? 1 : 0)
            << " selected=" << debugWindowLabel(selectedWindow())
            << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview)
            << " workspace=" << debugWorkspaceLabel(activeLayoutWorkspace());
        debugLog(out.str());
    }

    const auto activeWorkspace = activeLayoutWorkspace();
    const auto validTiledDispatchFocus = [&](const PHLWINDOW& window) {
        if (!window || !window->m_isMapped || window->m_pinned || !hasManagedWindow(window))
            return false;

        if (!window->m_workspace || window->m_workspace != activeWorkspace || !isScrollingWorkspace(window->m_workspace))
            return false;

        const auto target = window->layoutTarget();
        return target && !target->floating() && !isFloatingOverviewWindow(window);
    };

    if (editAction) {
        PHLWINDOW dispatchFocus = selectedWindow();
        if (!validTiledDispatchFocus(dispatchFocus))
            dispatchFocus = m_state.focusDuringOverview;
        if (!validTiledDispatchFocus(dispatchFocus))
            dispatchFocus = Desktop::focusState()->window();

        if (!validTiledDispatchFocus(dispatchFocus)) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] consume niri edit keybind without tiled focus"
                    << " selected=" << debugWindowLabel(selectedWindow())
                    << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview)
                    << " active=" << debugWindowLabel(Desktop::focusState()->window())
                    << " workspace=" << debugWorkspaceLabel(activeWorkspace);
                debugLog(out.str());
            }
            return true;
        }
    }

    const auto runNamedEditingDispatcher = [&](std::string_view name, std::string args) {
        const auto original = m_overviewEditingDispatchersOriginal.find(std::string{name});
        if (original == m_overviewEditingDispatchersOriginal.end())
            return false;
        return runOverviewEditingDispatcher(std::string{name}.c_str(), &original->second, std::move(args)).success;
    };

    const auto runFirstNamedEditingDispatcher = [&](std::initializer_list<std::string_view> names, const std::string& args) {
        for (const auto name : names) {
            if (runNamedEditingDispatcher(name, args))
                return true;
        }
        return false;
    };

    const auto runLayoutMessage = [&](std::string args) {
        const std::string logArgs = args;
        const auto logResult = [&](bool success, std::string_view source) {
            if (!debugLogsEnabled())
                return;
            std::ostringstream out;
            out << "[hymission] niri arrow layoutmsg"
                << " source=" << source
                << " args=" << logArgs
                << " success=" << (success ? 1 : 0)
                << " original=" << (m_layoutMessageOriginal ? 1 : 0)
                << " dispatcherName=" << (m_layoutMessageDispatcherName.empty() ? "layoutmsg" : m_layoutMessageDispatcherName)
                << " selected=" << debugWindowLabel(selectedWindow())
                << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview)
                << " workspace=" << debugWorkspaceLabel(activeLayoutWorkspace());
            debugLog(out.str());
        };

        if (m_layoutMessageOriginal) {
            const bool success =
                runOverviewEditingDispatcher(m_layoutMessageDispatcherName.empty() ? "layoutmsg" : m_layoutMessageDispatcherName.c_str(), &m_layoutMessageOriginal, std::move(args)).success;
            logResult(success, "wrapped-original");
            return success;
        }

        if (!g_pKeybindManager)
            return false;

        for (const auto name : {std::string_view{"layoutmsg"}, std::string_view{"layout"}}) {
            const auto dispatcher = g_pKeybindManager->m_dispatchers.find(std::string{name});
            if (dispatcher == g_pKeybindManager->m_dispatchers.end())
                continue;
            if (const bool success = dispatcher->second(args).success; success) {
                logResult(true, name);
                return true;
            }
        }
        logResult(false, "fallback");
        return false;
    };

    // SUPER + SHIFT + CTRL + ALT + Arrow: swap scrolling-layout columns in the strip.
    // Handle this directly because the overview grabs keyboard input while open; simply
    // letting the key event fall through is not reliable for global binds.
    if (hasShift && hasCtrl && hasAlt)
        return runLayoutMessage(std::string{"swapcol "} + direction);

    // SUPER + SHIFT + Arrow: move the selected window in the strip. Prefer the Lua
    // dispatcher names produced by hl.dsp.window.move(), then fall back to legacy names.
    if (hasShift && !hasCtrl && !hasAlt)
        return runFirstNamedEditingDispatcher({"window.move", "movewindow", "movewindoworgroup"}, direction);

    // Keep the older SUPER + SHIFT + CTRL + Arrow swap-window path working too.
    if (hasShift && hasCtrl && !hasAlt)
        return runFirstNamedEditingDispatcher({"window.swap", "swapwindow"}, direction);

    if (hasAlt && !hasCtrl && !hasShift && (keysym == XKB_KEY_Left || keysym == XKB_KEY_Right))
        return runLayoutMessage(keysym == XKB_KEY_Left ? "move -col" : "move +col");

    return false;
}
bool OverviewController::usesDirectNiriScrollingOverview(const State& state) const {
    if (!niriModeAppliesToState(state))
        return false;

    if (std::ranges::any_of(state.windows, [&](const ManagedWindow& managed) {
            if (!managed.window || managed.isNiriFloatingOverlay || !managed.window->m_workspace || !isScrollingWorkspace(managed.window->m_workspace))
                return false;

            const auto target = managed.window->layoutTarget();
            return target && !target->floating();
        }))
        return true;

    // Empty scrolling workspaces must stay on the same direct-Niri path as
    // window-backed scrolling workspaces.  Otherwise close/open code falls back
    // to the generic empty-placeholder path where every viewport's exit rect is
    // just its monitor-sized natural rect, making adjacent wallpaper viewports
    // converge during zoom-in.  The persistent empty workspace placeholders are
    // the direct-Niri "surfaces" in this case.
    if (!state.collectionPolicy.onlyActiveWorkspace || !state.ownerMonitor || state.emptyWorkspacePlaceholders.empty())
        return false;

    return std::ranges::any_of(state.emptyWorkspacePlaceholders, [&](const EmptyWorkspacePlaceholder& placeholder) {
        if (placeholder.backingOnly || !placeholder.monitor || placeholder.monitor != state.ownerMonitor || placeholder.workspaceId == WORKSPACE_INVALID)
            return false;

        return !placeholder.workspace || isScrollingWorkspace(placeholder.workspace);
    });
}
bool OverviewController::activeDirectNiriSingleWorkspaceOverview() const {
    if (!isVisible() || (m_state.phase != Phase::Opening && m_state.phase != Phase::Active) || !m_state.collectionPolicy.onlyActiveWorkspace)
        return false;

    if (usesDirectNiriScrollingOverview(m_state))
        return true;

    // Empty scrolling workspaces still use the direct Niri overview model: the
    // workspace lane/backing placeholder is the thing being zoomed even though
    // there are no tiled window targets. Treating it as non-direct lets focus and
    // edit paths fall back to Hyprland's live focus, which can belong to another
    // monitor/layout and can leave the renderer with mismatched workspace state.
    return niriModeEnabled() && centeredEmptyWorkspacePlaceholder(m_state) != nullptr;
}
bool OverviewController::timedNiriSingleWorkspaceTransitionActive() const {
    return m_workspaceTransition.active && m_workspaceTransition.mode == WorkspaceTransitionMode::TimedCommit &&
        m_state.collectionPolicy.onlyActiveWorkspace &&
        (niriModeAppliesToState(m_workspaceTransition.sourceState) || niriModeAppliesToState(m_workspaceTransition.targetState));
}
bool OverviewController::shouldSuppressNiriFocusScrollForMonitorReturn(const PHLWINDOW& window, const PHLMONITOR& previousFocusMonitor) const {
    if (!window || !previousFocusMonitor || !m_state.ownerMonitor)
        return false;

    if (!isVisible() || (m_state.phase != Phase::Opening && m_state.phase != Phase::Active) || !m_state.collectionPolicy.onlyActiveWorkspace ||
        !usesDirectNiriScrollingOverview(m_state))
        return false;

    const auto currentMonitor = focusMonitorForWindow(window);
    return currentMonitor && currentMonitor == m_state.ownerMonitor && previousFocusMonitor != m_state.ownerMonitor;
}
std::size_t directNiriScrollingColumnCount(const PHLWORKSPACE& workspace) {
    if (!workspace)
        return 0;

    auto* const scrolling = scrollingAlgorithmForWorkspace(workspace);
    return scrolling && scrolling->m_scrollingData ? scrolling->m_scrollingData->columns.size() : 0;
}

PHLWINDOW OverviewController::directNiriFocusedOverviewWindow(const State& state) const {
    if (!usesDirectNiriScrollingOverview(state))
        return {};

    const auto validManagedFocus = [&](const PHLWINDOW& window) -> PHLWINDOW {
        if (!window || !window->m_isMapped)
            return {};

        if (state.collectionPolicy.onlyActiveWorkspace && !window->m_pinned) {
            if (!window->m_workspace || window->m_workspace->m_isSpecialWorkspace)
                return {};

            // In direct niri single-workspace overview, the centered/focused
            // workspace is allowed to move away from state.ownerWorkspace while
            // the overview is still open.  Do not reject the actual overview
            // focus just because ownerWorkspace is one rebuild behind; only keep
            // it scoped to the overview monitor.
            if (state.ownerMonitor && window->m_workspace->m_monitor.lock() != state.ownerMonitor)
                return {};
        }

        return managedWindowFor(state, window, true) ? window : PHLWINDOW{};
    };

    if (directNiriOwnerEdgeCameraActive(state)) {
        const bool singleColumnOwnerWorkspace = directNiriScrollingColumnCount(state.ownerWorkspace) == 1;
        if (singleColumnOwnerWorkspace) {
            // A single centered scrolling column can still report the edge-camera
            // predicate even though Hyprland conceptually treats it as a normal
            // focused leaf. Prefer the overview-selected leaf immediately so the
            // active border/focus handoff works on the first workspace switch.
            if (const auto overviewFocus = validManagedFocus(state.focusDuringOverview); overviewFocus)
                return overviewFocus;

            if (state.selectedIndex && *state.selectedIndex < state.windows.size()) {
                if (const auto selected = validManagedFocus(state.windows[*state.selectedIndex].window); selected)
                    return selected;
            }

            if (const auto liveFocus = validManagedFocus(Desktop::focusState()->window()); liveFocus)
                return liveFocus;

            return {};
        }

        // Multi-column edge-camera is the real scroll-past state.  Do not borrow
        // Hyprland's stale live/last leaf focus here.  A focused edge-camera leaf
        // is only valid during the short reverse-scroll handoff window that
        // Hymission arms after moving back from scroll-past toward the strip.
        if (multiColumnEdgeFocusOverrideActive()) {
            if (const auto overviewFocus = validManagedFocus(state.focusDuringOverview); overviewFocus)
                return overviewFocus;
        }

        return {};
    }

    if (const auto overviewFocus = validManagedFocus(state.focusDuringOverview); overviewFocus)
        return overviewFocus;

    if (state.selectedIndex && *state.selectedIndex < state.windows.size()) {
        if (const auto selected = validManagedFocus(state.windows[*state.selectedIndex].window); selected)
            return selected;
    }

    return validManagedFocus(Desktop::focusState()->window());
}
bool OverviewController::directNiriOverviewHasSingleColumnAnchor(const PHLWINDOW& anchor) const {
    if (!anchor || !anchor->m_isMapped || !anchor->m_workspace || !isScrollingWorkspace(anchor->m_workspace))
        return false;

    auto* scrolling = scrollingAlgorithmForWorkspace(anchor->m_workspace);
    if (!scrolling || !scrolling->m_scrollingData)
        return false;

    const auto anchorTarget = anchor->layoutTarget();
    if (!anchorTarget || anchorTarget->floating())
        return false;

    const auto anchorData = scrolling->dataFor(anchorTarget);
    const auto anchorColumn = anchorData ? anchorData->column.lock() : SP<Layout::Tiled::SColumnData>{};
    if (!anchorColumn)
        return false;

    const auto workspace = anchor->m_workspace;
    std::vector<SP<Layout::Tiled::SColumnData>> overviewColumns;
    for (const auto& managed : m_state.windows) {
        const auto window = managed.window;
        if (!window || !window->m_isMapped || window->m_workspace != workspace || window->m_pinned || isFloatingOverviewWindow(window))
            continue;

        const auto target = window->layoutTarget();
        if (!target || target->floating())
            continue;

        const auto targetData = scrolling->dataFor(target);
        const auto column = targetData ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};
        if (!column || std::ranges::find(overviewColumns, column) != overviewColumns.end())
            continue;

        overviewColumns.push_back(column);
    }

    return overviewColumns.size() == 1 && overviewColumns.front() == anchorColumn;
}
PHLWINDOW OverviewController::directNiriOneToTwoColumnOpenAnchor(const PHLWINDOW& openedWindow) const {
    if (!openedWindow || !openedWindow->m_isMapped || hasManagedWindow(openedWindow))
        return {};

    if (!isVisible() || m_workspaceTransition.active || (m_state.phase != Phase::Opening && m_state.phase != Phase::Active) ||
        !m_state.collectionPolicy.onlyActiveWorkspace || !usesDirectNiriScrollingOverview(m_state))
        return {};

    const auto workspace = openedWindow->m_workspace;
    if (!workspace || !isScrollingWorkspace(workspace))
        return {};

    if (m_state.ownerWorkspace && m_state.ownerWorkspace != workspace)
        return {};

    auto* scrolling = scrollingAlgorithmForWorkspace(workspace);
    if (!scrolling || !scrolling->m_scrollingData || scrolling->m_scrollingData->columns.size() != 2)
        return {};

    const auto anchor = directNiriFocusedOverviewWindow(m_state);
    if (!anchor || anchor == openedWindow || anchor->m_workspace != workspace || !directNiriOverviewHasSingleColumnAnchor(anchor))
        return {};

    return anchor;
}
void OverviewController::stabilizeDirectNiriOneToTwoColumnOpen(const PHLWINDOW& anchor) {
    if (!anchor || !anchor->m_isMapped || !anchor->m_workspace)
        return;

    selectWindowInState(m_state, anchor);
    m_state.focusDuringOverview = anchor;
    anchor->m_workspace->m_lastFocusedWindow = anchor;

    if (Desktop::focusState()->window() != anchor) {
        m_pendingLiveFocusWorkspaceChangeTarget = anchor;
        focusWindowCompat(anchor, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
        if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == anchor)
            m_pendingLiveFocusWorkspaceChangeTarget.reset();
    }

    (void)syncScrollingWorkspaceSpotOnWindow(anchor);
    refreshWorkspaceLayoutSnapshot(anchor->m_workspace);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] niri one-to-two open anchor=" << debugWindowLabel(anchor)
            << " workspace=" << debugWorkspaceLabel(anchor->m_workspace);
        debugLog(out.str());
    }
}
PHLWINDOW OverviewController::preferredOverviewExitFocus() const {
    if (directNiriEdgeCameraActive()) {
        if (const auto directNiriFocus = directNiriFocusedOverviewWindow(m_state); directNiriFocus)
            return directNiriFocus;

        return {};
    }

    if (const auto directNiriFocus = directNiriFocusedOverviewWindow(m_state); directNiriFocus)
        return directNiriFocus;

    if (usesDirectNiriScrollingOverview(m_state) && m_state.collectionPolicy.onlyActiveWorkspace && m_state.focusDuringOverview &&
        m_state.focusDuringOverview->m_isMapped && !m_state.focusDuringOverview->m_pinned && m_state.focusDuringOverview->m_workspace &&
        !m_state.focusDuringOverview->m_workspace->m_isSpecialWorkspace && isScrollingWorkspace(m_state.focusDuringOverview->m_workspace))
        return m_state.focusDuringOverview;

    const auto focusDuringOverview = m_state.focusDuringOverview && hasManagedWindow(m_state.focusDuringOverview) ? m_state.focusDuringOverview : PHLWINDOW{};
    const auto selected = selectedWindow();
    const auto hovered = hoveredWindow();

    if (focusDuringOverview && focusDuringOverview != m_state.focusBeforeOpen)
        return focusDuringOverview;
    if (selected && selected != m_state.focusBeforeOpen)
        return selected;
    if (hovered)
        return hovered;
    if (focusDuringOverview)
        return focusDuringOverview;
    if (selected)
        return selected;

    return {};
}
bool OverviewController::directNiriEdgeCameraActive() const {
    if (!isVisible() || !m_state.collectionPolicy.onlyActiveWorkspace || !usesDirectNiriScrollingOverview(m_state))
        return false;

    const auto workspace = activeLayoutWorkspace();
    return workspace && isScrollingWorkspace(workspace) && scrollingEdgeCameraActive(scrollingAlgorithmForWorkspace(workspace));
}
bool OverviewController::directNiriOwnerEdgeCameraActive(const State& state) const {
    if (!state.collectionPolicy.onlyActiveWorkspace || !usesDirectNiriScrollingOverview(state))
        return false;

    const auto workspace = state.ownerWorkspace;
    return workspace && isScrollingWorkspace(workspace) && scrollingEdgeCameraActive(scrollingAlgorithmForWorkspace(workspace));
}
const OverviewController::EmptyWorkspacePlaceholder* OverviewController::directNiriEdgeCameraOpenPlaceholder(const State& state) const {
    if (!directNiriOwnerEdgeCameraActive(state) || !state.ownerWorkspace || !state.ownerMonitor)
        return nullptr;

    const auto placeholder = std::ranges::find_if(state.emptyWorkspacePlaceholders, [&](const EmptyWorkspacePlaceholder& candidate) {
        return candidate.backingOnly && candidate.workspace == state.ownerWorkspace && candidate.monitor == state.ownerMonitor;
    });
    return placeholder == state.emptyWorkspacePlaceholders.end() ? nullptr : &*placeholder;
}
bool OverviewController::shouldPreserveDirectNiriEdgeCamera(const PHLWINDOW& window) const {
    if (!window || !window->m_workspace || !isVisible() || !m_state.collectionPolicy.onlyActiveWorkspace ||
        !usesDirectNiriScrollingOverview(m_state) || !isScrollingWorkspace(window->m_workspace))
        return false;

    auto* const scrolling = scrollingAlgorithmForWorkspace(window->m_workspace);
    if (!scrollingEdgeCameraActive(scrolling))
        return false;

    if (directNiriScrollingColumnCount(window->m_workspace) == 1)
        return false;

    // A centered first/last column can legally put Hyprland's scroll offset outside
    // the normal clamped range. That is not the no-focus scroll-past state once
    // Hyprland has restored a real focused window. Preserve the edge camera only
    // while native focus is still released, otherwise the overview keeps erasing
    // the restored leaf focus on the first frame it becomes visible again.
    const auto focused = Desktop::focusState()->window();
    return !focused || !focused->m_isMapped || focused->m_workspace != window->m_workspace;
}
PHLWORKSPACE OverviewController::directNiriTwoColumnExitWorkspace() const {
    if (!m_state.collectionPolicy.onlyActiveWorkspace || !usesDirectNiriScrollingOverview(m_state))
        return {};

    PHLWORKSPACE workspace;
    if (const auto target = directNiriFocusedOverviewWindow(m_state); target && target->m_workspace)
        workspace = target->m_workspace;
    if (!workspace)
        workspace = m_state.ownerWorkspace;
    if (!workspace || !isScrollingWorkspace(workspace))
        return {};

    auto* scrolling = scrollingAlgorithmForWorkspace(workspace);
    if (!scrolling || !scrolling->m_scrollingData || scrolling->m_scrollingData->columns.size() != 2)
        return {};

    return workspace;
}
void OverviewController::freezeDirectNiriTwoColumnExitPreviewTargets() {
    if (m_state.phase != Phase::Active)
        return;

    const auto workspace = directNiriTwoColumnExitWorkspace();
    if (!workspace)
        return;

    std::vector<std::pair<std::size_t, Rect>> previews;
    previews.reserve(m_state.windows.size());
    for (std::size_t index = 0; index < m_state.windows.size(); ++index) {
        auto& managed = m_state.windows[index];
        if (!managed.window || !managed.window->m_isMapped)
            continue;

        const bool belongsToWorkspace = managed.window->m_workspace == workspace || managed.window->m_pinned || managed.isPinned;
        if (!belongsToWorkspace)
            continue;

        previews.emplace_back(index, currentPreviewRect(managed));
    }

    for (const auto& [index, preview] : previews) {
        auto& managed = m_state.windows[index];
        managed.targetGlobal = preview;
        managed.relayoutFromGlobal = preview;
        managed.exitGlobal = preview;
        if (managed.targetMonitor) {
            managed.slot.target = makeRect(preview.x - managed.targetMonitor->m_position.x,
                                           preview.y - managed.targetMonitor->m_position.y,
                                           preview.width,
                                           preview.height);
            for (auto& slot : m_state.slots) {
                if (slot.index == managed.slot.index) {
                    slot = managed.slot;
                    break;
                }
            }
        }
    }

    m_state.relayoutActive = false;
    m_state.relayoutProgress = 1.0;
    m_state.relayoutStart = {};

    if (debugLogsEnabled() && !previews.empty()) {
        std::ostringstream out;
        out << "[hymission] niri two-column exit preview freeze workspace=" << debugWorkspaceLabel(workspace)
            << " updated=" << previews.size();
        if (const auto selected = selectedWindow())
            out << " selected=" << debugWindowLabel(selected);
        debugLog(out.str());
    }
}
void OverviewController::stabilizeDirectNiriExitSnapshot(const PHLWINDOW& target) {
    if (!target || !target->m_isMapped || !usesDirectNiriScrollingOverview(m_state) || !m_state.collectionPolicy.onlyActiveWorkspace)
        return;

    auto monitor = m_state.ownerMonitor ? m_state.ownerMonitor : target->m_monitor.lock();
    if (!monitor)
        return;

    State stable = buildState(monitor, m_state.collectionPolicy.requestedScope, {}, false, m_state.suppressWorkspaceStrip, target);
    if (stable.windows.empty() || !selectWindowInState(stable, target))
        return;

    std::vector<WindowSlot> stableSlots;
    stableSlots.reserve(m_state.windows.size());
    std::size_t updated = 0;
    for (auto& managed : m_state.windows) {
        auto stableIt = std::find_if(stable.windows.begin(), stable.windows.end(), [&](const ManagedWindow& candidate) {
            return candidate.window == managed.window;
        });
        if (stableIt == stable.windows.end()) {
            stableSlots.push_back(managed.slot);
            continue;
        }

        managed.targetMonitor = stableIt->targetMonitor;
        managed.naturalGlobal = stableIt->naturalGlobal;
        managed.exitGlobal = stableIt->exitGlobal;
        managed.targetGlobal = stableIt->targetGlobal;
        managed.relayoutFromGlobal = stableIt->targetGlobal;
        managed.slot = stableIt->slot;
        managed.isNiriFloatingOverlay = stableIt->isNiriFloatingOverlay;
        stableSlots.push_back(managed.slot);
        ++updated;
    }

    if (updated == 0)
        return;

    m_state.slots = std::move(stableSlots);
    m_state.emptyWorkspacePlaceholders = stable.emptyWorkspacePlaceholders;
    for (auto& placeholder : m_state.emptyWorkspacePlaceholders)
        placeholder.relayoutFromGlobal = placeholder.targetGlobal;

    m_state.stripEntries = stable.stripEntries;
    for (auto& entry : m_state.stripEntries) {
        entry.relayoutFromRect = entry.rect;
        entry.hasRelayoutFromRect = false;
    }

    selectWindowInState(m_state, target);
    m_state.focusDuringOverview = target;
    m_state.relayoutActive = false;
    m_state.relayoutProgress = 1.0;
    m_state.relayoutStart = {};

    if (debugLogsEnabled()) {
        const auto* managed = managedWindowFor(m_state, target, true);
        std::ostringstream out;
        out << "[hymission] niri exit snapshot stabilize target=" << debugWindowLabel(target)
            << " updated=" << updated;
        if (managed)
            out << " targetGlobal=" << rectToString(managed->targetGlobal);
        debugLog(out.str());
    }
}
void OverviewController::enforceDirectNiriExitFocusGuard() {
    if (!usesDirectNiriScrollingOverview(m_state) || !m_state.collectionPolicy.onlyActiveWorkspace)
        return;

    if (directNiriEdgeCameraActive())
        return;

    if (m_workspaceTransition.active) {
        if (m_workspaceTransition.mode != WorkspaceTransitionMode::TimedCommit)
            return;

        commitOverviewWorkspaceTransition(false, true);
        if (!isVisible() || !usesDirectNiriScrollingOverview(m_state) || !m_state.collectionPolicy.onlyActiveWorkspace)
            return;
    }

    const auto target = directNiriFocusedOverviewWindow(m_state);
    if (!target || !target->m_isMapped)
        return;

    const auto previousSelected = selectedWindow();
    selectWindowInState(m_state, target);
    m_state.focusDuringOverview = target;
    m_queuedOverviewSelectionTarget.reset();
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewSelectionCenterCursor = false;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusCenterCursor = false;

    if (target->m_workspace)
        target->m_workspace->m_lastFocusedWindow = target;
    (void)syncScrollingWorkspaceSpotOnWindow(target);
    if (target->m_workspace)
        refreshWorkspaceLayoutSnapshot(target->m_workspace);
    refreshNiriScrollingOverviewAfterLayoutScroll("exit-focus-guard");
    stabilizeDirectNiriExitSnapshot(target);

    if (debugLogsEnabled() && previousSelected != target) {
        std::ostringstream out;
        out << "[hymission] niri exit focus guard target=" << debugWindowLabel(target)
            << " previousSelected=" << debugWindowLabel(previousSelected);
        debugLog(out.str());
    }
}
void OverviewController::reconcileNiriCenteredSelectionForExit() {
    if (!isVisible() || (m_state.phase != Phase::Opening && m_state.phase != Phase::Active) || !usesDirectNiriScrollingOverview(m_state))
        return;

    if (centeredEmptyWorkspacePlaceholder(m_state)) {
        m_state.selectedIndex.reset();
        m_state.focusDuringOverview.reset();
        m_queuedOverviewSelectionTarget.reset();
        m_queuedOverviewSelectionSyncScrollingSpot = false;
        m_queuedOverviewSelectionCenterCursor = false;
        m_queuedOverviewLiveFocusTarget.reset();
        m_queuedOverviewLiveFocusSyncScrollingSpot = false;
        m_queuedOverviewLiveFocusCenterCursor = false;
        return;
    }

    PHLWINDOW centeredTarget;
    if (const auto queuedSelection = m_queuedOverviewSelectionTarget.lock(); queuedSelection && queuedSelection->m_isMapped && hasManagedWindow(queuedSelection))
        centeredTarget = queuedSelection;
    else if (const auto queuedFocus = m_queuedOverviewLiveFocusTarget.lock(); queuedFocus && queuedFocus->m_isMapped && hasManagedWindow(queuedFocus))
        centeredTarget = queuedFocus;
    else if (m_state.focusDuringOverview && m_state.focusDuringOverview->m_isMapped && hasManagedWindow(m_state.focusDuringOverview))
        centeredTarget = m_state.focusDuringOverview;

    if (!centeredTarget)
        return;

    const auto centeredIt =
        std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == centeredTarget; });
    if (centeredIt == m_state.windows.end())
        return;

    const auto centeredIndex = static_cast<std::size_t>(std::distance(m_state.windows.begin(), centeredIt));
    const auto previousSelected = selectedWindow();
    m_state.selectedIndex = centeredIndex;
    m_state.focusDuringOverview = centeredTarget;

    m_queuedOverviewSelectionTarget.reset();
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewSelectionCenterCursor = false;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusCenterCursor = false;

    if (debugLogsEnabled() && previousSelected != centeredTarget) {
        std::ostringstream out;
        out << "[hymission] niri exit reconcile centered target=" << debugWindowLabel(centeredTarget)
            << " selected=" << centeredIndex
            << " previousSelected=" << debugWindowLabel(previousSelected);
        debugLog(out.str());
    }
}
std::optional<Vector2D> OverviewController::predictedScrollingExitTranslation(const PHLWINDOW& window) const {
    if (!window || !window->m_isMapped || !window->m_workspace || !window->m_workspace->m_space || !isScrollingWorkspace(window->m_workspace))
        return std::nullopt;

    const auto target = window->layoutTarget();
    if (!target || target->floating())
        return Vector2D{};

    auto direction = getConfigString(m_handle, "scrolling:direction", "right");
    const auto workspaceRule = Config::workspaceRuleMgr()->getWorkspaceRuleFor(window->m_workspace).value_or(Config::CWorkspaceRule{});
    if (workspaceRule.m_layoutopts.contains("direction") && !workspaceRule.m_layoutopts.at("direction").empty())
        direction = workspaceRule.m_layoutopts.at("direction");

    const bool vertical = direction == "down" || direction == "up";
    const auto workArea = window->m_workspace->m_space->workArea();
    const auto targetBox = target->position();

    const double viewStart = vertical ? workArea.y : workArea.x;
    const double viewEnd = vertical ? (workArea.y + workArea.h) : (workArea.x + workArea.w);
    const double stripStart = vertical ? targetBox.y : targetBox.x;
    const double stripEnd = vertical ? (targetBox.y + targetBox.h) : (targetBox.x + targetBox.w);

    double deltaPrimary = 0.0;
    const bool fullyVisible = stripStart >= viewStart && stripEnd <= viewEnd;
    if (!fullyVisible) {
        if (getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 1) {
            if (stripStart < viewStart)
                deltaPrimary = viewStart - stripStart;
            else if (stripEnd > viewEnd)
                deltaPrimary = viewEnd - stripEnd;
        } else {
            deltaPrimary = (viewStart + viewEnd) * 0.5 - (stripStart + stripEnd) * 0.5;
        }
    }

    return vertical ? Vector2D{0.0, deltaPrimary} : Vector2D{deltaPrimary, 0.0};
}
bool OverviewController::applyNiriScrollingCameraExitGeometry(const PHLWINDOW& window) {
    if (!window || !window->m_workspace || !isScrollingWorkspace(window->m_workspace) || !m_state.collectionPolicy.onlyActiveWorkspace ||
        !usesDirectNiriScrollingOverview(m_state))
        return false;

    const auto* selectedManaged = managedWindowFor(window);
    if (!selectedManaged)
        return false;

    const bool useStableExitPreview = m_beginCloseInProgress && m_state.collectionPolicy.onlyActiveWorkspace && usesDirectNiriScrollingOverview(m_state);
    const auto exitPreviewRect = [&](const ManagedWindow& managed) {
        return useStableExitPreview ? managed.targetGlobal : currentPreviewRect(managed);
    };

    const Rect selectedPreview = exitPreviewRect(*selectedManaged);
    const Rect selectedExit = goalGlobalRectForWindow(window);
    if (selectedPreview.width <= 1.0 || selectedPreview.height <= 1.0 || selectedExit.width <= 1.0 || selectedExit.height <= 1.0)
        return false;

    const double scaleX = selectedExit.width / selectedPreview.width;
    const double scaleY = selectedExit.height / selectedPreview.height;
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0 || scaleY <= 0.0)
        return false;

    for (auto& managed : m_state.windows) {
        if (!managed.window || !managed.window->m_isMapped)
            continue;

        const Rect preview = exitPreviewRect(managed);
        managed.exitGlobal = makeRect(selectedExit.centerX() + (preview.centerX() - selectedPreview.centerX()) * scaleX - preview.width * scaleX * 0.5,
                                      selectedExit.centerY() + (preview.centerY() - selectedPreview.centerY()) * scaleY - preview.height * scaleY * 0.5,
                                      preview.width * scaleX, preview.height * scaleY);
    }

    const auto currentPlaceholderRect = [&](const EmptyWorkspacePlaceholder& placeholder) {
        if (useStableExitPreview)
            return placeholder.targetGlobal;
        if (m_state.phase == Phase::Active && m_state.relayoutActive)
            return lerpRect(placeholder.relayoutFromGlobal, placeholder.targetGlobal, relayoutVisualProgress());
        return placeholder.targetGlobal;
    };
    for (auto& placeholder : m_state.emptyWorkspacePlaceholders) {
        const Rect preview = currentPlaceholderRect(placeholder);
        placeholder.exitGlobal = makeRect(selectedExit.centerX() + (preview.centerX() - selectedPreview.centerX()) * scaleX - preview.width * scaleX * 0.5,
                                          selectedExit.centerY() + (preview.centerY() - selectedPreview.centerY()) * scaleY - preview.height * scaleY * 0.5,
                                          preview.width * scaleX, preview.height * scaleY);
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] niri scrolling camera exit target=" << debugWindowLabel(window)
            << " selectedPreview=" << rectToString(selectedPreview)
            << " selectedExit=" << rectToString(selectedExit)
            << " scale=(" << scaleX << "," << scaleY << ")";
        debugLog(out.str());
    }

    return true;
}
bool OverviewController::applyNiriScrollingCameraExitGeometry(const EmptyWorkspacePlaceholder& placeholder) {
    if (!placeholder.monitor || placeholder.workspaceId == WORKSPACE_INVALID || !m_state.collectionPolicy.onlyActiveWorkspace ||
        !usesDirectNiriScrollingOverview(m_state))
        return false;

    if (placeholder.workspace && !isScrollingWorkspace(placeholder.workspace))
        return false;

    const auto currentPlaceholderRect = [&](const EmptyWorkspacePlaceholder& current) {
        if (m_state.phase == Phase::Active && m_state.relayoutActive)
            return lerpRect(current.relayoutFromGlobal, current.targetGlobal, relayoutVisualProgress());
        return current.targetGlobal;
    };

    const Rect selectedPreview = currentPlaceholderRect(placeholder);
    const Rect selectedExit = placeholder.naturalGlobal;
    if (selectedPreview.width <= 1.0 || selectedPreview.height <= 1.0 || selectedExit.width <= 1.0 || selectedExit.height <= 1.0)
        return false;

    const double scaleX = selectedExit.width / selectedPreview.width;
    const double scaleY = selectedExit.height / selectedPreview.height;
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0 || scaleY <= 0.0)
        return false;

    for (auto& managed : m_state.windows) {
        if (!managed.window || !managed.window->m_isMapped)
            continue;

        const Rect preview = currentPreviewRect(managed);
        managed.exitGlobal = makeRect(selectedExit.centerX() + (preview.centerX() - selectedPreview.centerX()) * scaleX - preview.width * scaleX * 0.5,
                                      selectedExit.centerY() + (preview.centerY() - selectedPreview.centerY()) * scaleY - preview.height * scaleY * 0.5,
                                      preview.width * scaleX, preview.height * scaleY);
    }

    for (auto& current : m_state.emptyWorkspacePlaceholders) {
        const Rect preview = currentPlaceholderRect(current);
        current.exitGlobal = makeRect(selectedExit.centerX() + (preview.centerX() - selectedPreview.centerX()) * scaleX - preview.width * scaleX * 0.5,
                                      selectedExit.centerY() + (preview.centerY() - selectedPreview.centerY()) * scaleY - preview.height * scaleY * 0.5,
                                      preview.width * scaleX, preview.height * scaleY);
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] niri scrolling camera exit placeholder=" << (placeholder.workspace ? debugWorkspaceLabel(placeholder.workspace) : std::to_string(placeholder.workspaceId))
            << " selectedPreview=" << rectToString(selectedPreview)
            << " selectedExit=" << rectToString(selectedExit)
            << " scale=(" << scaleX << "," << scaleY << ")";
        debugLog(out.str());
    }

    return true;
}
bool OverviewController::applyNiriScrollingCameraOpenGeometry(const PHLWINDOW& window) {
    if (!window || !window->m_workspace || !isScrollingWorkspace(window->m_workspace) || !m_state.collectionPolicy.onlyActiveWorkspace ||
        !usesDirectNiriScrollingOverview(m_state))
        return false;

    const auto* selectedManaged = managedWindowFor(window);
    if (!selectedManaged)
        return false;

    const Rect selectedStart = liveGlobalRectForWindow(window);
    const Rect selectedTarget = selectedManaged->targetGlobal;
    if (selectedStart.width <= 1.0 || selectedStart.height <= 1.0 || selectedTarget.width <= 1.0 || selectedTarget.height <= 1.0)
        return false;

    const double scaleX = selectedStart.width / selectedTarget.width;
    const double scaleY = selectedStart.height / selectedTarget.height;
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0 || scaleY <= 0.0)
        return false;

    for (auto& managed : m_state.windows) {
        if (!managed.window || !managed.window->m_isMapped)
            continue;

        const Rect target = managed.targetGlobal;
        managed.naturalGlobal = makeRect(selectedStart.centerX() + (target.centerX() - selectedTarget.centerX()) * scaleX - target.width * scaleX * 0.5,
                                         selectedStart.centerY() + (target.centerY() - selectedTarget.centerY()) * scaleY - target.height * scaleY * 0.5,
                                         target.width * scaleX, target.height * scaleY);
        managed.exitGlobal = managed.naturalGlobal;
    }

    for (auto& placeholder : m_state.emptyWorkspacePlaceholders) {
        const Rect target = placeholder.targetGlobal;
        placeholder.naturalGlobal = makeRect(selectedStart.centerX() + (target.centerX() - selectedTarget.centerX()) * scaleX - target.width * scaleX * 0.5,
                                             selectedStart.centerY() + (target.centerY() - selectedTarget.centerY()) * scaleY - target.height * scaleY * 0.5,
                                             target.width * scaleX, target.height * scaleY);
        placeholder.exitGlobal = placeholder.naturalGlobal;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] niri scrolling camera open target=" << debugWindowLabel(window)
            << " selectedStart=" << rectToString(selectedStart)
            << " selectedTarget=" << rectToString(selectedTarget)
            << " scale=(" << scaleX << "," << scaleY << ")";
        debugLog(out.str());
    }

    return true;
}
bool OverviewController::applyNiriScrollingCameraOpenGeometry(const EmptyWorkspacePlaceholder& placeholder) {
    if (!placeholder.monitor || placeholder.workspaceId == WORKSPACE_INVALID || !m_state.collectionPolicy.onlyActiveWorkspace ||
        !usesDirectNiriScrollingOverview(m_state))
        return false;

    if (placeholder.workspace && !isScrollingWorkspace(placeholder.workspace))
        return false;

    const Rect selectedStart = placeholder.naturalGlobal;
    const Rect selectedTarget = placeholder.targetGlobal;
    if (selectedStart.width <= 1.0 || selectedStart.height <= 1.0 || selectedTarget.width <= 1.0 || selectedTarget.height <= 1.0)
        return false;

    const double scaleX = selectedStart.width / selectedTarget.width;
    const double scaleY = selectedStart.height / selectedTarget.height;
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0 || scaleY <= 0.0)
        return false;

    for (auto& managed : m_state.windows) {
        if (!managed.window || !managed.window->m_isMapped)
            continue;

        const Rect target = managed.targetGlobal;
        managed.naturalGlobal = makeRect(selectedStart.centerX() + (target.centerX() - selectedTarget.centerX()) * scaleX - target.width * scaleX * 0.5,
                                         selectedStart.centerY() + (target.centerY() - selectedTarget.centerY()) * scaleY - target.height * scaleY * 0.5,
                                         target.width * scaleX, target.height * scaleY);
        managed.exitGlobal = managed.naturalGlobal;
    }

    for (auto& current : m_state.emptyWorkspacePlaceholders) {
        const Rect target = current.targetGlobal;
        current.naturalGlobal = makeRect(selectedStart.centerX() + (target.centerX() - selectedTarget.centerX()) * scaleX - target.width * scaleX * 0.5,
                                         selectedStart.centerY() + (target.centerY() - selectedTarget.centerY()) * scaleY - target.height * scaleY * 0.5,
                                         target.width * scaleX, target.height * scaleY);
        current.exitGlobal = current.naturalGlobal;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] niri scrolling camera open placeholder=" << (placeholder.workspace ? debugWorkspaceLabel(placeholder.workspace) : std::to_string(placeholder.workspaceId))
            << " selectedStart=" << rectToString(selectedStart)
            << " selectedTarget=" << rectToString(selectedTarget)
            << " scale=(" << scaleX << "," << scaleY << ")";
        debugLog(out.str());
    }

    return true;
}
void OverviewController::prepareGestureCloseExitGeometry() {
    const auto predictedPlaceholderWorkspace = resolveExitWorkspace(CloseMode::Normal);
    const auto* predictedPlaceholder = centeredEmptyWorkspacePlaceholder(m_state);
    const auto predictedExitFocus = resolveExitFocus(CloseMode::Normal);
    const auto predictedExitWorkspace = predictedPlaceholderWorkspace ? predictedPlaceholderWorkspace : (predictedExitFocus ? predictedExitFocus->m_workspace : PHLWORKSPACE{});
    const auto predictedExitMonitor =
        predictedExitWorkspace && predictedExitWorkspace->m_monitor.lock() ? predictedExitWorkspace->m_monitor.lock() :
        (predictedExitFocus ? predictedExitFocus->m_monitor.lock() : PHLMONITOR{});
    const auto currentWorkspaceOnTargetMonitor = predictedExitMonitor ? predictedExitMonitor->m_activeWorkspace : PHLWORKSPACE{};
    const auto scrollingTranslation = predictedScrollingExitTranslation(predictedExitFocus);
    const bool preferGoalGeometry = isScrollingWorkspace(predictedExitWorkspace);
    const bool workspaceSwitchOnExit =
        predictedExitWorkspace && predictedExitMonitor && !predictedExitWorkspace->m_isSpecialWorkspace && currentWorkspaceOnTargetMonitor &&
        predictedExitWorkspace != currentWorkspaceOnTargetMonitor;

    Vector2D incomingWorkspaceOffset;
    Vector2D outgoingWorkspaceOffset;
    if (workspaceSwitchOnExit) {
        const bool animToLeft =
            shouldWrapWorkspaceIds(predictedExitWorkspace->m_id, currentWorkspaceOnTargetMonitor->m_id) ^ (predictedExitWorkspace->m_id > currentWorkspaceOnTargetMonitor->m_id);
        incomingWorkspaceOffset = predictedWorkspaceAnimationOffset(m_handle, predictedExitMonitor, predictedExitWorkspace, animToLeft, true);
        outgoingWorkspaceOffset = predictedWorkspaceAnimationOffset(m_handle, predictedExitMonitor, currentWorkspaceOnTargetMonitor, animToLeft, false);
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] prepare gesture close exit";
        if (predictedExitFocus)
            out << " target=" << debugWindowLabel(predictedExitFocus);
        else if (predictedPlaceholderWorkspace)
            out << " targetWorkspace=" << debugWorkspaceLabel(predictedPlaceholderWorkspace);
        else
            out << " target=<null>";
        out << " workspaceSwitch=" << (workspaceSwitchOnExit ? 1 : 0);
        if (currentWorkspaceOnTargetMonitor)
            out << " currentWorkspace=" << currentWorkspaceOnTargetMonitor->m_id;
        if (predictedExitWorkspace)
            out << " targetWorkspace=" << predictedExitWorkspace->m_id;
        if (scrollingTranslation)
            out << " scrollingDelta=" << vectorToString(*scrollingTranslation);
        else
            out << " scrollingDelta=<none>";
        if (workspaceSwitchOnExit)
            out << " incomingWsDelta=" << vectorToString(incomingWorkspaceOffset) << " outgoingWsDelta=" << vectorToString(outgoingWorkspaceOffset);
        debugLog(out.str());
    }

    for (auto& managed : m_state.windows) {
        managed.exitGlobal = liveGlobalRectForWindow(managed.window);

        if (workspaceSwitchOnExit && managed.window && managed.window->m_workspace) {
            if (managed.window->m_workspace == predictedExitWorkspace) {
                const auto currentOffset = managed.window->m_workspace->m_renderOffset->value();
                const auto targetOffset = preferGoalGeometry ? Vector2D{} : incomingWorkspaceOffset;
                managed.exitGlobal = translateRect(managed.exitGlobal, targetOffset.x - currentOffset.x, targetOffset.y - currentOffset.y);
            } else if (managed.window->m_workspace == currentWorkspaceOnTargetMonitor) {
                if (preferGoalGeometry) {
                    const auto currentOffset = managed.window->m_workspace->m_renderOffset->value();
                    managed.exitGlobal =
                        translateRect(managed.exitGlobal, outgoingWorkspaceOffset.x - currentOffset.x, outgoingWorkspaceOffset.y - currentOffset.y);
                }
            }
        }

        if (!scrollingTranslation || !predictedExitFocus || !managed.window || !managed.window->m_isMapped)
            continue;

        if (managed.window->m_workspace != predictedExitFocus->m_workspace)
            continue;

        const auto layoutTarget = managed.window->layoutTarget();
        if (!layoutTarget || layoutTarget->floating())
            continue;

        managed.exitGlobal = translateRect(managed.exitGlobal, scrollingTranslation->x, scrollingTranslation->y);
    }

    if (preferGoalGeometry) {
        if (predictedPlaceholder && predictedPlaceholder->workspace == predictedPlaceholderWorkspace)
            (void)applyNiriScrollingCameraExitGeometry(*predictedPlaceholder);
        else
            (void)applyNiriScrollingCameraExitGeometry(predictedExitFocus);
    }
}
const OverviewController::EmptyWorkspacePlaceholder* OverviewController::centeredEmptyWorkspacePlaceholder(const State& state) const {
    if (!state.collectionPolicy.onlyActiveWorkspace || !niriModeAppliesToState(state) || !state.ownerMonitor)
        return nullptr;

    const Rect content = overviewContentRectForMonitor(state.ownerMonitor, state);
    if (content.width <= 0.0 || content.height <= 0.0)
        return nullptr;

    const double centerX = state.ownerMonitor->m_position.x + content.centerX();
    const double centerY = state.ownerMonitor->m_position.y + content.centerY();
    const EmptyWorkspacePlaceholder* best = nullptr;
    double bestDistance2 = std::numeric_limits<double>::max();

    for (const auto& placeholder : state.emptyWorkspacePlaceholders) {
        if (placeholder.backingOnly || !placeholder.monitor || placeholder.monitor != state.ownerMonitor || placeholder.workspaceId == WORKSPACE_INVALID)
            continue;
        if (placeholder.workspace && !isScrollingWorkspace(placeholder.workspace))
            continue;

        const double dx = placeholder.targetGlobal.centerX() - centerX;
        const double dy = placeholder.targetGlobal.centerY() - centerY;
        const double distance2 = dx * dx + dy * dy;
        if (distance2 < bestDistance2) {
            best = &placeholder;
            bestDistance2 = distance2;
        }
    }

    return best && bestDistance2 <= 4.0 ? best : nullptr;
}
bool OverviewController::syncScrollingWorkspaceSpotOnWindow(
    const PHLWINDOW& window, ScrollingSpotTargeting targeting, ScrollingSpotSyncIntent intent) const {
    if (!window || !window->m_isMapped || !window->m_workspace || !isScrollingWorkspace(window->m_workspace))
        return false;

    const bool closingDirectNiriOverview = m_beginCloseInProgress || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle;
    if (closingDirectNiriOverview && shouldPreserveDirectNiriEdgeCamera(window)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] sync scrolling workspace spot skipped (edge camera exit)"
                << " target=" << debugWindowLabel(window);
            debugLog(out.str());
        }
        return false;
    }

    const auto target = window->layoutTarget();
    if (!target || target->floating())
        return false;

    auto* scrolling = scrollingAlgorithmForWorkspace(window->m_workspace);
    if (!scrolling || !scrolling->m_scrollingData || !scrolling->m_scrollingData->controller)
        return false;

    const bool focusChange = intent == ScrollingSpotSyncIntent::FocusChange;
    const bool bypassScrollSyncSuppression = focusChange || m_applyingWorkspaceTransitionCommit ||
        (m_state.phase == Phase::Active && activeDirectNiriSingleWorkspaceOverview() && niriOverviewAnimationsEnabled());
    if (activeDirectNiriSingleWorkspaceOverview() && scrollingLiveCameraOwnsOverviewGeometry(scrolling) && !bypassScrollSyncSuppression) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] sync scrolling workspace spot skipped (native camera in flight)"
                << " target=" << debugWindowLabel(window);
            debugLog(out.str());
        }
        return false;
    }

    // Debounce rapid scroll sync calls (e.g., from spamming movecol).
    // If called too frequently, the Hyprland scrolling controller's animation
    // state becomes stale, causing jitter.
    constexpr std::chrono::milliseconds scrollSyncDebounce{50};
    const auto now = std::chrono::steady_clock::now();
    if (!bypassScrollSyncSuppression && now - m_lastScrollSyncTime < scrollSyncDebounce) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] sync scrolling workspace spot skipped (debounce)"
                << " target=" << debugWindowLabel(window)
                << " elapsed=" << std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastScrollSyncTime).count() << "ms";
            debugLog(out.str());
        }
        return false;
    }
    m_lastScrollSyncTime = now;

    const auto targetData = scrolling->dataFor(target);
    if (!targetData)
        return false;

    const auto column = targetData->column.lock();
    if (!column)
        return false;

    auto& data = scrolling->m_scrollingData;
    auto* const controller = data->controller.get();
    const auto columnIndex = data->idx(column);
    const auto offsetBefore = controller->getOffset();

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] sync scrolling workspace spot target=" << debugWindowLabel(window)
            << " workspace=" << debugWorkspaceLabel(window->m_workspace)
            << " live=" << rectToString(liveGlobalRectForWindow(window))
            << " goal=" << rectToString(goalGlobalRectForWindow(window))
            << " col=" << columnIndex
            << " offsetBefore=" << offsetBefore
            << " focusChange=" << (focusChange ? 1 : 0);
        debugLog(out.str());
        logScrollingWorkspaceSpotState("before explicit focus offset", window->m_workspace, window);
    }

    const CBox usable = scrolling->usableArea();
    const bool fullscreenOnOne = getConfigInt(m_handle, "scrolling:fullscreen_on_one_column", 1) != 0;
    const double maxExtent = controller->calculateMaxExtent(usable, fullscreenOnOne);
    double requestedOffset = offsetBefore;

    column->lastFocusedTarget = targetData;
    if (targeting == ScrollingSpotTargeting::Center || getConfigInt(m_handle, "scrolling:focus_fit_method", 0) != 1) {
        data->centerCol(column);
    } else {
        data->fitCol(column);
    }
    requestedOffset = controller->getOffset();

    const bool allowNativeAnimation = isVisible() && activeDirectNiriSingleWorkspaceOverview() && niriOverviewAnimationsEnabled();
    data->recalculate(!allowNativeAnimation);

    if (const auto monitor = window->m_workspace->m_monitor.lock())
        g_layoutManager->recalculateMonitor(monitor);

    if (g_pAnimationManager)
        g_pAnimationManager->frameTick();

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] sync scrolling workspace spot result=" << debugWindowLabel(window)
            << " workspace=" << debugWorkspaceLabel(window->m_workspace)
            << " live=" << rectToString(liveGlobalRectForWindow(window))
            << " goal=" << rectToString(goalGlobalRectForWindow(window))
            << " col=" << columnIndex
            << " requested=" << requestedOffset
            << " offsetAfter=" << controller->getOffset()
            << " maxExtent=" << maxExtent;
        debugLog(out.str());
        logScrollingWorkspaceSpotState("after explicit focus offset", window->m_workspace, window);
    }

    return true;
}
void OverviewController::refreshExitLayoutForFocus(const PHLWINDOW& window) const {
    if (!window || !window->m_isMapped)
        return;

    std::vector<PHLMONITOR> monitors;
    const auto addMonitor = [&](const PHLMONITOR& monitor) {
        if (!monitor)
            return;
        if (std::ranges::find(monitors, monitor) == monitors.end())
            monitors.push_back(monitor);
    };

    for (const auto& monitor : m_state.participatingMonitors)
        addMonitor(monitor);
    addMonitor(window->m_monitor.lock());
    addMonitor(previewMonitorForWindow(window));

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] refresh exit layout target=" << debugWindowLabel(window);
        if (monitors.empty()) {
            out << " monitors=<none>";
        } else {
            out << " monitors=";
            for (std::size_t i = 0; i < monitors.size(); ++i) {
                out << (i == 0 ? "" : ",") << monitors[i]->m_name;
            }
        }
        debugLog(out.str());
    }

    (void)syncScrollingWorkspaceSpotOnWindow(window);

    for (const auto& monitor : monitors)
        g_layoutManager->recalculateMonitor(monitor);
}
void OverviewController::syncRealFocusDuringOverview(
    const PHLWINDOW& window, bool syncScrollingSpot, const PreviewRectSnapshot* previousPreviewRects, bool forceRealFocus) {
    if (!window || !window->m_isMapped || !hasManagedWindow(window))
        return;

    const bool suppressSwapColumnFollowupScroll = syncScrollingSpot && shouldSuppressSwapColumnFollowupFocusScroll(window);
    const auto workspace = window->m_pinned ? activeLayoutWorkspace() : window->m_workspace;
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] sync real focus request"
            << " target=" << debugWindowLabel(window)
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " syncScrollingSpot=" << (syncScrollingSpot ? 1 : 0)
            << " suppressSwapcolScroll=" << (suppressSwapColumnFollowupScroll ? 1 : 0)
            << " forceRealFocus=" << (forceRealFocus ? 1 : 0)
            << " shouldSyncRealFocus=" << (shouldSyncRealFocusDuringOverview() ? 1 : 0)
            << " active=" << debugWindowLabel(Desktop::focusState()->window());
        debugLog(out.str());
        logSwapColumnFollowupState("sync-real-focus-request", workspace, "focus-sync", window);
    }

    const auto syncSpotWithoutFocusChange = [&]() {
        if (!syncScrollingSpot || suppressSwapColumnFollowupScroll)
            return;

        if (syncScrollingWorkspaceSpotOnWindow(window, ScrollingSpotTargeting::Configured, ScrollingSpotSyncIntent::FocusChange) &&
            shouldSyncScrollingLayoutDuringOverviewFocus())
            refreshNiriScrollingOverviewAfterLayoutScroll("focus-sync", previousPreviewRects);
    };

    if (!forceRealFocus && !shouldSyncRealFocusDuringOverview()) {
        syncSpotWithoutFocusChange();
        if (debugLogsEnabled()) {
            debugLog("[hymission] sync real focus early return reason=real-focus-disabled");
            logSwapColumnFollowupState("sync-real-focus-disabled", workspace, "focus-sync", window);
        }
        return;
    }

    if (Desktop::focusState()->window() == window) {
        syncSpotWithoutFocusChange();
        if (debugLogsEnabled()) {
            debugLog("[hymission] sync real focus early return reason=already-focused");
            logSwapColumnFollowupState("sync-real-focus-already-focused", workspace, "focus-sync", window);
        }
        return;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] sync real focus during overview target=" << debugWindowLabel(window);
        const auto activeBefore = Desktop::focusState()->window();
        if (activeBefore)
            out << " activeBefore=" << debugWindowLabel(activeBefore);
        else
            out << " activeBefore=<null>";
        if (window->m_workspace)
            out << " targetWorkspace=" << debugWorkspaceLabel(window->m_workspace);
        if (activeBefore && activeBefore->m_workspace)
            out << " activeWorkspaceBefore=" << debugWorkspaceLabel(activeBefore->m_workspace);
        debugLog(out.str());
        if (window->m_workspace && isScrollingWorkspace(window->m_workspace))
            logScrollingWorkspaceSpotState("before live focus", window->m_workspace, window);
    }

    if (suppressSwapColumnFollowupScroll && debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] suppress swapcol follow-up focus scroll"
            << " target=" << debugWindowLabel(window)
            << " workspace=" << debugWorkspaceLabel(window->m_workspace);
        debugLog(out.str());
    }

    const bool syncOverviewScrollingSpot = syncScrollingSpot && !suppressSwapColumnFollowupScroll && shouldSyncScrollingLayoutDuringOverviewFocus();
    const bool keepNativeAnimations = syncOverviewScrollingSpot && niriOverviewAnimationsEnabled();
    const bool temporarilyDisabledAnimations = !keepNativeAnimations && !m_animationsEnabledOverridden;
    if (temporarilyDisabledAnimations)
        setAnimationsEnabledOverride(true);

    m_pendingLiveFocusWorkspaceChangeTarget = window;
    focusWindowCompat(window, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    if (debugLogsEnabled() && window->m_workspace && isScrollingWorkspace(window->m_workspace))
        logScrollingWorkspaceSpotState("after focus before explicit spot sync", window->m_workspace, window);
    if (syncOverviewScrollingSpot)
        (void)syncScrollingWorkspaceSpotOnWindow(
            window, ScrollingSpotTargeting::Configured, ScrollingSpotSyncIntent::FocusChange);
    if (g_pAnimationManager)
        g_pAnimationManager->frameTick();
    if (syncOverviewScrollingSpot)
        refreshNiriScrollingOverviewAfterLayoutScroll("focus-sync", previousPreviewRects);
    if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == window)
        m_pendingLiveFocusWorkspaceChangeTarget.reset();

    if (temporarilyDisabledAnimations)
        setAnimationsEnabledOverride(false);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] sync real focus result=";
        const auto activeAfter = Desktop::focusState()->window();
        if (activeAfter)
            out << debugWindowLabel(activeAfter);
        else
            out << "<null>";
        if (activeAfter && activeAfter->m_workspace)
            out << " workspace=" << debugWorkspaceLabel(activeAfter->m_workspace);
        if (const auto monitor = Desktop::focusState()->monitor(); monitor)
            out << " activeWorkspaceOnFocusMonitor=" << debugWorkspaceLabel(monitor->m_activeWorkspace);
        debugLog(out.str());
        logSwapColumnFollowupState("sync-real-focus-result", workspace, "focus-sync", window);
    }
}
void OverviewController::syncFocusDuringOverviewFromSelection(bool syncScrollingSpot, const char* source, bool centerCursor) {
    const auto selected = selectedWindow();
    if (!selected)
        return;

    const auto previousSelected = m_state.focusDuringOverview;
    const bool capturePreviousRects = syncScrollingSpot && shouldSyncScrollingLayoutDuringOverviewFocus();
    const auto previousPreviewRects = capturePreviousRects ? captureCurrentPreviewRects() : PreviewRectSnapshot{};
    if (m_state.focusDuringOverview != selected && debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview target " << debugWindowLabel(selected) << " source=" << (source ? source : "?");
        if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
            out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        else
            out << " selected=<null>";
        if (m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size())
            out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        else
            out << " hovered=<null>";
        out << " pointer=" << vectorToString(g_pInputManager->getMouseCoordsInternal());
        debugLog(out.str());
    }

    m_state.focusDuringOverview = selected;
    latchHoverSelectionAnchor(g_pInputManager->getMouseCoordsInternal());
    m_queuedOverviewSelectionTarget.reset();
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewSelectionCenterCursor = false;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusCenterCursor = false;
    syncRealFocusDuringOverview(selected, syncScrollingSpot, capturePreviousRects ? &previousPreviewRects : nullptr);
    if (centerCursor)
        centerCursorOnOverviewWindow(selected, source);
    updateSelectedWindowLayout(previousSelected);
}
bool OverviewController::refocusDirectNiriSelectionWithoutScroll(const char* source) {
    if (!activeDirectNiriSingleWorkspaceOverview())
        return false;

    auto target = directNiriFocusedOverviewWindow(m_state);
    if (!target || !target->m_isMapped || !hasManagedWindow(target))
        return false;

    if (!selectWindowInState(m_state, target))
        return false;

    syncFocusDuringOverviewFromSelection(false, source);
    damageOwnedMonitors();
    return true;
}
void OverviewController::armPendingSwapColumnRelayoutCommit(const PHLWORKSPACE& workspace) {
    if (!workspace || !activeDirectNiriSingleWorkspaceOverview() || !m_state.relayoutActive)
        return;

    m_pendingSwapColumnRelayoutCommitWorkspace = workspace;
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] arm pending swapcol relayout commit"
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " relayoutActive=" << (m_state.relayoutActive ? 1 : 0)
            << " relayoutProgress=" << m_state.relayoutProgress;
        debugLog(out.str());
        logSwapColumnFollowupState("arm-pending-swapcol-relayout", workspace, "swapcol", selectedWindow());
    }
}
bool OverviewController::hasPendingSwapColumnRelayoutCommit(const PHLWORKSPACE& workspace) const {
    const auto pendingWorkspace = m_pendingSwapColumnRelayoutCommitWorkspace.lock();
    return pendingWorkspace && workspace && pendingWorkspace == workspace;
}
bool OverviewController::commitPendingSwapColumnRelayout(const char* source) {
    const auto workspace = m_pendingSwapColumnRelayoutCommitWorkspace.lock();
    if (!workspace)
        return false;

    m_pendingSwapColumnRelayoutCommitWorkspace.reset();

    if (!activeDirectNiriSingleWorkspaceOverview() || m_state.phase != Phase::Active || activeLayoutWorkspace() != workspace)
        return false;

    freezeSwapColumnBackendPreview(workspace, source);

    for (auto& managed : m_state.windows)
        managed.relayoutFromGlobal = managed.targetGlobal;

    for (auto& placeholder : m_state.emptyWorkspacePlaceholders)
        placeholder.relayoutFromGlobal = placeholder.targetGlobal;

    const bool wasActive = m_state.relayoutActive;
    m_state.relayoutActive = false;
    m_state.relayoutProgress = 1.0;
    m_state.relayoutStart = {};

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] commit pending swapcol relayout"
            << " source=" << (source ? source : "?")
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " wasActive=" << (wasActive ? 1 : 0);
        debugLog(out.str());
    }

    if (wasActive)
        damageOwnedMonitors();

    return wasActive;
}
void OverviewController::freezeSwapColumnBackendPreview(const PHLWORKSPACE& workspace, const char* source) {
    if (!workspace || !activeDirectNiriSingleWorkspaceOverview() || m_state.phase != Phase::Active || activeLayoutWorkspace() != workspace)
        return;

    m_swapColumnBackendPreviewFreezeWorkspace = workspace;
    m_swapColumnBackendPreviewFreezeUntil =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(std::ceil(RELAYOUT_DURATION_MS)));
    m_swapColumnBackendPreviewFrozenLayout.clear();
    for (const auto& managed : m_state.windows) {
        if (shouldCarryFrozenSwapColumnBackendPreview(managed, workspace))
            m_swapColumnBackendPreviewFrozenLayout.push_back(managed);
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] freeze swapcol backend preview"
            << " source=" << (source ? source : "?")
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " frozen=" << m_swapColumnBackendPreviewFrozenLayout.size();
        debugLog(out.str());
    }
}
bool OverviewController::swapColumnBackendPreviewFreezeActiveFor(const PHLWORKSPACE& workspace) const {
    const auto frozenWorkspace = m_swapColumnBackendPreviewFreezeWorkspace.lock();
    if (!workspace || !frozenWorkspace || m_swapColumnBackendPreviewFreezeUntil == std::chrono::steady_clock::time_point{} ||
        std::chrono::steady_clock::now() >= m_swapColumnBackendPreviewFreezeUntil)
        return false;

    if (!activeDirectNiriSingleWorkspaceOverview() || m_state.phase != Phase::Active || activeLayoutWorkspace() != frozenWorkspace)
        return false;

    return frozenWorkspace == workspace;
}
bool OverviewController::swapColumnBackendPreviewFrozenFor(const ManagedWindow& window) const {
    const auto workspace = m_swapColumnBackendPreviewFreezeWorkspace.lock();
    if (!swapColumnBackendPreviewFreezeActiveFor(workspace))
        return false;

    if (!window.window || !window.window->m_isMapped)
        return false;

    if (window.window->m_workspace == workspace)
        return true;

    return (window.window->m_pinned || window.isPinned) && activeLayoutWorkspace() == workspace;
}
bool OverviewController::pendingSwapColumnRelayoutOwnsPreviewFor(const ManagedWindow& window) const {
    if (!window.window || !window.window->m_isMapped)
        return false;

    const auto workspace = window.window->m_pinned ? activeLayoutWorkspace() : window.window->m_workspace;
    if (!workspace || !hasPendingSwapColumnRelayoutCommit(workspace))
        return false;

    return shouldCarryFrozenSwapColumnBackendPreview(window, workspace);
}
const OverviewController::ManagedWindow* OverviewController::frozenSwapColumnBackendPreviewManagedFor(const PHLWINDOW& window) const {
    if (!window)
        return nullptr;

    const auto it = std::find_if(m_swapColumnBackendPreviewFrozenLayout.begin(), m_swapColumnBackendPreviewFrozenLayout.end(),
                                 [&](const ManagedWindow& managed) { return managed.window == window; });
    return it == m_swapColumnBackendPreviewFrozenLayout.end() ? nullptr : &*it;
}
bool OverviewController::shouldCarryFrozenSwapColumnBackendPreview(const ManagedWindow& managed, const PHLWORKSPACE& workspace) const {
    if (!workspace || !managed.window || !managed.window->m_isMapped || managed.window->m_workspace != workspace)
        return false;

    if (managed.window->m_pinned || managed.isPinned || managed.isFloating || managed.isNiriFloatingOverlay || isFloatingOverviewWindow(managed.window))
        return false;

    const auto target = managed.window->layoutTarget();
    return target && !target->floating();
}
bool OverviewController::shouldSuppressSwapColumnFollowupFocusScroll(const PHLWINDOW& window) const {
    if (!window || !window->m_isMapped)
        return false;

    PHLWORKSPACE workspace = window->m_pinned ? activeLayoutWorkspace() : window->m_workspace;
    if (!workspace || !isScrollingWorkspace(workspace))
        return false;

    return swapColumnBackendPreviewFreezeActiveFor(workspace);
}
bool OverviewController::carryFrozenSwapColumnBackendPreviewLayout(ManagedWindow& managed, std::size_t index, const PHLWORKSPACE& workspace) const {
    if (!swapColumnBackendPreviewFreezeActiveFor(workspace) || !shouldCarryFrozenSwapColumnBackendPreview(managed, workspace))
        return false;

    const auto* frozen = frozenSwapColumnBackendPreviewManagedFor(managed.window);
    if (!frozen)
        return false;

    managed.targetMonitor = frozen->targetMonitor ? frozen->targetMonitor : managed.targetMonitor;
    managed.targetGlobal = frozen->targetGlobal;
    managed.relayoutFromGlobal = frozen->targetGlobal;
    managed.exitGlobal = frozen->exitGlobal;
    managed.slot = frozen->slot;
    managed.slot.index = index;
    if (managed.targetMonitor)
        managed.slot.target = makeRect(managed.targetGlobal.x - managed.targetMonitor->m_position.x,
                                       managed.targetGlobal.y - managed.targetMonitor->m_position.y,
                                       managed.targetGlobal.width,
                                       managed.targetGlobal.height);

    return true;
}
bool OverviewController::carryFrozenSwapColumnBackendPreviewLayout(State& state, const PHLWORKSPACE& workspace, const char* source) const {
    if (!swapColumnBackendPreviewFreezeActiveFor(workspace) || m_swapColumnBackendPreviewFrozenLayout.empty())
        return false;

    std::size_t carried = 0;
    for (std::size_t index = 0; index < state.windows.size(); ++index) {
        if (carryFrozenSwapColumnBackendPreviewLayout(state.windows[index], index, workspace))
            ++carried;
    }

    if (carried == 0)
        return false;

    state.slots.clear();
    for (const auto& managed : state.windows) {
        if (managed.targetMonitor)
            state.slots.push_back(managed.slot);
    }

    for (auto& placeholder : state.emptyWorkspacePlaceholders)
        placeholder.relayoutFromGlobal = placeholder.targetGlobal;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] carry frozen swapcol backend preview"
            << " source=" << (source ? source : "?")
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " windows=" << carried;
        debugLog(out.str());
    }

    return true;
}
void OverviewController::centerCursorOnOverviewWindow(const PHLWINDOW& window, const char* source) {
    if (!window || !g_pCompositor || getConfigInt(m_handle, "plugin:hymission:overview_center_cursor_on_hover_focus", 1) == 0)
        return;

    if (!isVisible() || m_state.phase != Phase::Active || !usesDirectNiriScrollingOverview(m_state))
        return;

    if (niriOverviewAnimationsEnabled())
        return;

    const auto* managed = managedWindowFor(window);
    if (!managed)
        return;

    const Rect    preview = m_state.relayoutActive ? managed->targetGlobal : currentPreviewRect(*managed);
    const Vector2D center{preview.centerX(), preview.centerY()};

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] center cursor on hover focus target=" << debugWindowLabel(window)
            << " source=" << (source ? source : "?")
            << " point=" << vectorToString(center)
            << " preview=" << rectToString(preview);
        debugLog(out.str());
    }

    g_pCompositor->warpCursorTo(center);
    latchHoverSelectionAnchor(center);
    updateHoveredFromPointer(false, false, false, false, "center-cursor-hover-focus");
}
void OverviewController::logSwapColumnFollowupState(const char* context, const PHLWORKSPACE& workspace, const char* source,
                                                    const PHLWINDOW& focusWindow) const {
    if (!debugLogsEnabled())
        return;

    const auto pendingWorkspace = m_pendingSwapColumnRelayoutCommitWorkspace.lock();
    const auto freezeWorkspace = m_swapColumnBackendPreviewFreezeWorkspace.lock();
    const auto now = std::chrono::steady_clock::now();
    const auto freezeRemainingMs = m_swapColumnBackendPreviewFreezeUntil != std::chrono::steady_clock::time_point{}
        ? std::chrono::duration_cast<std::chrono::milliseconds>(m_swapColumnBackendPreviewFreezeUntil - now).count()
        : 0;
    auto* const scrolling = workspace ? scrollingAlgorithmForWorkspace(workspace) : nullptr;
    const auto* const scrollingData = scrolling ? scrolling->m_scrollingData.get() : nullptr;

    const auto phaseName = [&]() {
        switch (m_state.phase) {
            case Phase::Inactive: return "inactive";
            case Phase::Opening: return "opening";
            case Phase::Active: return "active";
            case Phase::ClosingSettle: return "closing-settle";
            case Phase::Closing: return "closing";
        }
        return "unknown";
    };

    std::ostringstream out;
    out << "[hymission] swapcol follow-up state"
        << " context=" << (context ? context : "?")
        << " source=" << (source ? source : "?")
        << " workspace=" << debugWorkspaceLabel(workspace)
        << " activeWorkspace=" << debugWorkspaceLabel(activeLayoutWorkspace())
        << " phase=" << phaseName()
        << " activeDirect=" << (activeDirectNiriSingleWorkspaceOverview() ? 1 : 0)
        << " relayoutActive=" << (m_state.relayoutActive ? 1 : 0)
        << " relayoutProgress=" << m_state.relayoutProgress
        << " pendingWorkspace=" << debugWorkspaceLabel(pendingWorkspace)
        << " pendingSame=" << (pendingWorkspace && workspace && pendingWorkspace == workspace ? 1 : 0)
        << " freezeWorkspace=" << debugWorkspaceLabel(freezeWorkspace)
        << " freezeSame=" << (freezeWorkspace && workspace && freezeWorkspace == workspace ? 1 : 0)
        << " freezeActive=" << (swapColumnBackendPreviewFreezeActiveFor(workspace) ? 1 : 0)
        << " freezeRemainingMs=" << freezeRemainingMs
        << " frozenWindows=" << m_swapColumnBackendPreviewFrozenLayout.size()
        << " columns=" << (scrollingData ? scrollingData->columns.size() : 0)
        << " offset=" << (scrollingData && scrollingData->controller ? scrollingData->controller->getOffset() : 0.0)
        << " selected=" << debugWindowLabel(selectedWindow())
        << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview)
        << " focusWindow=" << debugWindowLabel(focusWindow)
        << " activeWindow=" << debugWindowLabel(Desktop::focusState()->window());
    debugLog(out.str());
}
void OverviewController::logScrollingWorkspaceSpotState(const char* context, const PHLWORKSPACE& workspace, const PHLWINDOW& focusWindow) const {
    if (!debugLogsEnabled() || !workspace)
        return;

    auto* scrolling = scrollingAlgorithmForWorkspace(workspace);
    if (!scrolling || !scrolling->m_scrollingData || !scrolling->m_scrollingData->controller) {
        std::ostringstream out;
        out << "[hymission] scrolling spot dump context=" << (context ? context : "?")
            << " workspace=" << debugWorkspaceLabel(workspace) << " unavailable=1";
        debugLog(out.str());
        return;
    }

    const auto& data = scrolling->m_scrollingData;
    std::ostringstream summary;
    summary << "[hymission] scrolling spot dump context=" << (context ? context : "?")
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " visible=" << (workspace->isVisible() ? 1 : 0)
            << " columns=" << data->columns.size()
            << " offset=" << data->controller->getOffset();
    if (data->lockedCameraOffset)
        summary << " lockedOffset=" << *data->lockedCameraOffset;
    else
        summary << " lockedOffset=<null>";
    if (focusWindow)
        summary << " focus=" << debugWindowLabel(focusWindow);
    const auto activeWindow = Desktop::focusState()->window();
    if (activeWindow)
        summary << " active=" << debugWindowLabel(activeWindow);
    else
        summary << " active=<null>";
    debugLog(summary.str());

    std::ostringstream columns;
    columns << "[hymission] scrolling spot columns context=" << (context ? context : "?");
    for (std::size_t columnIndex = 0; columnIndex < data->columns.size(); ++columnIndex) {
        const auto& column = data->columns[columnIndex];
        if (!column) {
            columns << " | col#" << columnIndex << " <null-column>";
            continue;
        }

        const auto lastFocusedTarget = column->lastFocusedTarget.lock();
        columns << " | col#" << columnIndex << " width=" << column->getColumnWidth() << " targets=" << column->targetDatas.size();
        if (lastFocusedTarget) {
            const auto lastTarget = lastFocusedTarget->target.lock();
            columns << " lastFocused=" << debugWindowLabel(lastTarget ? lastTarget->window() : PHLWINDOW{});
        } else {
            columns << " lastFocused=<null>";
        }

        for (std::size_t targetIndex = 0; targetIndex < column->targetDatas.size(); ++targetIndex) {
            const auto& targetData = column->targetDatas[targetIndex];
            const auto target = targetData ? targetData->target.lock() : SP<Layout::ITarget>{};
            const auto window = target ? target->window() : PHLWINDOW{};

            columns << " [" << targetIndex << ']';
            if (targetData && targetData == lastFocusedTarget)
                columns << '*';
            if (window == focusWindow)
                columns << '!';
            columns << debugWindowLabel(window);
            if (target)
                columns << " pos=" << boxToString(target->position());
            if (targetData)
                columns << " layout=" << boxToString(targetData->layoutBox);
        }
    }
    debugLog(columns.str());
}

SDispatchResult OverviewController::layoutMessageDispatcherHook(std::string args) {
    return runOverviewEditingDispatcher("layoutmsg", &m_layoutMessageOriginal, std::move(args));
}
SDispatchResult OverviewController::moveFocusDispatcherHook(std::string args) {
    if (!m_moveFocusOriginal)
        return {};

    if (activeDirectNiriSingleWorkspaceOverview() || timedNiriSingleWorkspaceTransitionActive())
        return runOverviewEditingDispatcher("movefocus", &m_moveFocusOriginal, std::move(args));

    auto previousFocusMonitor = focusMonitorForWindow(Desktop::focusState()->window());
    if (!previousFocusMonitor)
        previousFocusMonitor = Desktop::focusState()->monitor();

    const auto result = m_moveFocusOriginal(std::move(args));
    const bool syncScrollingSpot = !shouldSuppressNiriFocusScrollForMonitorReturn(Desktop::focusState()->window(), previousFocusMonitor);
    refreshNiriScrollingOverviewAfterFocusDispatcher("movefocus", {}, syncScrollingSpot);
    if (debugLogsEnabled() && usesDirectNiriScrollingOverview(m_state)) {
        std::ostringstream out;
        out << "[hymission] movefocus native result"
            << " success=" << (result.success ? 1 : 0)
            << " syncScrollingSpot=" << (syncScrollingSpot ? 1 : 0)
            << " selected=" << debugWindowLabel(selectedWindow())
            << " active=" << debugWindowLabel(Desktop::focusState()->window());
        debugLog(out.str());
        logSwapColumnFollowupState("movefocus-native-result", activeLayoutWorkspace(), "movefocus", selectedWindow());
    }
    return result;
}

Config::Actions::ActionResult OverviewController::moveToWorkspaceActionHook(PHLWORKSPACE workspace, bool silent, std::optional<PHLWINDOW> window) {
    if (!m_moveToWorkspaceActionOriginal)
        return {};

    const auto movedWindow = window.value_or(Desktop::focusState()->window());
    const bool canRouteThroughOverviewTransition = !silent && !m_overviewEditingDispatcherInProgress && workspace && !workspace->m_isSpecialWorkspace &&
        isVisible() && m_state.phase == Phase::Active && activeDirectNiriSingleWorkspaceOverview() && movedWindow && movedWindow == selectedWindow() &&
        movedWindow->m_isMapped && hasManagedWindow(movedWindow);
    if (!canRouteThroughOverviewTransition)
        return m_moveToWorkspaceActionOriginal(std::move(workspace), silent, std::move(window));

    const auto legacyDispatcher = m_overviewEditingDispatchersOriginal.find("movetoworkspace");
    if (legacyDispatcher == m_overviewEditingDispatchersOriginal.end())
        return m_moveToWorkspaceActionOriginal(std::move(workspace), silent, std::move(window));

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] intercept direct niri move-to-workspace action"
            << " target=" << debugWorkspaceLabel(workspace)
            << " window=" << debugWindowLabel(movedWindow);
        debugLog(out.str());
    }

    const auto result = runOverviewEditingDispatcher("movetoworkspace", &legacyDispatcher->second, std::to_string(workspace->m_id));
    if (result.success)
        return {};

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] direct niri move-to-workspace action fallback"
            << " target=" << debugWorkspaceLabel(workspace)
            << " error=" << result.error;
        debugLog(out.str());
    }

    return m_moveToWorkspaceActionOriginal(std::move(workspace), silent, std::move(window));
}

std::optional<SDispatchResult> OverviewController::tryRunDirectNiriMoveToWorkspaceDispatcher(const std::string& args, const PHLWINDOW& selectedBefore) {
    std::string workspaceArgs = args;
    PHLWINDOW   movedWindow = selectedBefore;
    if (const auto separator = args.find_last_of(','); separator != std::string::npos) {
        movedWindow = g_pCompositor->getWindowByRegex(args.substr(separator + 1));
        workspaceArgs = args.substr(0, separator);
    }

    const auto sourceWorkspace = movedWindow ? movedWindow->m_workspace : PHLWORKSPACE{};
    const auto sourceMonitor = sourceWorkspace ? sourceWorkspace->m_monitor.lock() : PHLMONITOR{};
    const auto targetSpec = getWorkspaceIDNameFromString(workspaceArgs);
    const auto silentDispatcher = m_overviewEditingDispatchersOriginal.find("movetoworkspacesilent");
    const bool canPrepareWorkspaceTransition = movedWindow && movedWindow->m_isMapped && hasManagedWindow(movedWindow) && sourceWorkspace && sourceMonitor &&
        sourceMonitor == m_state.ownerMonitor && targetSpec.id != WORKSPACE_INVALID && !targetSpec.name.starts_with("special:") &&
        silentDispatcher != m_overviewEditingDispatchersOriginal.end();
    if (!canPrepareWorkspaceTransition)
        return std::nullopt;

    auto       targetWorkspace = targetSpec.id == WORKSPACE_INVALID ? PHLWORKSPACE{} : g_pCompositor->getWorkspaceByID(targetSpec.id);
    if (!targetWorkspace && sourceMonitor && targetSpec.id != WORKSPACE_INVALID && !targetSpec.name.starts_with("special:")) {
        const std::string targetName = targetSpec.name.empty() ? std::to_string(targetSpec.id) : targetSpec.name;
        targetWorkspace = g_pCompositor->createNewWorkspace(targetSpec.id, sourceMonitor->m_id, targetName, false);
    }
    const auto targetMonitor = targetWorkspace ? targetWorkspace->m_monitor.lock() : PHLMONITOR{};
    const bool canOwnWorkspaceTransition = targetWorkspace && !targetWorkspace->m_isSpecialWorkspace && sourceWorkspace != targetWorkspace && targetMonitor &&
        sourceMonitor == targetMonitor;
    if (!canOwnWorkspaceTransition)
        return std::nullopt;

    // Hyprland can destroy the source workspace immediately when its last
    // window is moved out. Keep that lane alive as a synthetic overview
    // placeholder until overview closes, otherwise later rebuilds drop it.
    niri_scrolling_detail::retainDirectNiriWorkspaceLane(sourceMonitor, sourceWorkspace);

    const auto* previousManaged = managedWindowFor(m_state, movedWindow, true);
    const float movedPreviewAlpha = previousManaged ? previousManaged->previewAlpha : 1.0F;
    const ScopedFlag dispatchGuard(m_overviewEditingDispatcherInProgress);
    selectWindowInState(m_state, movedWindow);
    m_state.focusDuringOverview = movedWindow;
    if (Desktop::focusState()->window() != movedWindow)
        focusWindowCompat(movedWindow, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    State sourceState = captureOverviewWorkspaceTransitionSourceState();

    SDispatchResult result;
    {
        // Hyprland's silent move refocuses a window under the moved window's
        // old position. Keep that intermediate focus out of overview state;
        // this transition explicitly owns focus for the moved window.
        const ScopedFlag applyingWorkspaceTransitionCommit(m_applyingWorkspaceTransitionCommit);
        result = silentDispatcher->second(args);
    }
    if (!result.success)
        return result;

    selectWindowInState(m_state, movedWindow);
    m_state.focusDuringOverview = movedWindow;

    // Keep the pre-dispatch target workspace viewport in the source transition
    // state.  movetoworkspace makes that workspace occupied only after the
    // silent dispatcher runs; deleting its old empty viewport here leaves the
    // wallpaper renderer with a target-only lane, so it enters from the native
    // full-workspace offset and snaps at the end instead of scrolling with the
    // overview rows.
    targetWorkspace->m_lastFocusedWindow = movedWindow;
    if (!beginOverviewWorkspaceTransition(sourceMonitor, targetWorkspace->m_id, targetWorkspace->m_name, targetWorkspace, false,
                                          WorkspaceTransitionMode::TimedCommit, std::move(sourceState), movedWindow))
        return result;

    // Same reason as above: the source transition state must retain the
    // target workspace's pre-move empty viewport so source and target lanes can
    // be matched by workspace id and interpolated row-to-row.  The target state
    // may still contain a stale non-backing placeholder for the now-occupied
    // workspace, so only clean that side.
    (void)removeOccupiedWorkspacePlaceholder(m_workspaceTransition.targetState, movedWindow);

    const auto placeholderForWorkspace = [&](State& state, WORKSPACEID workspaceId) -> EmptyWorkspacePlaceholder* {
        EmptyWorkspacePlaceholder* fallback = nullptr;
        for (auto& placeholder : state.emptyWorkspacePlaceholders) {
            if (placeholder.monitor != sourceMonitor || placeholder.workspaceId != workspaceId)
                continue;

            if (!fallback)
                fallback = &placeholder;
            if (placeholder.backingOnly)
                return &placeholder;
        }

        return fallback;
    };
    const auto appendSyntheticTransitionPlaceholder = [&](State& state, const EmptyWorkspacePlaceholder& model, const PHLWORKSPACE& workspace,
                                                          double dx, double dy) {
        EmptyWorkspacePlaceholder placeholder = model;
        placeholder.workspace = workspace;
        placeholder.workspaceId = workspace ? workspace->m_id : model.workspaceId;
        placeholder.targetGlobal = translateRect(model.targetGlobal, dx, dy);
        placeholder.relayoutFromGlobal = placeholder.targetGlobal;
        state.emptyWorkspacePlaceholders.push_back(std::move(placeholder));
    };

    auto* sourceSourcePlaceholder = placeholderForWorkspace(m_workspaceTransition.sourceState, sourceWorkspace->m_id);
    auto* targetSourcePlaceholder = placeholderForWorkspace(m_workspaceTransition.targetState, sourceWorkspace->m_id);
    auto* sourceTargetPlaceholder = placeholderForWorkspace(m_workspaceTransition.sourceState, targetWorkspace->m_id);
    auto* targetTargetPlaceholder = placeholderForWorkspace(m_workspaceTransition.targetState, targetWorkspace->m_id);

    if (!sourceTargetPlaceholder && targetTargetPlaceholder && sourceSourcePlaceholder && targetSourcePlaceholder) {
        const double dx = sourceSourcePlaceholder->targetGlobal.x - targetSourcePlaceholder->targetGlobal.x;
        const double dy = sourceSourcePlaceholder->targetGlobal.y - targetSourcePlaceholder->targetGlobal.y;
        appendSyntheticTransitionPlaceholder(m_workspaceTransition.sourceState, *targetTargetPlaceholder, targetWorkspace, dx, dy);

        // Re-read pointers after appending, because the placeholder vectors may
        // have reallocated.
        sourceSourcePlaceholder = placeholderForWorkspace(m_workspaceTransition.sourceState, sourceWorkspace->m_id);
        targetSourcePlaceholder = placeholderForWorkspace(m_workspaceTransition.targetState, sourceWorkspace->m_id);
        sourceTargetPlaceholder = placeholderForWorkspace(m_workspaceTransition.sourceState, targetWorkspace->m_id);
        targetTargetPlaceholder = placeholderForWorkspace(m_workspaceTransition.targetState, targetWorkspace->m_id);
    }
    if (!targetSourcePlaceholder && sourceSourcePlaceholder && sourceTargetPlaceholder && targetTargetPlaceholder) {
        const double dx = targetTargetPlaceholder->targetGlobal.x - sourceTargetPlaceholder->targetGlobal.x;
        const double dy = targetTargetPlaceholder->targetGlobal.y - sourceTargetPlaceholder->targetGlobal.y;
        appendSyntheticTransitionPlaceholder(m_workspaceTransition.targetState, *sourceSourcePlaceholder, sourceWorkspace, dx, dy);
    }

    const auto targetManaged = std::find_if(m_workspaceTransition.targetState.windows.begin(), m_workspaceTransition.targetState.windows.end(),
                                            [&](const ManagedWindow& managed) { return managed.window == movedWindow; });
    if (targetManaged != m_workspaceTransition.targetState.windows.end()) {
        targetManaged->previewAlpha = movedPreviewAlpha;
        selectWindowInState(m_workspaceTransition.targetState, movedWindow);
        m_workspaceTransition.targetState.focusDuringOverview = movedWindow;
        m_state.focusDuringOverview = movedWindow;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] direct niri workspace move transition"
            << " window=" << debugWindowLabel(movedWindow)
            << " source=" << debugWorkspaceLabel(sourceWorkspace)
            << " target=" << debugWorkspaceLabel(targetWorkspace)
            << " sourceWindows=" << m_workspaceTransition.sourceState.windows.size()
            << " targetWindows=" << m_workspaceTransition.targetState.windows.size();
        debugLog(out.str());
    }

    damageOwnedMonitors();
    return result;
}

SDispatchResult OverviewController::runOverviewEditingDispatcher(const char* dispatcherName, DispatcherHandler* original, std::string args) {
    if (!original || !*original)
        return {};

    const bool overviewActive = isVisible() && m_state.phase == Phase::Active;
    const std::string dispatcherNameLower = asciiLowerCopy(dispatcherName ? std::string(dispatcherName) : std::string{});
    const std::string dispatcherArgsLower = asciiLowerCopy(trimCopy(args));
    const bool isLayoutMessageDispatcher = dispatcherNameLower == "layoutmsg" || dispatcherNameLower == "layout";
    const bool isSwapColumnLayoutMessage = isLayoutMessageDispatcher &&
        (dispatcherArgsLower == "swapcol" || dispatcherArgsLower.starts_with("swapcol ") || dispatcherArgsLower.starts_with("swapcol,") ||
         dispatcherArgsLower.find("swapcol") != std::string::npos || dispatcherArgsLower == "swap col" ||
         dispatcherArgsLower.starts_with("swap col ") || dispatcherArgsLower.starts_with("swap col,") ||
         dispatcherArgsLower == "swap +col" || dispatcherArgsLower.starts_with("swap +col ") || dispatcherArgsLower.starts_with("swap +col,") ||
         dispatcherArgsLower == "swap -col" || dispatcherArgsLower.starts_with("swap -col ") || dispatcherArgsLower.starts_with("swap -col,"));
    const bool isMoveColumnLayoutMessage = isLayoutMessageDispatcher &&
        (dispatcherArgsLower == "movecol" || dispatcherArgsLower.starts_with("movecol ") || dispatcherArgsLower.starts_with("movecol,") ||
         dispatcherArgsLower == "move +col" || dispatcherArgsLower.starts_with("move +col ") || dispatcherArgsLower.starts_with("move +col,") ||
         dispatcherArgsLower == "move col" || dispatcherArgsLower.starts_with("move col ") || dispatcherArgsLower.starts_with("move col,") ||
         dispatcherArgsLower == "move -col" || dispatcherArgsLower.starts_with("move -col ") || dispatcherArgsLower.starts_with("move -col,"));
    const bool isResizeColumnLayoutMessage = isLayoutMessageDispatcher &&
        (dispatcherArgsLower == "resizecol" || dispatcherArgsLower.starts_with("resizecol ") || dispatcherArgsLower.starts_with("resizecol,") ||
         dispatcherArgsLower == "resize +col" || dispatcherArgsLower.starts_with("resize +col ") || dispatcherArgsLower.starts_with("resize +col,") ||
         dispatcherArgsLower == "resize col" || dispatcherArgsLower.starts_with("resize col ") || dispatcherArgsLower.starts_with("resize col,") ||
         dispatcherArgsLower == "resize -col" || dispatcherArgsLower.starts_with("resize -col ") || dispatcherArgsLower.starts_with("resize -col,") ||
         dispatcherArgsLower.find("resizecol") != std::string::npos || dispatcherArgsLower.find("resize col") != std::string::npos ||
         dispatcherArgsLower.find("colresize") != std::string::npos);
    const bool isDirectMoveColumnDispatcher = dispatcherNameLower == "movecol" || dispatcherNameLower == "movecolumn";
    const bool isDirectSwapColumnDispatcher = dispatcherNameLower == "swapcol" || dispatcherNameLower == "swapcolumn";
    const bool isDirectResizeColumnDispatcher = dispatcherNameLower == "resizecol" || dispatcherNameLower == "resizecolumn";
    const bool isDirectResizeActiveDispatcher = dispatcherNameLower == "resizeactive" || dispatcherNameLower.starts_with("resizeactive") ||
        dispatcherNameLower.find("window.resize") != std::string::npos;
    const bool isMoveFocusDispatcher = dispatcherNameLower == "movefocus";
    const bool isMoveToWorkspaceDispatcher = dispatcherNameLower == "movetoworkspace" ||
        (dispatcherNameLower.find("window.workspace") != std::string::npos && dispatcherNameLower.find("silent") == std::string::npos);
    const bool isFocusOrMovementDispatcher = isMoveFocusDispatcher || isMoveColumnLayoutMessage || isSwapColumnLayoutMessage ||
        isResizeColumnLayoutMessage || isDirectMoveColumnDispatcher || isDirectSwapColumnDispatcher || isDirectResizeColumnDispatcher ||
        isDirectResizeActiveDispatcher;

    const bool niriSingleWorkspaceTransition = timedNiriSingleWorkspaceTransitionActive();
    const auto transitionAction = resolveOverviewEditTransitionAction(
        m_workspaceTransition.active,
        isFocusOrMovementDispatcher,
        m_workspaceTransition.mode == WorkspaceTransitionMode::TimedCommit,
        niriSingleWorkspaceTransition);
    const bool retargetTimedNiriTransition = transitionAction == OverviewEditTransitionAction::Retarget ||
        (niriSingleWorkspaceTransition && isMoveToWorkspaceDispatcher);

    clearExpiredOverviewOpenInputBarrier();
    clearExpiredOverviewHeavyEditInputBarrier();
    const bool heavyDelayedOpenEditDispatcher = isSwapColumnLayoutMessage || isResizeColumnLayoutMessage || isDirectSwapColumnDispatcher ||
        isDirectResizeColumnDispatcher || isDirectResizeActiveDispatcher;
    const bool openVisibilityAnimationActive =
        m_overviewVisibilityAnimation && m_overviewVisibilityAnimation->isBeingAnimated();
    const bool openDispatcherCooldownActive = overviewOpenInputBarrierActive();
    const bool heavyOpenDispatcherCooldownActive = heavyDelayedOpenEditDispatcher && overviewHeavyEditInputBarrierActive();
    const bool workspaceSwitchDispatcherCooldownActive =
        niri_scrolling_detail::workspaceSwitchDispatcherBlockUntil != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() < niri_scrolling_detail::workspaceSwitchDispatcherBlockUntil;
    const bool workspaceSwitchSettling = isVisible() && m_state.collectionPolicy.onlyActiveWorkspace &&
        (m_state.phase != Phase::Active || openVisibilityAnimationActive || m_postOpenRefreshFrames > 0 || m_workspaceSwipeGesture.active ||
         (m_workspaceTransition.active && !retargetTimedNiriTransition) ||
         (m_workspaceTransitionCommitScheduled && !retargetTimedNiriTransition) || m_applyingWorkspaceTransitionCommit ||
         (niri_scrolling_detail::workspaceSwitchDispatcherBlockRelayout && m_state.relayoutActive && niriModeAppliesToState(m_state)) ||
         openDispatcherCooldownActive || heavyOpenDispatcherCooldownActive || workspaceSwitchDispatcherCooldownActive);
    if (!workspaceSwitchSettling && niri_scrolling_detail::workspaceSwitchDispatcherBlockUntil != std::chrono::steady_clock::time_point{}) {
        niri_scrolling_detail::workspaceSwitchDispatcherBlockUntil = {};
        niri_scrolling_detail::workspaceSwitchDispatcherBlockRelayout = false;
    }

    if (workspaceSwitchSettling) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] block overview edit dispatcher during workspace switch settle"
                << " dispatcher=" << dispatcherNameLower
                << " args=" << dispatcherArgsLower
                << " phase=" << static_cast<int>(m_state.phase)
                << " openAnim=" << (openVisibilityAnimationActive ? 1 : 0)
                << " openBarrier=" << (openDispatcherCooldownActive ? 1 : 0)
                << " heavyBarrier=" << (heavyOpenDispatcherCooldownActive ? 1 : 0)
                << " workspaceCooldown=" << (workspaceSwitchDispatcherCooldownActive ? 1 : 0)
                << " postOpenFrames=" << m_postOpenRefreshFrames;
            debugLog(out.str());
        }
        return {};
    }

    if (retargetTimedNiriTransition) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] retarget workspace transition for edit dispatcher"
                << " dispatcher=" << dispatcherNameLower
                << " args=" << dispatcherArgsLower;
            debugLog(out.str());
        }
        commitActiveNiriWorkspaceTransitionForRetarget();
    }

    if (isFocusOrMovementDispatcher && (transitionAction == OverviewEditTransitionAction::Defer || m_workspaceTransition.active)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] block edit dispatcher during workspace transition"
                << " dispatcher=" << dispatcherNameLower
                << " args=" << dispatcherArgsLower
                << " transitionMode=" << (m_workspaceTransition.mode == WorkspaceTransitionMode::Gesture ? "gesture"
                    : m_workspaceTransition.mode == WorkspaceTransitionMode::TimedCommit ? "commit"
                    : m_workspaceTransition.mode == WorkspaceTransitionMode::TimedRevert ? "revert" : "unknown");
            debugLog(out.str());
        }
        return {};
    }

    if (m_workspaceTransition.active && m_workspaceTransition.mode == WorkspaceTransitionMode::TimedCommit && !isFocusOrMovementDispatcher)
        commitActiveNiriWorkspaceTransitionForRetarget();

    PHLWINDOW selectedBefore = overviewActive ? selectedWindow() : PHLWINDOW{};

    const auto clearDirectNiriEdgeCameraFocusState = [&](const char* source) {
        m_state.selectedIndex.reset();
        m_state.focusDuringOverview.reset();
        m_queuedOverviewSelectionTarget.reset();
        m_queuedOverviewSelectionSyncScrollingSpot = false;
        m_queuedOverviewSelectionCenterCursor = false;
        m_queuedOverviewLiveFocusTarget.reset();
        m_queuedOverviewLiveFocusSyncScrollingSpot = false;
        m_queuedOverviewLiveFocusCenterCursor = false;
        m_hoverSelectionRetargetCandidateIndex.reset();
        m_hoverSelectionRetargetCandidateSince = {};
        m_hoverSelectionRetargetCandidatePrimed = false;
        g_multiColumnEdgeFocusOverrideUntil = {};
        Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] niri edge camera released overview focus"
                << " source=" << (source ? source : "?")
                << " selectedBefore=" << debugWindowLabel(selectedBefore)
                << " activeWorkspace=" << debugWorkspaceLabel(activeLayoutWorkspace());
            debugLog(out.str());
        }
    };

    const auto edgeCameraWorkspaceBefore = activeLayoutWorkspace();
    auto* const edgeCameraScrollingBefore = edgeCameraWorkspaceBefore && isScrollingWorkspace(edgeCameraWorkspaceBefore) ?
        scrollingAlgorithmForWorkspace(edgeCameraWorkspaceBefore) : nullptr;
    const bool directEdgeCameraBefore = overviewActive && activeDirectNiriSingleWorkspaceOverview() && scrollingEdgeCameraActive(edgeCameraScrollingBefore);
    const bool edgeMoveColumnTowardEdge = directEdgeCameraBefore && (isMoveColumnLayoutMessage || isDirectMoveColumnDispatcher) &&
        moveColumnCommandTargetsEdge(edgeCameraScrollingBefore, dispatcherNameLower, dispatcherArgsLower);
    const bool edgeMoveColumnAwayFromEdge = directEdgeCameraBefore && (isMoveColumnLayoutMessage || isDirectMoveColumnDispatcher) && !edgeMoveColumnTowardEdge;
    const bool leafMoveColumnTowardEdge = !directEdgeCameraBefore && (isMoveColumnLayoutMessage || isDirectMoveColumnDispatcher) &&
        moveColumnCommandLeavesFocusedColumn(edgeCameraScrollingBefore, selectedBefore, dispatcherNameLower, dispatcherArgsLower);
    const bool preserveNativeEdgeCameraFocusRelease = directEdgeCameraBefore && (isMoveColumnLayoutMessage || isDirectMoveColumnDispatcher) &&
        !edgeMoveColumnAwayFromEdge;
    const bool nativeEdgeCameraTransition = overviewActive && activeDirectNiriSingleWorkspaceOverview() &&
        (leafMoveColumnTowardEdge || edgeMoveColumnAwayFromEdge);
    const bool preserveNativeEdgeCameraDispatchFocus = preserveNativeEdgeCameraFocusRelease || edgeMoveColumnAwayFromEdge;

    if (debugLogsEnabled() && overviewActive && (isMoveColumnLayoutMessage || isDirectMoveColumnDispatcher)) {
        std::ostringstream out;
        out << "[hymission] niri movecol edge classification"
            << " directEdgeBefore=" << (directEdgeCameraBefore ? 1 : 0)
            << " leafTowardEdge=" << (leafMoveColumnTowardEdge ? 1 : 0)
            << " edgeTowardEdge=" << (edgeMoveColumnTowardEdge ? 1 : 0)
            << " edgeAway=" << (edgeMoveColumnAwayFromEdge ? 1 : 0)
            << " nativeTransition=" << (nativeEdgeCameraTransition ? 1 : 0)
            << " relayoutActive=" << (m_state.relayoutActive ? 1 : 0);
        debugLog(out.str());
    }

    if (directEdgeCameraBefore && (isMoveFocusDispatcher || edgeMoveColumnTowardEdge)) {
        clearDirectNiriEdgeCameraFocusState(edgeMoveColumnTowardEdge ? "movecol-edge-noop" : "movefocus-edge-noop");
        damageOwnedMonitors();
        return {};
    }

    if (preserveNativeEdgeCameraFocusRelease)
        clearDirectNiriEdgeCameraFocusState("movecol-edge-preserve-before-dispatch");

    if (overviewActive && isFocusOrMovementDispatcher && !preserveNativeEdgeCameraDispatchFocus && m_state.collectionPolicy.onlyActiveWorkspace &&
        niriModeAppliesToState(m_state)) {
        const auto dispatchWorkspace = activeLayoutWorkspace();
        const auto validTiledDispatchWindow = [&](const PHLWINDOW& window) {
            return window && window->m_isMapped && !window->m_pinned && !isFloatingOverviewWindow(window) && window->m_workspace == dispatchWorkspace;
        };

        const bool stateWorkspaceStale = dispatchWorkspace && m_state.ownerWorkspace && m_state.ownerWorkspace != dispatchWorkspace;
        const bool selectedStale = selectedBefore && !validTiledDispatchWindow(selectedBefore);
        const bool focusStale = m_state.focusDuringOverview && !validTiledDispatchWindow(m_state.focusDuringOverview);
        if (dispatchWorkspace && isScrollingWorkspace(dispatchWorkspace) && (stateWorkspaceStale || selectedStale || focusStale || !validTiledDispatchWindow(selectedBefore))) {
            PHLWINDOW liveFocus = Desktop::focusState()->window();
            if (!validTiledDispatchWindow(liveFocus))
                liveFocus = focusCandidateForWorkspace(dispatchWorkspace);

            if (validTiledDispatchWindow(liveFocus)) {
                const bool needsStateRebuild = stateWorkspaceStale || !hasManagedWindow(liveFocus);
                if (needsStateRebuild && !insideRenderLifecycle())
                    rebuildVisibleState(liveFocus, true);

                selectWindowInState(m_state, liveFocus);
                m_state.focusDuringOverview = liveFocus;
                selectedBefore = liveFocus;
                (void)syncScrollingWorkspaceSpotOnWindow(liveFocus);

                if (debugLogsEnabled()) {
                    std::ostringstream out;
                    out << "[hymission] rebase overview edit focus to active workspace"
                        << " dispatcher=" << dispatcherNameLower
                        << " args=" << dispatcherArgsLower
                        << " workspace=" << debugWorkspaceLabel(dispatchWorkspace)
                        << " focus=" << debugWindowLabel(liveFocus)
                        << " rebuilt=" << (needsStateRebuild ? 1 : 0);
                    debugLog(out.str());
                }
            } else {
                if (debugLogsEnabled()) {
                    std::ostringstream out;
                    out << "[hymission] block overview edit: no valid tiled focus on active workspace"
                        << " dispatcher=" << dispatcherNameLower
                        << " args=" << dispatcherArgsLower
                        << " workspace=" << debugWorkspaceLabel(dispatchWorkspace);
                    debugLog(out.str());
                }
                return {};
            }
        }
    }

    const bool isScrollingGeometryLayoutMessage = isLayoutMessageDispatcher &&
        (isResizeColumnLayoutMessage || dispatcherArgsLower.find("colresize") != std::string::npos || dispatcherArgsLower.find("fit") != std::string::npos ||
         dispatcherArgsLower.find("promote") != std::string::npos || dispatcherArgsLower.find("expel") != std::string::npos ||
         dispatcherArgsLower.find("consume") != std::string::npos || isSwapColumnLayoutMessage);
    const bool forceGeometryRefocus =
        dispatcherNameLower == "resizeactive" || dispatcherNameLower == "resizecol" || dispatcherNameLower == "resizecolumn" || dispatcherNameLower == "togglefloating" || dispatcherNameLower == "setfloating" ||
        dispatcherNameLower == "settiled" || dispatcherNameLower == "pin" || dispatcherNameLower.starts_with("resizewindow") || dispatcherNameLower.starts_with("resizecol") || dispatcherNameLower.starts_with("resizecolumn") ||
        dispatcherNameLower.starts_with("togglefloating") || dispatcherNameLower.starts_with("setfloating") || dispatcherNameLower.starts_with("settiled") ||
        dispatcherNameLower.starts_with("pin") || dispatcherNameLower.find("window.resize") != std::string::npos ||
        dispatcherNameLower.find("window.float") != std::string::npos || dispatcherNameLower.find("window.pin") != std::string::npos ||
        isScrollingGeometryLayoutMessage;

    const bool directLiveGeometryAvailable = overviewActive && activeDirectNiriSingleWorkspaceOverview() &&
        std::ranges::all_of(m_state.windows, [&](const ManagedWindow& managed) {
            return !managed.window || !managed.window->m_isMapped || static_cast<bool>(livePreviewRectForManagedWindow(managed));
        });
    const bool directNiriFocusOrColumnRelayout = overviewActive && activeDirectNiriSingleWorkspaceOverview() &&
        (dispatcherNameLower == "movefocus" || isMoveColumnLayoutMessage || isSwapColumnLayoutMessage || isResizeColumnLayoutMessage ||
         isDirectMoveColumnDispatcher || isDirectSwapColumnDispatcher || isDirectResizeColumnDispatcher || isDirectResizeActiveDispatcher);
    const bool directNiriColumnRelayout = overviewActive && activeDirectNiriSingleWorkspaceOverview() &&
        (isMoveColumnLayoutMessage || isSwapColumnLayoutMessage || isResizeColumnLayoutMessage ||
         isDirectMoveColumnDispatcher || isDirectSwapColumnDispatcher || isDirectResizeColumnDispatcher || isDirectResizeActiveDispatcher);
    const bool animateDirectStripRelayout = niriOverviewAnimationsEnabled() &&
        ((directLiveGeometryAvailable && directNiriFocusOrColumnRelayout) || directNiriColumnRelayout);
    const bool commitActiveStripRelayout = animateDirectStripRelayout && m_state.phase == Phase::Active && m_state.relayoutActive;
    const auto directStripPreviewRects = commitActiveStripRelayout ? commitActiveNiriRelayoutForRetarget() :
        (animateDirectStripRelayout ? captureCurrentPreviewRects() : PreviewRectSnapshot{});
    const auto* const directStripRelayoutOrigins = animateDirectStripRelayout ? &directStripPreviewRects : nullptr;

    const auto retainVisibleDirectNiriWorkspaceLanes = [&] {
        if (!overviewActive || !usesDirectNiriScrollingOverview(m_state))
            return;

        std::vector<std::pair<MONITORID, WORKSPACEID>> retained;
        const auto alreadyRetained = [&](MONITORID monitorId, WORKSPACEID workspaceId) {
            return std::ranges::any_of(retained, [&](const auto& entry) {
                return entry.first == monitorId && entry.second == workspaceId;
            });
        };
        const auto retainId = [&](const PHLMONITOR& laneMonitor, WORKSPACEID workspaceId, const std::string& workspaceName = std::string{}) {
            if (!laneMonitor || workspaceId == WORKSPACE_INVALID || alreadyRetained(laneMonitor->m_id, workspaceId))
                return;

            retained.emplace_back(laneMonitor->m_id, workspaceId);
            niri_scrolling_detail::retainDirectNiriWorkspaceLaneId(laneMonitor, workspaceId, workspaceName);
        };
        const auto retainWorkspace = [&](const PHLWORKSPACE& workspace) {
            if (!workspace || workspace->m_isSpecialWorkspace || workspace->m_id == WORKSPACE_INVALID)
                return;

            retainId(workspace->m_monitor.lock(), workspace->m_id, workspace->m_name);
        };

        retainWorkspace(m_state.ownerWorkspace);
        retainWorkspace(activeLayoutWorkspace());
        retainWorkspace(selectedBefore ? selectedBefore->m_workspace : PHLWORKSPACE{});
        if (const auto focused = Desktop::focusState()->window(); focused)
            retainWorkspace(focused->m_workspace);

        for (const auto& managed : m_state.windows) {
            if (!managed.window)
                continue;

            if (managed.window->m_pinned) {
                retainWorkspace(m_state.ownerWorkspace);
                continue;
            }

            retainWorkspace(managed.window->m_workspace);
        }

        for (const auto& placeholder : m_state.emptyWorkspacePlaceholders) {
            if (!placeholder.monitor || placeholder.workspaceId == WORKSPACE_INVALID)
                continue;

            std::string workspaceName;
            if (placeholder.workspace)
                workspaceName = placeholder.workspace->m_name;
            retainId(placeholder.monitor, placeholder.workspaceId, workspaceName);
        }
    };

    const bool runDirectNiriDispatcherPath = directLiveGeometryAvailable || directNiriFocusOrColumnRelayout;

    if (overviewActive && activeDirectNiriSingleWorkspaceOverview() && isMoveToWorkspaceDispatcher) {
        retainVisibleDirectNiriWorkspaceLanes();

        if (const auto result = tryRunDirectNiriMoveToWorkspaceDispatcher(args, selectedBefore); result)
            return *result;

        PHLWINDOW retainedMoveSource = selectedBefore;
        if (const auto separator = args.find_last_of(','); separator != std::string::npos)
            retainedMoveSource = g_pCompositor->getWindowByRegex(args.substr(separator + 1));

        const auto retainedSourceWorkspace = retainedMoveSource ? retainedMoveSource->m_workspace : PHLWORKSPACE{};
        const auto retainedSourceMonitor = retainedSourceWorkspace ? retainedSourceWorkspace->m_monitor.lock() : PHLMONITOR{};
        niri_scrolling_detail::retainDirectNiriWorkspaceLane(retainedSourceMonitor, retainedSourceWorkspace);
    }

    if (runDirectNiriDispatcherPath) {
        retainVisibleDirectNiriWorkspaceLanes();

        const auto dispatchWorkspace = activeLayoutWorkspace();
        const auto validDispatchFocus = [&](const PHLWINDOW& window) {
            if (!window || !window->m_isMapped || !hasManagedWindow(window))
                return false;
            if (!m_state.collectionPolicy.onlyActiveWorkspace || !dispatchWorkspace)
                return true;
            if (window->m_pinned)
                return false;
            return window->m_workspace == dispatchWorkspace;
        };

        PHLWINDOW dispatchFocus;
        if (!preserveNativeEdgeCameraDispatchFocus) {
            dispatchFocus = validDispatchFocus(selectedBefore) ? selectedBefore : PHLWINDOW{};
            if (!validDispatchFocus(dispatchFocus))
                dispatchFocus = validDispatchFocus(m_state.focusDuringOverview) ? m_state.focusDuringOverview : PHLWINDOW{};
            if (!validDispatchFocus(dispatchFocus))
                dispatchFocus = validDispatchFocus(Desktop::focusState()->window()) ? Desktop::focusState()->window() : PHLWINDOW{};
            if (!validDispatchFocus(dispatchFocus) && dispatchWorkspace)
                dispatchFocus = focusCandidateForWorkspace(dispatchWorkspace);
        }

        const auto activeDispatchWorkspace = activeLayoutWorkspace();
        const auto dispatchTarget = dispatchFocus ? dispatchFocus->layoutTarget() : nullptr;
        const bool dispatchFocusIsTiledScrolling = dispatchFocus && dispatchFocus->m_isMapped && !dispatchFocus->m_pinned && hasManagedWindow(dispatchFocus) &&
            dispatchFocus->m_workspace && dispatchFocus->m_workspace == activeDispatchWorkspace && isScrollingWorkspace(dispatchFocus->m_workspace) &&
            dispatchTarget && !dispatchTarget->floating() && !isFloatingOverviewWindow(dispatchFocus);

        if ((isMoveFocusDispatcher || isMoveColumnLayoutMessage || isSwapColumnLayoutMessage || isDirectSwapColumnDispatcher ||
             isResizeColumnLayoutMessage || isDirectResizeColumnDispatcher || isDirectResizeActiveDispatcher) &&
            !dispatchFocusIsTiledScrolling && !preserveNativeEdgeCameraDispatchFocus) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] consume niri edit dispatcher without tiled focus"
                    << " dispatcher=" << dispatcherName
                    << " selected=" << debugWindowLabel(selectedBefore)
                    << " dispatchFocus=" << debugWindowLabel(dispatchFocus)
                    << " active=" << debugWindowLabel(Desktop::focusState()->window())
                    << " workspace=" << debugWorkspaceLabel(activeDispatchWorkspace);
                debugLog(out.str());
            }
            damageOwnedMonitors();
            return {};
        }

        SDispatchResult result;
        {
            const ScopedFlag dispatchGuard(m_overviewEditingDispatcherInProgress);
            if (dispatchFocus && dispatchFocus->m_isMapped && hasManagedWindow(dispatchFocus)) {
                selectWindowInState(m_state, dispatchFocus);
                m_state.focusDuringOverview = dispatchFocus;
                if (Desktop::focusState()->window() != dispatchFocus)
                    focusWindowCompat(dispatchFocus, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            }

            result = (*original)(std::move(args));
        }
        if (!result.success)
            return result;

        PHLWINDOW preferred = Desktop::focusState()->window();
        const auto preferredWorkspace = activeLayoutWorkspace();
        const bool multiColumnEdgeCameraAfter = preferredWorkspace && isScrollingWorkspace(preferredWorkspace) && directNiriEdgeCameraActive() &&
            directNiriScrollingColumnCount(preferredWorkspace) != 1;
        const bool movingIntoOrStayingAtEdge = leafMoveColumnTowardEdge || edgeMoveColumnTowardEdge || preserveNativeEdgeCameraFocusRelease;
        const bool nativeEdgeCameraFocusReleased = (isMoveColumnLayoutMessage || isDirectMoveColumnDispatcher) && multiColumnEdgeCameraAfter &&
            (movingIntoOrStayingAtEdge || (!preferred && !edgeMoveColumnAwayFromEdge));
        if (nativeEdgeCameraFocusReleased) {
            clearDirectNiriEdgeCameraFocusState("movecol-edge-release-after-dispatch");
            preferred = PHLWINDOW{};
            // Match Hyprland's scrolling movecol edge behavior: once native
            // layout scrolls past the last column it calls fullWindowFocus(nullptr).
            // Re-selecting dispatchFocus here pins the overview to the last window
            // and makes the edge-camera pan snap after an in-flight windowsMove.
            m_state.selectedIndex.reset();
            m_state.focusDuringOverview.reset();
            m_queuedOverviewSelectionTarget.reset();
            m_queuedOverviewSelectionSyncScrollingSpot = false;
            m_queuedOverviewSelectionCenterCursor = false;
            m_queuedOverviewLiveFocusTarget.reset();
            m_queuedOverviewLiveFocusSyncScrollingSpot = false;
            m_queuedOverviewLiveFocusCenterCursor = false;

            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] niri movecol preserved native edge-camera focus release"
                    << " dispatcher=" << dispatcherName
                    << " workspace=" << debugWorkspaceLabel(preferredWorkspace)
                    << " previous=" << debugWindowLabel(dispatchFocus)
                    << " leafTowardEdge=" << (leafMoveColumnTowardEdge ? 1 : 0)
                    << " edgeTowardEdge=" << (edgeMoveColumnTowardEdge ? 1 : 0)
                    << " edgeAway=" << (edgeMoveColumnAwayFromEdge ? 1 : 0)
                    << " multiColumnEdgeAfter=" << (multiColumnEdgeCameraAfter ? 1 : 0);
                debugLog(out.str());
            }
        }
        const auto validPreferred = [&](const PHLWINDOW& window) {
            if (!window || !window->m_isMapped)
                return false;
            if (!m_state.collectionPolicy.onlyActiveWorkspace || !preferredWorkspace)
                return true;
            return !window->m_pinned && window->m_workspace == preferredWorkspace;
        };
        if (!nativeEdgeCameraFocusReleased) {
            if (!validPreferred(preferred))
                preferred = validPreferred(dispatchFocus) ? dispatchFocus : PHLWINDOW{};
            if (!validPreferred(preferred) && preferredWorkspace)
                preferred = focusCandidateForWorkspace(preferredWorkspace);

            if (edgeMoveColumnAwayFromEdge && validPreferred(preferred)) {
                if (Desktop::focusState()->window() != preferred)
                    focusWindowCompat(preferred, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
                selectWindowInState(m_state, preferred);
                m_state.focusDuringOverview = preferred;
                g_multiColumnEdgeFocusOverrideUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(320);

                if (debugLogsEnabled()) {
                    std::ostringstream out;
                    out << "[hymission] niri edge camera restored leaf focus"
                        << " dispatcher=" << dispatcherName
                        << " workspace=" << debugWorkspaceLabel(preferredWorkspace)
                        << " focus=" << debugWindowLabel(preferred);
                    debugLog(out.str());
                }
            }
        }

        if (insideRenderLifecycle()) {
            scheduleVisibleStateRebuild();
        } else {
            // Keep post-dispatch relayout behavior aligned with movecol.
            // Movecol does not force a focus-fit/center sync here; doing that for
            // swapcol or resize dispatchers snaps the scrolling camera after a
            // workspace switch and skips the relayout animation.
            const bool swapColumnDispatcher = isSwapColumnLayoutMessage || isDirectSwapColumnDispatcher;
            const bool resizeColumnDispatcher = isResizeColumnLayoutMessage || isDirectResizeColumnDispatcher;
            const bool resizeActiveDispatcher = isDirectResizeActiveDispatcher;
            const bool movecolStyleRelayout = isMoveColumnLayoutMessage || isDirectMoveColumnDispatcher ||
                swapColumnDispatcher || resizeColumnDispatcher || resizeActiveDispatcher;
            const char* relayoutSource = nativeEdgeCameraFocusReleased ? "movecol-edge-release" :
                (edgeMoveColumnAwayFromEdge ? "movecol-edge-return" :
                 (movecolStyleRelayout ? "movecol" : "movefocus"));

            if (nativeEdgeCameraTransition) {
                if (debugLogsEnabled())
                    debugLog("[hymission] niri movecol edge transition retargeted overview relayout");
                refreshNiriScrollingOverviewAfterLayoutScroll(relayoutSource, directStripRelayoutOrigins);
            } else if (nativeEdgeCameraFocusReleased) {
                refreshNiriScrollingOverviewAfterLayoutScroll(relayoutSource, directStripRelayoutOrigins);
            } else {
                // Match movecol exactly for swapcol / resizecol / resizeactive here:
                // do not force a focus-fit/center sync after the native dispatcher.
                // Forcing that sync snaps the scrolling layout camera and skips the
                // relayout animation after a workspace switch. Let the native
                // dispatcher own the layout motion, then rebuild the overview from
                // the captured preview origins just like movecol.
                if (isMoveFocusDispatcher)
                    (void)syncScrollingWorkspaceSpotOnWindow(
                        preferred, ScrollingSpotTargeting::Configured, ScrollingSpotSyncIntent::FocusChange);
                refreshVisibleStateMetadata(preferred, directStripRelayoutOrigins, relayoutSource);
            }

            // Ensure overview focus state matches the new focused window before
            // refreshing the layout scroll.
            if (preferred && preferred->m_isMapped && hasManagedWindow(preferred)) {
                selectWindowInState(m_state, preferred);
                m_state.focusDuringOverview = preferred;
            }

        }

        damageOwnedMonitors();
        return result;
    }

    if (debugLogsEnabled() && overviewActive && (isSwapColumnLayoutMessage || isMoveColumnLayoutMessage)) {
        const auto workspace = activeLayoutWorkspace();
        std::ostringstream out;
        out << "[hymission] overview column edit before pending commit"
            << " edit=" << (isSwapColumnLayoutMessage ? "swapcol" : "movecol")
            << " args=" << dispatcherArgsLower
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " pendingSwapcol=" << (hasPendingSwapColumnRelayoutCommit(workspace) ? 1 : 0)
            << " freezeActive=" << (swapColumnBackendPreviewFreezeActiveFor(workspace) ? 1 : 0)
            << " selected=" << debugWindowLabel(selectedBefore)
            << " active=" << debugWindowLabel(Desktop::focusState()->window());
        debugLog(out.str());
        logSwapColumnFollowupState("overview-edit-before-pending-commit", workspace, isSwapColumnLayoutMessage ? "swapcol" : "movecol", selectedBefore);
        logScrollingWorkspaceSpotState(isSwapColumnLayoutMessage ? "swapcol-before-pending-commit" : "movecol-before-pending-commit", workspace, selectedBefore);
    }

    if (overviewActive)
        (void)commitPendingSwapColumnRelayout("overview-edit-before-dispatch");

    retainVisibleDirectNiriWorkspaceLanes();

    const auto closestTiledWindowInWorkspace = [&](const PHLWINDOW& window) -> PHLWINDOW {
        if (!window || !usesDirectNiriScrollingOverview(m_state))
            return window;

        if (!window->m_pinned && !isFloatingOverviewWindow(window))
            return window;

        const auto workspace = window->m_pinned ? activeLayoutWorkspace() : window->m_workspace;
        if (!workspace || !isScrollingWorkspace(workspace))
            return window;

        const Rect sourceRect = liveGlobalRectForWindow(window);

        const auto overlapArea = [](const Rect& lhs, const Rect& rhs) {
            const double x1 = std::max(lhs.x, rhs.x);
            const double y1 = std::max(lhs.y, rhs.y);
            const double x2 = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
            const double y2 = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
            if (x2 <= x1 || y2 <= y1)
                return 0.0;
            return (x2 - x1) * (y2 - y1);
        };

        struct CandidateScore {
            PHLWINDOW window;
            double    overlap = 0.0;
            double    distance = std::numeric_limits<double>::infinity();
        };

        std::optional<CandidateScore> bestTiled;
        std::optional<CandidateScore> bestAny;

        const auto better = [](const CandidateScore& candidate, const std::optional<CandidateScore>& current) {
            if (!current)
                return true;
            if (candidate.overlap > current->overlap + 1.0)
                return true;
            if (candidate.overlap + 1.0 < current->overlap)
                return false;
            return candidate.distance < current->distance;
        };

        for (const auto& managed : m_state.windows) {
            const auto candidate = managed.window;
            if (!candidate || candidate == window || !candidate->m_isMapped || candidate->m_workspace != workspace || candidate->m_pinned)
                continue;

            const Rect candidateRect = liveGlobalRectForWindow(candidate);
            const double dx = candidateRect.centerX() - sourceRect.centerX();
            const double dy = candidateRect.centerY() - sourceRect.centerY();
            CandidateScore score{
                .window = candidate,
                .overlap = overlapArea(sourceRect, candidateRect),
                .distance = dx * dx + dy * dy,
            };

            const auto target = candidate->layoutTarget();
            const bool candidateIsTiled = target && !target->floating() && !isFloatingOverviewWindow(candidate);

            if (candidateIsTiled && better(score, bestTiled))
                bestTiled = score;

            if (better(score, bestAny))
                bestAny = score;
        }

        if (bestTiled)
            return bestTiled->window;
        if (bestAny)
            return bestAny->window;
        return window;
    };

    const auto hardRecalculateScrollingWorkspaceAroundWindow = [&](const PHLWINDOW& anchor, const char* source) -> bool {
        if (!anchor || !anchor->m_isMapped || !hasManagedWindow(anchor) || !anchor->m_workspace || !isScrollingWorkspace(anchor->m_workspace))
            return false;

        auto* const scrolling = scrollingAlgorithmForWorkspace(anchor->m_workspace);
        const auto target = anchor->layoutTarget();
        const bool targetIsTiled = target && !target->floating();

        if (targetIsTiled) {
            if (const auto targetData = scrolling ? scrolling->dataFor(target) : nullptr; targetData) {
                if (const auto column = targetData->column.lock()) {
                    column->lastFocusedTarget = targetData;
                    if (scrolling->m_scrollingData && scrolling->m_scrollingData->controller) {
                        if (getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 1)
                            scrolling->m_scrollingData->fitCol(column);
                        else
                            scrolling->m_scrollingData->centerCol(column);
                    }
                }
            }
        }

        // This is the important part for colresize / regular resize while the overview is open:
        // force Hyprland's scrolling layout to commit fresh target boxes before we rebuild the
        // overview state.  The old path refreshed our cached snapshot, but it could still read
        // the pre-resize column positions until a later manual focus move.
        if (anchor->m_workspace->m_space)
            anchor->m_workspace->m_space->recalculate();
        if (scrolling && scrolling->m_scrollingData)
            scrolling->m_scrollingData->recalculate(true);
        (void)syncScrollingWorkspaceSpotOnWindow(anchor);
        if (anchor->m_workspace->m_space)
            anchor->m_workspace->m_space->recalculate();
        if (scrolling && scrolling->m_scrollingData)
            scrolling->m_scrollingData->recalculate(true);

        refreshWorkspaceLayoutSnapshot(anchor->m_workspace);
        if (const auto monitor = anchor->m_monitor.lock())
            g_layoutManager->recalculateMonitor(monitor);
        if (g_pAnimationManager)
            g_pAnimationManager->frameTick();

        m_stripSnapshotsDirty = true;
        scheduleWorkspaceStripSnapshotRefresh();
        refreshNiriScrollingOverviewAfterLayoutScroll(source);
        damageOwnedMonitors();
        return true;
    };

    const auto forceDirectNiriGeometryFocus = [&](const PHLWINDOW& anchor, const char* source) -> bool {
        if (!anchor || !anchor->m_isMapped || !hasManagedWindow(anchor) || !usesDirectNiriScrollingOverview(m_state))
            return false;

        selectWindowInState(m_state, anchor);
        m_state.focusDuringOverview = anchor;
        m_queuedOverviewSelectionTarget.reset();
        m_queuedOverviewSelectionSyncScrollingSpot = false;
        m_queuedOverviewSelectionCenterCursor = false;
        m_queuedOverviewLiveFocusTarget.reset();
        m_queuedOverviewLiveFocusSyncScrollingSpot = false;
        m_queuedOverviewLiveFocusCenterCursor = false;

        if (anchor->m_workspace) {
            m_pendingLiveFocusWorkspaceChangeTarget = anchor;
            (void)activateWindowWorkspaceForFocus(anchor);
        }

        // Do not route this through syncRealFocusDuringOverview().  That helper may
        // intentionally skip real focus changes depending on config, but resize / float /
        // pin needs the scrolling layout to behave exactly as if the user moved focus.
        focusWindowCompat(anchor, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
        if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == anchor)
            m_pendingLiveFocusWorkspaceChangeTarget.reset();

        return hardRecalculateScrollingWorkspaceAroundWindow(anchor, source);
    };

    if (overviewActive && selectedBefore) {
        m_state.focusDuringOverview = selectedBefore;
        syncRealFocusDuringOverview(selectedBefore, true);
        if (Desktop::focusState()->window() != selectedBefore) {
            m_pendingLiveFocusWorkspaceChangeTarget = selectedBefore;
            focusWindowCompat(selectedBefore, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == selectedBefore)
                m_pendingLiveFocusWorkspaceChangeTarget.reset();
        }
        (void)syncScrollingWorkspaceSpotOnWindow(selectedBefore);
    }

    if (debugLogsEnabled() && isSwapColumnLayoutMessage) {
        const auto workspace = activeLayoutWorkspace();
        auto* const scrolling = workspace && isScrollingWorkspace(workspace) ? scrollingAlgorithmForWorkspace(workspace) : nullptr;
        std::ostringstream out;
        out << "[hymission] swapcol dispatch begin"
            << " dispatcher=" << (dispatcherName ? dispatcherName : "?")
            << " args=" << dispatcherArgsLower
            << " overviewActive=" << (overviewActive ? 1 : 0)
            << " phase=" << static_cast<int>(m_state.phase)
            << " onlyActiveWorkspace=" << (m_state.collectionPolicy.onlyActiveWorkspace ? 1 : 0)
            << " directNiri=" << (usesDirectNiriScrollingOverview(m_state) ? 1 : 0)
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " scrolling=" << (scrolling ? 1 : 0)
            << " columns=" << (scrolling && scrolling->m_scrollingData ? scrolling->m_scrollingData->columns.size() : 0)
            << " selected=" << debugWindowLabel(selectedBefore)
            << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview);
        debugLog(out.str());
        if (workspace)
            logScrollingWorkspaceSpotState("swapcol-before-dispatch", workspace, selectedBefore);
    }

    if (debugLogsEnabled() && isMoveColumnLayoutMessage) {
        const auto workspace = activeLayoutWorkspace();
        auto* const scrolling = workspace && isScrollingWorkspace(workspace) ? scrollingAlgorithmForWorkspace(workspace) : nullptr;
        std::size_t originCount = 0;
        std::ostringstream origins;
        origins << "[hymission] movecol relayout origins";
        for (const auto& managed : m_state.windows) {
            if (!managed.window || !managed.window->m_isMapped || managed.window->m_workspace != workspace || managed.window->m_pinned ||
                isFloatingOverviewWindow(managed.window))
                continue;

            ++originCount;
            origins << " | " << debugWindowLabel(managed.window)
                    << " preview=" << rectToString(currentPreviewRect(managed))
                    << " target=" << rectToString(managed.targetGlobal)
                    << " relayoutFrom=" << rectToString(managed.relayoutFromGlobal);
        }

        std::ostringstream out;
        out << "[hymission] movecol dispatch begin"
            << " dispatcher=" << (dispatcherName ? dispatcherName : "?")
            << " args=" << dispatcherArgsLower
            << " overviewActive=" << (overviewActive ? 1 : 0)
            << " phase=" << static_cast<int>(m_state.phase)
            << " onlyActiveWorkspace=" << (m_state.collectionPolicy.onlyActiveWorkspace ? 1 : 0)
            << " directNiri=" << (usesDirectNiriScrollingOverview(m_state) ? 1 : 0)
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " scrolling=" << (scrolling ? 1 : 0)
            << " columns=" << (scrolling && scrolling->m_scrollingData ? scrolling->m_scrollingData->columns.size() : 0)
            << " focusFit=" << getConfigInt(m_handle, "scrolling:focus_fit_method", 0)
            << " relayoutOrigins=" << originCount
            << " selected=" << debugWindowLabel(selectedBefore)
            << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview);
        debugLog(out.str());
        debugLog(origins.str());
        logSwapColumnFollowupState("movecol-before-dispatch", workspace, "movecol", selectedBefore);
        if (workspace)
            logScrollingWorkspaceSpotState("movecol-before-dispatch", workspace, selectedBefore);
    }

    struct TwoColumnOverviewSwapCapture {
        struct WindowOrigin {
            PHLWINDOW window;
            Rect      rect;
            std::size_t groupIndex = 0;
        };

        PHLWORKSPACE workspace;
        std::array<Rect, 2> groupBounds{};
        std::vector<WindowOrigin> windows;
        bool valid = false;
    };

    const auto captureTwoColumnOverviewSwap = [&]() {
        TwoColumnOverviewSwapCapture capture;
        const auto logSkip = [&](std::string_view reason) {
            if (!debugLogsEnabled() || !isSwapColumnLayoutMessage)
                return;
            std::ostringstream out;
            out << "[hymission] swapcol exact-two capture skip reason=" << reason
                << " overviewActive=" << (overviewActive ? 1 : 0)
                << " onlyActiveWorkspace=" << (m_state.collectionPolicy.onlyActiveWorkspace ? 1 : 0)
                << " directNiri=" << (usesDirectNiriScrollingOverview(m_state) ? 1 : 0);
            debugLog(out.str());
        };

        if (!overviewActive || !isSwapColumnLayoutMessage || !m_state.collectionPolicy.onlyActiveWorkspace || !usesDirectNiriScrollingOverview(m_state)) {
            logSkip("scope");
            return capture;
        }

        const auto workspace = activeLayoutWorkspace();
        auto* const scrolling = workspace && isScrollingWorkspace(workspace) ? scrollingAlgorithmForWorkspace(workspace) : nullptr;
        if (!scrolling || !scrolling->m_scrollingData || scrolling->m_scrollingData->columns.size() != 2) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] swapcol exact-two capture skip reason=columns"
                    << " workspace=" << debugWorkspaceLabel(workspace)
                    << " scrolling=" << (scrolling ? 1 : 0)
                    << " columns=" << (scrolling && scrolling->m_scrollingData ? scrolling->m_scrollingData->columns.size() : 0);
                debugLog(out.str());
            }
            return capture;
        }

        if (debugLogsEnabled())
            armTwoColumnSwapTrace(workspace);

        struct Group {
            SP<Layout::Tiled::SColumnData> column;
            Rect                           bounds;
            bool                           hasBounds = false;
        };

        std::array<Group, 2> groups{{
            {.column = scrolling->m_scrollingData->columns[0]},
            {.column = scrolling->m_scrollingData->columns[1]},
        }};

        const auto expandGroupBounds = [](Group& group, const Rect& rect) {
            if (!group.hasBounds) {
                group.bounds = rect;
                group.hasBounds = true;
                return;
            }

            const double minX = std::min(group.bounds.x, rect.x);
            const double minY = std::min(group.bounds.y, rect.y);
            const double maxX = std::max(group.bounds.x + group.bounds.width, rect.x + rect.width);
            const double maxY = std::max(group.bounds.y + group.bounds.height, rect.y + rect.height);
            group.bounds = makeRect(minX, minY, maxX - minX, maxY - minY);
        };

        for (const auto& managed : m_state.windows) {
            const auto window = managed.window;
            if (!window || !window->m_isMapped || window->m_workspace != workspace || window->m_pinned || isFloatingOverviewWindow(window))
                continue;

            const auto target = window->layoutTarget();
            if (!target || target->floating())
                continue;

            const auto targetData = scrolling->dataFor(target);
            const auto column = targetData ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};
            if (!column)
                continue;

            std::optional<std::size_t> groupIndex;
            for (std::size_t index = 0; index < groups.size(); ++index) {
                if (groups[index].column == column) {
                    groupIndex = index;
                    break;
                }
            }
            if (!groupIndex)
                continue;

            const Rect rect = currentPreviewRect(managed);
            expandGroupBounds(groups[*groupIndex], rect);
            capture.windows.push_back({
                .window = window,
                .rect = rect,
                .groupIndex = *groupIndex,
            });

            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] swapcol exact-two capture window"
                    << " group=" << *groupIndex
                    << " window=" << debugWindowLabel(window)
                    << " rect=" << rectToString(rect);
                debugLog(out.str());
            }
        }

        if (!groups[0].hasBounds || !groups[1].hasBounds || capture.windows.empty()) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] swapcol exact-two capture skip reason=empty-groups"
                    << " group0=" << (groups[0].hasBounds ? rectToString(groups[0].bounds) : "<empty>")
                    << " group1=" << (groups[1].hasBounds ? rectToString(groups[1].bounds) : "<empty>")
                    << " windows=" << capture.windows.size();
                debugLog(out.str());
            }
            return capture;
        }

        capture.workspace = workspace;
        capture.groupBounds[0] = groups[0].bounds;
        capture.groupBounds[1] = groups[1].bounds;
        capture.valid = true;

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] swapcol exact-two capture armed"
                << " workspace=" << debugWorkspaceLabel(workspace)
                << " group0=" << rectToString(capture.groupBounds[0])
                << " group1=" << rectToString(capture.groupBounds[1])
                << " windows=" << capture.windows.size();
            debugLog(out.str());
        }

        return capture;
    };

    const auto applyTwoColumnOverviewSwap = [&](const TwoColumnOverviewSwapCapture& capture, const SDispatchResult& dispatchResult) {
        if (!capture.valid || !dispatchResult.success || !isVisible() || m_state.phase != Phase::Active || activeLayoutWorkspace() != capture.workspace) {
            if (debugLogsEnabled() && isSwapColumnLayoutMessage) {
                std::ostringstream out;
                out << "[hymission] swapcol exact-two apply skip"
                    << " captureValid=" << (capture.valid ? 1 : 0)
                    << " resultSuccess=" << (dispatchResult.success ? 1 : 0)
                    << " visible=" << (isVisible() ? 1 : 0)
                    << " phase=" << static_cast<int>(m_state.phase)
                    << " sameWorkspace=" << (activeLayoutWorkspace() == capture.workspace ? 1 : 0)
                    << " activeWorkspace=" << debugWorkspaceLabel(activeLayoutWorkspace())
                    << " captureWorkspace=" << debugWorkspaceLabel(capture.workspace);
                debugLog(out.str());
            }
            return false;
        }

        if (debugLogsEnabled())
            logScrollingWorkspaceSpotState("swapcol-after-dispatch-before-apply", capture.workspace, selectedBefore);

        bool changed = false;
        for (const auto& origin : capture.windows) {
            auto it = std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == origin.window; });
            if (it == m_state.windows.end() || !it->window || !it->window->m_isMapped || it->window->m_workspace != capture.workspace) {
                if (debugLogsEnabled()) {
                    std::ostringstream out;
                    out << "[hymission] swapcol exact-two apply window skip"
                        << " window=" << debugWindowLabel(origin.window)
                        << " found=" << (it != m_state.windows.end() ? 1 : 0);
                    debugLog(out.str());
                }
                continue;
            }

            const std::size_t targetGroupIndex = origin.groupIndex == 0 ? 1 : 0;
            const Rect& fromGroup = capture.groupBounds[origin.groupIndex];
            const Rect& toGroup = capture.groupBounds[targetGroupIndex];
            const double dx = toGroup.centerX() - fromGroup.centerX();
            const double dy = toGroup.centerY() - fromGroup.centerY();
            const Rect target = translateRect(origin.rect, dx, dy);

            it->relayoutFromGlobal = origin.rect;
            it->targetGlobal = target;
            if (it->targetMonitor) {
                it->slot.target = makeRect(target.x - it->targetMonitor->m_position.x, target.y - it->targetMonitor->m_position.y, target.width, target.height);
                if (it->slot.natural.width > 1.0)
                    it->slot.scale = target.width / it->slot.natural.width;
                else if (it->slot.natural.height > 1.0)
                    it->slot.scale = target.height / it->slot.natural.height;
            }

            if (!rectApproxEqual(origin.rect, target, 0.5))
                changed = true;

            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] swapcol exact-two apply window"
                    << " fromGroup=" << origin.groupIndex
                    << " toGroup=" << targetGroupIndex
                    << " dx=" << dx
                    << " dy=" << dy
                    << " window=" << debugWindowLabel(origin.window)
                    << " from=" << rectToString(origin.rect)
                    << " target=" << rectToString(target);
                debugLog(out.str());
            }
        }

        if (!changed) {
            if (debugLogsEnabled())
                debugLog("[hymission] swapcol exact-two apply skip reason=unchanged-targets");
            return false;
        }

        m_state.relayoutActive = niriOverviewAnimationsEnabled();
        m_state.relayoutProgress = m_state.relayoutActive ? 0.0 : 1.0;
        m_state.relayoutStart = {};
        if (!m_state.relayoutActive) {
            for (auto& managed : m_state.windows)
                managed.relayoutFromGlobal = managed.targetGlobal;
        }

        if (selectedBefore && selectedBefore->m_isMapped && selectedBefore->m_workspace == capture.workspace && !selectedBefore->m_pinned &&
            !isFloatingOverviewWindow(selectedBefore)) {
            m_state.focusDuringOverview = selectedBefore;
            selectWindowInState(m_state, selectedBefore);
        }

        refreshWorkspaceLayoutSnapshot(capture.workspace);
        m_stripSnapshotsDirty = true;
        scheduleWorkspaceStripSnapshotRefresh();
        armPendingSwapColumnRelayoutCommit(capture.workspace);
        damageOwnedMonitors();

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] applied exact two-column overview swap"
                << " workspace=" << capture.workspace->m_id
                << " windows=" << capture.windows.size()
                << " animate=" << (m_state.relayoutActive ? 1 : 0);
            debugLog(out.str());
            logOverviewLayoutState("swapcol-exact-two-applied", m_state);
        }

        return true;
    };

    const TwoColumnOverviewSwapCapture twoColumnOverviewSwap = captureTwoColumnOverviewSwap();
    if (twoColumnOverviewSwap.valid)
        armPendingTwoColumnSwapRepair(twoColumnOverviewSwap.workspace);

    const auto result = (*original)(std::move(args));
    if (!result.success)
        clearPendingTwoColumnSwapRepair(twoColumnOverviewSwap.workspace);

    if (debugLogsEnabled() && isSwapColumnLayoutMessage) {
        std::ostringstream out;
        out << "[hymission] swapcol dispatch result"
            << " success=" << (result.success ? 1 : 0)
            << " captureValid=" << (twoColumnOverviewSwap.valid ? 1 : 0);
        debugLog(out.str());
        logSwapColumnFollowupState("swapcol-after-dispatch", activeLayoutWorkspace(), "swapcol", selectedBefore);
    }

    if (debugLogsEnabled() && isMoveColumnLayoutMessage) {
        const auto workspace = activeLayoutWorkspace();
        std::ostringstream out;
        out << "[hymission] movecol dispatch result"
            << " success=" << (result.success ? 1 : 0)
            << " selectedBefore=" << debugWindowLabel(selectedBefore)
            << " selectedAfter=" << debugWindowLabel(selectedWindow())
            << " activeAfter=" << debugWindowLabel(Desktop::focusState()->window())
            << " relayoutActive=" << (m_state.relayoutActive ? 1 : 0)
            << " relayoutProgress=" << m_state.relayoutProgress;
        debugLog(out.str());
        logSwapColumnFollowupState("movecol-after-dispatch", workspace, "movecol", selectedWindow());
        logScrollingWorkspaceSpotState("movecol-after-dispatch", workspace, selectedWindow());
    }

    if (applyTwoColumnOverviewSwap(twoColumnOverviewSwap, result)) {
        clearPendingTwoColumnSwapRepair(twoColumnOverviewSwap.workspace);
        return result;
    }

    if (overviewActive && result.success && (isMoveColumnLayoutMessage || isDirectMoveColumnDispatcher) && directNiriEdgeCameraActive()) {
        clearDirectNiriEdgeCameraFocusState("movecol-edge-after-dispatch");
        refreshVisibleStateMetadata({}, directStripRelayoutOrigins, "movecol-edge");
        damageOwnedMonitors();
        return result;
    }

    if (twoColumnOverviewSwap.valid && result.success) {
        PHLWINDOW swapAnchor = selectedBefore && selectedBefore->m_isMapped ? selectedBefore : Desktop::focusState()->window();
        swapAnchor = closestTiledWindowInWorkspace(swapAnchor);
        if (hardRecalculateScrollingWorkspaceAroundWindow(swapAnchor, "swapcol-post-dispatch-hard-recalc")) {
            clearPendingTwoColumnSwapRepair(twoColumnOverviewSwap.workspace);
            armPendingSwapColumnRelayoutCommit(twoColumnOverviewSwap.workspace);
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] swapcol exact-two refreshed from layout"
                    << " anchor=" << debugWindowLabel(swapAnchor)
                    << " workspace=" << debugWorkspaceLabel(twoColumnOverviewSwap.workspace);
                debugLog(out.str());
            }
            return result;
        }
    }

    if (overviewActive && isVisible() && m_state.phase == Phase::Active) {
        PHLWINDOW forcedGeometryAnchor;
        if (forceGeometryRefocus) {
            PHLWINDOW editedWindow = selectedBefore;
            if ((!editedWindow || !editedWindow->m_isMapped) && Desktop::focusState()->window())
                editedWindow = Desktop::focusState()->window();
            forcedGeometryAnchor = closestTiledWindowInWorkspace(editedWindow);
            if (forcedGeometryAnchor)
                (void)forceDirectNiriGeometryFocus(forcedGeometryAnchor, "geometry-edit-force-refocus");
        }

        std::vector<PHLWORKSPACE> affectedWorkspaces;
        affectedWorkspaces.reserve(4);
        const auto addWorkspace = [&](const PHLWORKSPACE& workspace) {
            if (workspace && !containsHandle(affectedWorkspaces, workspace))
                affectedWorkspaces.push_back(workspace);
        };

        addWorkspace(selectedBefore ? selectedBefore->m_workspace : PHLWORKSPACE{});
        if (const auto focusedAfter = Desktop::focusState()->window(); focusedAfter)
            addWorkspace(focusedAfter->m_workspace);
        if (const auto activeWorkspace = activeLayoutWorkspace(); activeWorkspace)
            addWorkspace(activeWorkspace);

        if (g_pAnimationManager)
            g_pAnimationManager->frameTick();
        for (const auto& workspace : affectedWorkspaces)
            refreshWorkspaceLayoutSnapshot(workspace);

        const auto focusAfter = Desktop::focusState()->window();
        PHLWINDOW preferredSelected = forcedGeometryAnchor ? forcedGeometryAnchor : selectedBefore;
        if ((!preferredSelected || !preferredSelected->m_isMapped || !hasManagedWindow(preferredSelected)) && focusAfter && hasManagedWindow(focusAfter))
            preferredSelected = focusAfter;

        if (forceGeometryRefocus && preferredSelected && preferredSelected->m_isMapped && hasManagedWindow(preferredSelected))
            (void)hardRecalculateScrollingWorkspaceAroundWindow(preferredSelected, "geometry-edit-pre-rebuild-hard-recalc");

        const auto relayoutAnchor = closestTiledWindowInWorkspace(preferredSelected);
        if (relayoutAnchor && relayoutAnchor != preferredSelected) {
            preferredSelected = relayoutAnchor;
            selectWindowInState(m_state, preferredSelected);
            m_state.focusDuringOverview = preferredSelected;
            syncRealFocusDuringOverview(preferredSelected, true);
            (void)syncScrollingWorkspaceSpotOnWindow(preferredSelected);
            if (preferredSelected->m_workspace)
                refreshWorkspaceLayoutSnapshot(preferredSelected->m_workspace);
        }

        rebuildVisibleState(preferredSelected, true);
        refreshNiriScrollingOverviewAfterFocusDispatcher(dispatcherName, preferredSelected);
        if (isSwapColumnLayoutMessage && result.success)
            armPendingSwapColumnRelayoutCommit(activeLayoutWorkspace());

        if (g_pEventLoopManager && usesDirectNiriScrollingOverview(m_state)) {
            const auto deferredTarget = preferredSelected;
            g_pEventLoopManager->doLater([this, deferredTarget, source = std::string(dispatcherName ? dispatcherName : "overview-edit"), forceGeometryRefocus,
                                          deferredFromSwapcol = isSwapColumnLayoutMessage] {
                if (!niri_scrolling_detail::isActiveController(this) || !isVisible() || m_state.phase != Phase::Active || m_workspaceTransition.active ||
                    m_beginCloseInProgress)
                    return;

                if (deferredFromSwapcol && !hasPendingSwapColumnRelayoutCommit(activeLayoutWorkspace()))
                    return;

                PHLWINDOW target = deferredTarget;
                if (!target || !target->m_isMapped || !hasManagedWindow(target))
                    target = selectedWindow();
                if (!target || !target->m_isMapped || !hasManagedWindow(target))
                    target = Desktop::focusState()->window();
                if (!target || !target->m_isMapped || !hasManagedWindow(target))
                    return;

                if ((target->m_pinned || isFloatingOverviewWindow(target)) && usesDirectNiriScrollingOverview(m_state)) {
                    const auto workspace = target->m_pinned ? activeLayoutWorkspace() : target->m_workspace;
                    if (workspace && isScrollingWorkspace(workspace)) {
                        const auto* sourceManaged = managedWindowFor(m_state, target, true);
                        const Rect sourceRect = sourceManaged ? currentPreviewRect(*sourceManaged) : liveGlobalRectForWindow(target);
                        PHLWINDOW best;
                        double    bestDistance = std::numeric_limits<double>::infinity();
                        for (const auto& managed : m_state.windows) {
                            const auto candidate = managed.window;
                            if (!candidate || candidate == target || !candidate->m_isMapped || candidate->m_workspace != workspace || candidate->m_pinned)
                                continue;

                            const auto layoutTarget = candidate->layoutTarget();
                            if (!layoutTarget || layoutTarget->floating() || isFloatingOverviewWindow(candidate))
                                continue;

                            const Rect candidateRect = currentPreviewRect(managed);
                            const double dx = candidateRect.centerX() - sourceRect.centerX();
                            const double dy = candidateRect.centerY() - sourceRect.centerY();
                            const double distance = dx * dx + dy * dy;
                            if (!best || distance < bestDistance) {
                                best = candidate;
                                bestDistance = distance;
                            }
                        }

                        if (best && best->m_isMapped && hasManagedWindow(best))
                            target = best;
                    }
                }

                if (g_pAnimationManager)
                    g_pAnimationManager->frameTick();
                if (target->m_workspace)
                    refreshWorkspaceLayoutSnapshot(target->m_workspace);
                if (const auto activeWorkspace = activeLayoutWorkspace(); activeWorkspace && activeWorkspace != target->m_workspace)
                    refreshWorkspaceLayoutSnapshot(activeWorkspace);

                if (forceGeometryRefocus) {
                    selectWindowInState(m_state, target);
                    m_state.focusDuringOverview = target;
                    m_queuedOverviewSelectionTarget.reset();
                    m_queuedOverviewSelectionSyncScrollingSpot = false;
                    m_queuedOverviewSelectionCenterCursor = false;
                    m_queuedOverviewLiveFocusTarget.reset();
                    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
                    m_queuedOverviewLiveFocusCenterCursor = false;

                    if (target->m_workspace) {
                        m_pendingLiveFocusWorkspaceChangeTarget = target;
                        (void)activateWindowWorkspaceForFocus(target);
                    }
                    focusWindowCompat(target, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
                    if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == target)
                        m_pendingLiveFocusWorkspaceChangeTarget.reset();
                    if (target->m_workspace && isScrollingWorkspace(target->m_workspace)) {
                        auto* const scrolling = scrollingAlgorithmForWorkspace(target->m_workspace);
                        const auto layoutTarget = target->layoutTarget();
                        if (scrolling && layoutTarget && !layoutTarget->floating()) {
                            if (const auto targetData = scrolling->dataFor(layoutTarget); targetData) {
                                if (const auto column = targetData->column.lock()) {
                                    column->lastFocusedTarget = targetData;
                                    if (scrolling->m_scrollingData && scrolling->m_scrollingData->controller) {
                                        if (getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 1)
                                            scrolling->m_scrollingData->fitCol(column);
                                        else
                                            scrolling->m_scrollingData->centerCol(column);
                                    }
                                }
                            }
                        }
                        if (target->m_workspace->m_space)
                            target->m_workspace->m_space->recalculate();
                        if (scrolling && scrolling->m_scrollingData)
                            scrolling->m_scrollingData->recalculate(true);
                    }
                    (void)syncScrollingWorkspaceSpotOnWindow(target);
                    if (target->m_workspace && target->m_workspace->m_space)
                        target->m_workspace->m_space->recalculate();
                    refreshNiriScrollingOverviewAfterLayoutScroll("geometry-edit-deferred-refocus");
                    m_stripSnapshotsDirty = true;
                    scheduleWorkspaceStripSnapshotRefresh();
                } else {
                    selectWindowInState(m_state, target);
                    m_state.focusDuringOverview = target;
                    syncRealFocusDuringOverview(target, true);
                    (void)syncScrollingWorkspaceSpotOnWindow(target);
                }
                rebuildVisibleState(target, true);
                refreshNiriScrollingOverviewAfterFocusDispatcher(source.c_str(), target);
            });
        }
    }

    return result;
}

void OverviewController::scrollMoveGestureBeginHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureBegin& e) {
    if (m_scrollMoveGestureBeginOriginal)
        m_scrollMoveGestureBeginOriginal(gestureThisptr, e);

    refreshAfterOfficialScrollMove("scrollMove-begin");
}
void OverviewController::scrollMoveGestureUpdateHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (m_scrollMoveGestureUpdateOriginal)
        m_scrollMoveGestureUpdateOriginal(gestureThisptr, e);

    refreshAfterOfficialScrollMove("scrollMove-update");
}
void OverviewController::scrollMoveGestureEndHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (m_scrollMoveGestureEndOriginal)
        m_scrollMoveGestureEndOriginal(gestureThisptr, e);

    refreshAfterOfficialScrollMove("scrollMove-end");
}
bool OverviewController::shouldRenderEmptyOverviewPlaceholder(const State& state, const PHLMONITOR& monitor) const {
    if (!niriModeEnabled() || !state.collectionPolicy.onlyActiveWorkspace)
        return false;

    const auto workspace = state.ownerWorkspace ? state.ownerWorkspace : (monitor ? monitor->m_activeWorkspace : PHLWORKSPACE{});
    return workspace && isScrollingWorkspace(workspace);
}
bool OverviewController::beginScrollGesture(HymissionScrollMode mode, eTrackpadGestureDirection direction, const IPointer::SSwipeUpdateEvent& event, float deltaScale) {
    m_scrollGestureSession = {};

    const auto phaseName = [this]() {
        switch (m_state.phase) {
            case Phase::Inactive: return "inactive";
            case Phase::Opening: return "opening";
            case Phase::Active: return "active";
            case Phase::ClosingSettle: return "closing_settle";
            case Phase::Closing: return "closing";
        }
        return "unknown";
    };

    if (debugLogsEnabled()) {
        const auto layoutDirection = scrollingLayoutDirection();
        const auto workspace = activeLayoutWorkspace();
        std::ostringstream out;
        out << "[hymission] scroll gesture begin request mode=" << (mode == HymissionScrollMode::Layout ? "layout" : "unknown")
            << " dir=" << trackpadDirectionName(direction) << " gestureAxis=" << gestureAxisName(gestureAxisForDirection(direction))
            << " layoutDir=" << scrollingDirectionName(layoutDirection) << " layoutAxis=" << gestureAxisName(axisForScrollingLayoutDirection(layoutDirection))
            << " delta=" << vectorToString(event.delta) << " scale=" << deltaScale << " phase=" << phaseName() << " visible=" << (isVisible() ? 1 : 0)
            << " overviewGestureActive=" << (m_gestureSession.active ? 1 : 0) << " workspace=" << (workspace ? workspace->m_name : std::string{"<none>"})
            << " workspaceScrolling=" << (isScrollingWorkspace(workspace) ? 1 : 0);
        debugLog(out.str());
    }

    const auto reject = [&](const char* reason) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] scroll gesture reject reason=" << reason;
            debugLog(out.str());
        }
        return false;
    };

    if (mode != HymissionScrollMode::Layout)
        return reject("unsupported-mode");

    if (m_gestureSession.active)
        return reject("overview-gesture-active");

    if (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return reject("overview-closing");

    const bool overviewVisible = isVisible();
    if (overviewVisible && (!niriModeAppliesToState(m_state) || m_state.phase != Phase::Active))
        return reject("overview-visible");

    if (!canScrollActiveLayoutWithGesture(direction))
        return reject("axis-mismatch");

    if (!isScrollingWorkspace(activeLayoutWorkspace()))
        return reject("active-workspace-not-scrolling");

    const bool scrollingFollowFocusWasOverridden = m_scrollingFollowFocusOverridden;
    setScrollingFollowFocusOverride(true);

    m_scrollGestureSession = {
        .active = true,
        .mode = mode,
        .route = ScrollGestureRoute::Layout,
        .direction = direction,
        .deltaScale = deltaScale,
        .skipNextUpdate = true,
        .restoreScrollingFollowFocus = !scrollingFollowFocusWasOverridden && m_scrollingFollowFocusOverridden,
    };

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] scroll gesture accepted route=layout dir=" << trackpadDirectionName(direction) << " scale=" << deltaScale
            << " suppressScrollingFollowFocus=" << (m_scrollGestureSession.restoreScrollingFollowFocus ? 1 : 0);
        debugLog(out.str());
    }

    if (!scrollActiveLayoutByGestureDelta(event, direction, deltaScale)) {
        m_scrollGestureSession = {};
        return reject("initial-layout-scroll-failed");
    }
    refreshNiriScrollingOverviewAfterLayoutScroll("scroll-begin");

    return true;
}
void OverviewController::updateScrollGesture(const IPointer::SSwipeUpdateEvent& event) {
    if (!m_scrollGestureSession.active)
        return;

    if (m_scrollGestureSession.skipNextUpdate) {
        m_scrollGestureSession.skipNextUpdate = false;
        return;
    }

    switch (m_scrollGestureSession.route) {
        case ScrollGestureRoute::Layout:
            (void)scrollActiveLayoutByGestureDelta(event, m_scrollGestureSession.direction, m_scrollGestureSession.deltaScale);
            refreshNiriScrollingOverviewAfterLayoutScroll("scroll-update");
            break;
        case ScrollGestureRoute::None:
        default:
            break;
    }
}
void OverviewController::endScrollGesture(bool cancelled) {
    if (!m_scrollGestureSession.active)
        return;

    const bool deferScrollingFollowFocusRestore = m_scrollGestureSession.restoreScrollingFollowFocus;
    const bool forceInputRefocus = !isVisible() && !cancelled && m_scrollGestureSession.route == ScrollGestureRoute::Layout;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] scroll gesture end cancelled=" << (cancelled ? 1 : 0) << " samples=" << m_scrollGestureSession.debugSamples
            << " deferScrollingFollowFocusRestore=" << (deferScrollingFollowFocusRestore ? 1 : 0)
            << " forceInputRefocus=" << (forceInputRefocus ? 1 : 0);
        debugLog(out.str());
    }

    m_scrollGestureSession = {};

    if (forceInputRefocus && g_pInputManager) {
        if (debugLogsEnabled())
            debugLog("[hymission] scroll gesture end force input refocus");
        g_pInputManager->refocus();
    }

    if (deferScrollingFollowFocusRestore)
        m_restoreScrollingFollowFocusAfterScrollMouseMove = true;
}
void OverviewController::refreshWorkspaceLayoutSnapshot(const PHLWORKSPACE& workspace) const {
    if (!workspace || !workspace->m_space)
        return;

    const bool shouldRefresh = !workspace->isVisible() || isScrollingWorkspace(workspace);
    if (!shouldRefresh)
        return;

    const auto workspaceMonitor = workspace->m_monitor.lock();
    const auto previousActiveWorkspace = workspaceMonitor ? workspaceMonitor->m_activeWorkspace : PHLWORKSPACE{};
    const auto previousActiveSpecialWorkspace = workspaceMonitor ? workspaceMonitor->m_activeSpecialWorkspace : PHLWORKSPACE{};
    const bool previousVisible = workspace->m_visible;
    const bool previousForceRendering = workspace->m_forceRendering;
    const Vector2D previousRenderOffsetValue = workspace->m_renderOffset ? workspace->m_renderOffset->value() : Vector2D{};
    const Vector2D previousRenderOffsetGoal = workspace->m_renderOffset ? workspace->m_renderOffset->goal() : Vector2D{};
    const float previousAlphaValue = workspace->m_alpha ? workspace->m_alpha->value() : 1.0F;
    const float previousAlphaGoal = workspace->m_alpha ? workspace->m_alpha->goal() : 1.0F;

    auto* const scrolling = isScrollingWorkspace(workspace) ? scrollingAlgorithmForWorkspace(workspace) : nullptr;
    const bool inactiveScrollingWorkspace = scrolling && scrolling->m_scrollingData && workspaceMonitor && previousActiveWorkspace != workspace;

    const auto logScrollingGeometry = [&](const char* phase) {
        if (!debugLogsEnabled() || !scrolling || !scrolling->m_scrollingData)
            return;

        std::size_t totalTargets = 0;
        std::size_t zeroTargets = 0;
        std::ostringstream columns;
        columns << "[hymission] refresh workspace layout snapshot geometry phase=" << (phase ? phase : "?")
                << " workspace=" << debugWorkspaceLabel(workspace)
                << " active=" << debugWorkspaceLabel(workspaceMonitor ? workspaceMonitor->m_activeWorkspace : PHLWORKSPACE{})
                << " visible=" << (workspace->isVisible() ? 1 : 0)
                << " forceRendering=" << (workspace->m_forceRendering ? 1 : 0)
                << " columns=" << scrolling->m_scrollingData->columns.size()
                << " offset=" << (scrolling->m_scrollingData->controller ? scrolling->m_scrollingData->controller->getOffset() : 0.0);

        for (std::size_t columnIndex = 0; columnIndex < scrolling->m_scrollingData->columns.size(); ++columnIndex) {
            const auto& column = scrolling->m_scrollingData->columns[columnIndex];
            if (!column) {
                columns << " | col#" << columnIndex << " <null>";
                continue;
            }

            columns << " | col#" << columnIndex << " width=" << column->getColumnWidth() << " targets=" << column->targetDatas.size();
            for (std::size_t targetIndex = 0; targetIndex < column->targetDatas.size(); ++targetIndex) {
                const auto& targetData = column->targetDatas[targetIndex];
                const auto target = targetData ? targetData->target.lock() : SP<Layout::ITarget>{};
                const auto window = target ? target->window() : PHLWINDOW{};
                const CBox targetBox = target ? target->position() : CBox{};
                const CBox layoutBox = targetData ? targetData->layoutBox : CBox{};
                const bool zero = targetBox.width <= 1.0 || targetBox.height <= 1.0 || layoutBox.width <= 1.0 || layoutBox.height <= 1.0;
                ++totalTargets;
                if (zero)
                    ++zeroTargets;

                columns << " [" << targetIndex << "]" << debugWindowLabel(window)
                        << " pos=" << boxToString(targetBox)
                        << " layout=" << boxToString(layoutBox)
                        << " zero=" << (zero ? 1 : 0);
            }
        }

        columns << " totalTargets=" << totalTargets << " zeroTargets=" << zeroTargets;
        debugLog(columns.str());
    };

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] refresh workspace layout snapshot workspace=" << debugWorkspaceLabel(workspace)
            << " visible=" << (workspace->isVisible() ? 1 : 0)
            << " scrolling=" << (isScrollingWorkspace(workspace) ? 1 : 0)
            << " inactiveScrolling=" << (inactiveScrollingWorkspace ? 1 : 0)
            << " monitorActiveBefore=" << debugWorkspaceLabel(previousActiveWorkspace);
        debugLog(out.str());
    }

    logScrollingGeometry("before");

    if (inactiveScrollingWorkspace) {
        // Niri computes overview rows from layout state, not from the currently
        // visible compositor workspace.  Hyprland's scrolling algorithm only
        // reliably refreshes some target boxes after its workspace becomes the
        // monitor active workspace.  For strip snapshots, briefly borrow that
        // active-workspace context, run the normal scrolling recalculation, then
        // restore the user's real focus/active workspace immediately.
        workspaceMonitor->m_activeWorkspace = workspace;
        workspaceMonitor->m_activeSpecialWorkspace.reset();
        workspace->m_visible = true;
        workspace->m_forceRendering = true;
        if (workspace->m_renderOffset)
            workspace->m_renderOffset->setValueAndWarp(Vector2D{});
        if (workspace->m_alpha)
            workspace->m_alpha->setValueAndWarp(1.F);

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] refresh workspace layout snapshot borrowed active workspace=" << debugWorkspaceLabel(workspace)
                << " previousActive=" << debugWorkspaceLabel(previousActiveWorkspace);
            debugLog(out.str());
        }
    }

    // Preserve the current scrolling offset when snapshotting overview geometry.
    // CScrollingAlgorithm::recalculate() may hard-fit the focused column back
    // into view; update target boxes directly so overview layout scrolling can
    // travel across the whole tape instead of snapping around the focused window.
    if (scrolling && scrolling->m_scrollingData) {
        const bool staleTargets = scrollingDataHasStaleWorkspaceTargets(scrolling, workspace);
        if (debugLogsEnabled() && staleTargets)
            debugLog("[hymission] refresh workspace layout snapshot stale targets; forcing active-context recalc workspace=" + debugWorkspaceLabel(workspace));

        if (staleTargets)
            workspace->m_space->recalculate();

        const auto hydrated = hydrateScrollingDataFromWorkspaceTargets(scrolling, workspace);
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] hydrate scrolling snapshot workspace=" << debugWorkspaceLabel(workspace)
                << " columnsBefore=" << hydrated.columnsBefore
                << " columnsAfter=" << hydrated.columnsAfter
                << " spaceTargetRefs=" << hydrated.spaceTargetRefs
                << " acceptedSpaceTargets=" << hydrated.acceptedSpaceTargets
                << " acceptedWindowTargets=" << hydrated.acceptedWindowTargets
                << " alreadyPresent=" << hydrated.alreadyPresent
                << " added=" << hydrated.added;
            debugLog(out.str());
            if (hydrated.added > 0)
                debugLog("[hymission] refresh workspace layout snapshot hydrated missing scrolling columns workspace=" + debugWorkspaceLabel(workspace));
        }

        scrolling->m_scrollingData->recalculate(true);
        workspace->updateWindows();
    } else {
        workspace->m_space->recalculate();
        workspace->updateWindows();
    }

    logScrollingGeometry("after");

    if (inactiveScrollingWorkspace) {
        workspaceMonitor->m_activeWorkspace = previousActiveWorkspace;
        workspaceMonitor->m_activeSpecialWorkspace = previousActiveSpecialWorkspace;
        workspace->m_visible = previousVisible;
        workspace->m_forceRendering = previousForceRendering;
        if (workspace->m_renderOffset) {
            workspace->m_renderOffset->setValueAndWarp(previousRenderOffsetValue);
            if (previousRenderOffsetGoal != previousRenderOffsetValue)
                *workspace->m_renderOffset = previousRenderOffsetGoal;
        }
        if (workspace->m_alpha) {
            workspace->m_alpha->setValueAndWarp(previousAlphaValue);
            if (std::abs(previousAlphaGoal - previousAlphaValue) > 0.0001F)
                *workspace->m_alpha = previousAlphaGoal;
        }

        if (previousActiveWorkspace)
            previousActiveWorkspace->updateWindows();
    }
}

std::optional<Rect> OverviewController::livePreviewRectForManagedWindow(const ManagedWindow& window) const {
    if (!window.window || !window.window->m_isMapped || !window.targetMonitor || !usesDirectNiriScrollingOverview(m_state))
        return std::nullopt;

    const auto workspace = (window.window->m_pinned || window.isPinned) ? m_state.ownerWorkspace : window.window->m_workspace;
    if (!workspace || !workspace->m_space || !isScrollingWorkspace(workspace))
        return std::nullopt;

    if (!window.window->m_pinned && !window.isPinned && window.targetMonitor->m_activeWorkspace != workspace)
        return window.targetGlobal;

    const auto placeholder = std::find_if(m_state.emptyWorkspacePlaceholders.begin(), m_state.emptyWorkspacePlaceholders.end(),
                                          [&](const EmptyWorkspacePlaceholder& candidate) {
                                              return candidate.backingOnly && candidate.monitor == window.targetMonitor &&
                                                  candidate.workspaceId == workspace->m_id;
                                          });
    if (placeholder == m_state.emptyWorkspacePlaceholders.end())
        return std::nullopt;

    const CBox desktopBox = workspace->m_space->workArea();
    const Rect desktopViewport = makeRect(desktopBox.x, desktopBox.y, desktopBox.width, desktopBox.height);
    const Rect liveRect = renderGlobalRectForWindow(window.window);
    if (desktopViewport.width <= 1.0 || desktopViewport.height <= 1.0 || liveRect.width <= 0.0 || liveRect.height <= 0.0)
        return std::nullopt;

    const Rect transformed = transformLiveOverviewRect(liveRect, desktopViewport, placeholder->targetGlobal);
    if (transformed.width <= 0.0 || transformed.height <= 0.0)
        return std::nullopt;

    return transformed;
}

void OverviewController::resetDirectNiriWorkspaceLanes() {
    niri_scrolling_detail::clearRetainedDirectNiriWorkspaceLanes();
}

bool OverviewController::removeOccupiedWorkspacePlaceholder(State& state, const PHLWINDOW& window) const {
    if (!window || window->m_pinned || !window->m_workspace)
        return false;

    const auto workspace = window->m_workspace;
    const auto monitor = workspace->m_monitor.lock();
    const auto previousSize = state.emptyWorkspacePlaceholders.size();
    std::erase_if(state.emptyWorkspacePlaceholders, [&](const EmptyWorkspacePlaceholder& placeholder) {
        return !placeholder.backingOnly && placeholder.workspaceId == workspace->m_id && (!monitor || placeholder.monitor == monitor);
    });
    return state.emptyWorkspacePlaceholders.size() != previousSize;
}

Rect OverviewController::currentPreviewRect(const ManagedWindow& window) const {
    if (m_workspaceTransition.active) {
        if (const auto rect = workspaceTransitionRectForWindow(window.window); rect)
            return *rect;
    }

    if (m_gestureSession.active) {
        if (m_gestureSession.opening)
            return lerpRect(window.naturalGlobal, window.targetGlobal, visualProgress());
        return lerpRect(window.exitGlobal, window.targetGlobal, visualProgress());
    }

    const auto activeBaseRect = [&]() {
        if (m_state.relayoutActive)
            return lerpRect(window.relayoutFromGlobal, window.targetGlobal, relayoutVisualProgress());
        return window.targetGlobal;
    };

    const auto focusedStripWorkspaceForPreviewMonitor = [&](const PHLMONITOR& candidateMonitor) -> PHLWORKSPACE {
        if (!candidateMonitor)
            return {};

        if (m_state.focusDuringOverview && !m_state.focusDuringOverview->m_pinned && m_state.focusDuringOverview->m_workspace) {
            const auto focusedWorkspaceMonitor = m_state.focusDuringOverview->m_workspace->m_monitor.lock();
            if (focusedWorkspaceMonitor == candidateMonitor)
                return m_state.focusDuringOverview->m_workspace;
        }

        if (candidateMonitor->m_activeWorkspace)
            return candidateMonitor->m_activeWorkspace;

        if (m_state.ownerWorkspace && m_state.ownerWorkspace->m_monitor.lock() == candidateMonitor)
            return m_state.ownerWorkspace;

        return {};
    };

    const auto dynamicNiriTiledResizeRect = [&]() -> std::optional<Rect> {
        if (m_state.phase != Phase::Active || !usesDirectNiriScrollingOverview(m_state) || !window.window || !window.window->m_isMapped || !window.targetMonitor)
            return std::nullopt;

        if (window.window->m_pinned || window.isPinned || isFloatingOverviewWindow(window.window) || window.isNiriFloatingOverlay)
            return std::nullopt;

        const auto workspace = window.window->m_workspace;
        if (!workspace || !isScrollingWorkspace(workspace))
            return std::nullopt;

        const auto target = window.window->layoutTarget();
        if (!target || target->floating())
            return std::nullopt;

        PHLWINDOW anchorWindow;
        if (m_state.focusDuringOverview && m_state.focusDuringOverview->m_isMapped && !m_state.focusDuringOverview->m_pinned &&
            m_state.focusDuringOverview->m_workspace == workspace && !isFloatingOverviewWindow(m_state.focusDuringOverview)) {
            anchorWindow = m_state.focusDuringOverview;
        } else if (const auto selected = selectedWindow(); selected && selected->m_isMapped && !selected->m_pinned && selected->m_workspace == workspace &&
                   !isFloatingOverviewWindow(selected)) {
            anchorWindow = selected;
        } else {
            anchorWindow = focusCandidateForWorkspace(workspace);
            const auto anchorTarget = anchorWindow ? anchorWindow->layoutTarget() : nullptr;
            if (!anchorWindow || !anchorWindow->m_isMapped || anchorWindow->m_pinned || isFloatingOverviewWindow(anchorWindow) || !anchorTarget || anchorTarget->floating())
                anchorWindow = window.window;
        }

        const auto* anchorManaged = managedWindowFor(m_state, anchorWindow, true);
        if (!anchorManaged || !anchorManaged->window || !anchorManaged->targetMonitor || anchorManaged->targetMonitor != window.targetMonitor)
            anchorManaged = &window;

        const bool isResizeAnchorWindow = window.window == anchorWindow;

        // For the actively resized column, do not build the preview from Hyprland's
        // goal geometry. Goal geometry jumps straight to the final column width while
        // the live surface is still animating there, which makes the overview draw a
        // shrink-then-grow rubber-band.  Track the live box for the resized column and
        // let Hyprland's own linear size progression drive the preview size.
        const bool useGoalGeometry = !isResizeAnchorWindow && shouldUseGoalGeometryForStateSnapshot(window.window);
        const Rect naturalGlobal = stateSnapshotGlobalRectForWindow(window.window, useGoalGeometry);
        const Rect sourceGlobal = scrollingOverviewSourceGlobalRectForWindow(window.window, naturalGlobal);
        const auto rowGeometry = scrollingOverviewTapeRowGeometryForWindow(window.window, sourceGlobal, anchorWindow);
        if (!rowGeometry)
            return std::nullopt;

        const bool anchorUseGoalGeometry = anchorManaged->window != anchorWindow && shouldUseGoalGeometryForStateSnapshot(anchorManaged->window);
        const Rect anchorNaturalGlobal = stateSnapshotGlobalRectForWindow(anchorManaged->window, anchorUseGoalGeometry);
        const Rect anchorSourceBase = scrollingOverviewSourceGlobalRectForWindow(anchorManaged->window, anchorNaturalGlobal);
        const auto anchorRowGeometry = scrollingOverviewTapeRowGeometryForWindow(anchorManaged->window, anchorSourceBase, anchorManaged->window);
        const Rect anchorSourceGlobal = anchorRowGeometry ? anchorRowGeometry->anchorGlobal : anchorManaged->naturalGlobal;

        double scale = window.slot.scale;
        if (scale <= 0.0 && window.naturalGlobal.width > 1.0)
            scale = window.targetGlobal.width / window.naturalGlobal.width;
        if (scale <= 0.0 && window.naturalGlobal.height > 1.0)
            scale = window.targetGlobal.height / window.naturalGlobal.height;
        if (scale <= 0.0)
            return std::nullopt;

        const auto previewBaseForManaged = [&](const ManagedWindow& managed) {
            if (m_state.relayoutActive)
                return lerpRect(managed.relayoutFromGlobal, managed.targetGlobal, relayoutVisualProgress());
            return managed.targetGlobal;
        };

        const Rect ownPreviewBase = previewBaseForManaged(window);
        const Rect rawAnchorPreviewBase = anchorManaged == &window ? ownPreviewBase :
            (m_state.relayoutActive ? lerpRect(anchorManaged->relayoutFromGlobal, anchorManaged->targetGlobal, relayoutVisualProgress()) : anchorManaged->targetGlobal);
        const double targetWidth = std::max(1.0, rowGeometry->sourceGlobal.width * scale);
        const double targetHeight = std::max(1.0, rowGeometry->sourceGlobal.height * scale);
        const double anchorTargetWidth = std::max(1.0, anchorSourceGlobal.width * scale);
        const double anchorTargetHeight = std::max(1.0, anchorSourceGlobal.height * scale);
        const bool ownSizeChanged = std::abs(targetWidth - ownPreviewBase.width) > 1.0 || std::abs(targetHeight - ownPreviewBase.height) > 1.0;
        const bool anchorSizeChanged = std::abs(anchorTargetWidth - rawAnchorPreviewBase.width) > 1.0 || std::abs(anchorTargetHeight - rawAnchorPreviewBase.height) > 1.0;
        const bool traceTwoColumnSwap = debugLogsEnabled() && twoColumnSwapTraceActive(workspace);

        Rect anchorPreviewBase = rawAnchorPreviewBase;
        if (anchorRowGeometry) {
            Rect columnPreview{};
            bool hasColumnPreview = false;
            if (auto* scrolling = scrollingAlgorithmForWorkspace(workspace); scrolling) {
                const auto anchorTarget = anchorWindow ? anchorWindow->layoutTarget() : nullptr;
                const auto anchorData = anchorTarget ? scrolling->dataFor(anchorTarget) : nullptr;
                const auto anchorColumn = anchorData ? anchorData->column.lock() : SP<Layout::Tiled::SColumnData>{};
                if (anchorColumn) {
                    for (const auto& candidate : m_state.windows) {
                        if (!candidate.window || !candidate.window->m_isMapped || candidate.window->m_workspace != workspace ||
                            candidate.window->m_pinned || isFloatingOverviewWindow(candidate.window))
                            continue;

                        const auto candidateTarget = candidate.window->layoutTarget();
                        if (!candidateTarget || candidateTarget->floating())
                            continue;

                        const auto candidateData = scrolling->dataFor(candidateTarget);
                        if (!candidateData || candidateData->column.lock() != anchorColumn)
                            continue;

                        const Rect candidatePreview = previewBaseForManaged(candidate);
                        if (!hasColumnPreview) {
                            columnPreview = candidatePreview;
                            hasColumnPreview = true;
                            continue;
                        }

                        const double minX = std::min(columnPreview.x, candidatePreview.x);
                        const double minY = std::min(columnPreview.y, candidatePreview.y);
                        const double maxX = std::max(columnPreview.x + columnPreview.width, candidatePreview.x + candidatePreview.width);
                        const double maxY = std::max(columnPreview.y + columnPreview.height, candidatePreview.y + candidatePreview.height);
                        columnPreview = makeRect(minX, minY, maxX - minX, maxY - minY);
                    }
                }
            }

            if (hasColumnPreview)
                anchorPreviewBase = columnPreview;
        }

        const double viewportX = anchorPreviewBase.centerX() - (anchorSourceGlobal.centerX() - rowGeometry->baseGlobal.x) * scale;
        const double viewportY = anchorPreviewBase.centerY() - (anchorSourceGlobal.centerY() - rowGeometry->baseGlobal.y) * scale;

        const double targetCenterX = viewportX + (rowGeometry->sourceGlobal.centerX() - rowGeometry->baseGlobal.x) * scale;
        const double targetCenterY = viewportY + (rowGeometry->sourceGlobal.centerY() - rowGeometry->baseGlobal.y) * scale;
        Rect dynamicRect = makeRect(targetCenterX - targetWidth * 0.5, targetCenterY - targetHeight * 0.5, targetWidth, targetHeight);

        const double previewGap = niriWindowGapsForWorkspace(workspace, rowGeometry->primaryAxis);
        if (previewGap > 0.0) {
            if (rowGeometry->primaryAxis == GestureAxis::Horizontal) {
                const double width = std::max(1.0, dynamicRect.width - previewGap);
                dynamicRect = makeRect(dynamicRect.centerX() - width * 0.5, dynamicRect.y, width, dynamicRect.height);
            } else {
                const double height = std::max(1.0, dynamicRect.height - previewGap);
                dynamicRect = makeRect(dynamicRect.x, dynamicRect.centerY() - height * 0.5, dynamicRect.width, height);
            }
        }

        if (usesDirectNiriScrollingOverview(m_state)) {
            // Direct-Niri live geometry can jump to Hyprland's final target as soon
            // as the scrolling controller reflows from edge-camera/scroll-past back
            // to a leaf. While the overview has a relayout animation armed, draw
            // from the captured relayout origin instead of the live dynamic rect so
            // every reflow visibly follows the windowsMove-style animation path.
            if (m_state.relayoutActive)
                return activeBaseRect();

            return dynamicRect;
        }

        struct DynamicResizeAnimation {
            Rect                               from;
            Rect                               to;
            Rect                               lastRendered;
            std::chrono::steady_clock::time_point start;
            bool                               hasLastRendered = false;
        };

        static std::unordered_map<const void*, DynamicResizeAnimation> s_dynamicResizeAnimations;

        const void* const animationKey = window.window.get();
        const auto        now = std::chrono::steady_clock::now();
        auto&             animation = s_dynamicResizeAnimations[animationKey];
        const bool        noSizePositionChanged = !ownSizeChanged && !anchorSizeChanged && !rectApproxEqual(dynamicRect, activeBaseRect(), 0.5);
        bool              twoColumnWorkspace = false;
        if (noSizePositionChanged) {
            if (auto* scrolling = scrollingAlgorithmForWorkspace(workspace); scrolling && scrolling->m_scrollingData)
                twoColumnWorkspace = scrolling->m_scrollingData->columns.size() == 2;
        }

        // Keep the resize animation origin current while no live resize is happening.
        // This prevents a later grow animation from starting from an old stale position
        // after normal scrolling or focus movement.
        if (!ownSizeChanged && !anchorSizeChanged && !twoColumnWorkspace) {
            const Rect currentBase = activeBaseRect();
            animation.from = currentBase;
            animation.to = currentBase;
            animation.lastRendered = currentBase;
            animation.start = now - std::chrono::milliseconds(static_cast<int>(RELAYOUT_DURATION_MS));
            animation.hasLastRendered = true;
            if (traceTwoColumnSwap && consumeTwoColumnSwapPreviewTrace(workspace)) {
                std::ostringstream out;
                out << "[hymission] swapcol preview dynamic-tiled null"
                    << " reason=no-size-change"
                    << " window=" << debugWindowLabel(window.window)
                    << " base=" << rectToString(currentBase)
                    << " source=" << rectToString(rowGeometry->sourceGlobal)
                    << " anchorSource=" << rectToString(anchorSourceGlobal)
                    << " ownSizeChanged=0 anchorSizeChanged=0";
                debugLog(out.str());
            }
            return std::nullopt;
        }

        const auto progressFor = [&](const DynamicResizeAnimation& candidate) {
            if (candidate.start == std::chrono::steady_clock::time_point{})
                return 1.0;

            const double elapsedMs = std::chrono::duration<double, std::milli>(now - candidate.start).count();
            return clampUnit(elapsedMs / std::max(1.0, RELAYOUT_DURATION_MS));
        };

        const auto currentVisibleRect = [&]() {
            if (animation.hasLastRendered)
                return animation.lastRendered;

            if (animation.start != std::chrono::steady_clock::time_point{}) {
                const double previousProgress = progressFor(animation);
                return lerpRect(animation.from, animation.to, easeOutCubic(previousProgress));
            }

            return activeBaseRect();
        };

        const bool directlyResizedWindow = ownSizeChanged;
        if (twoColumnWorkspace && !ownSizeChanged && !anchorSizeChanged) {
            animation.from = dynamicRect;
            animation.to = dynamicRect;
            animation.lastRendered = dynamicRect;
            animation.start = now - std::chrono::milliseconds(static_cast<int>(RELAYOUT_DURATION_MS));
            animation.hasLastRendered = true;
            if (traceTwoColumnSwap && consumeTwoColumnSwapPreviewTrace(workspace)) {
                std::ostringstream out;
                out << "[hymission] swapcol preview dynamic-tiled"
                    << " reason=two-column-position"
                    << " window=" << debugWindowLabel(window.window)
                    << " rect=" << rectToString(dynamicRect)
                    << " activeBase=" << rectToString(activeBaseRect())
                    << " source=" << rectToString(rowGeometry->sourceGlobal)
                    << " ownSizeChanged=0 anchorSizeChanged=0";
                debugLog(out.str());
            }
            return dynamicRect;
        }

        if (animation.start == std::chrono::steady_clock::time_point{}) {
            const Rect visibleNow = currentVisibleRect();
            animation = {
                .from = visibleNow,
                .to = dynamicRect,
                .lastRendered = visibleNow,
                .start = now,
                .hasLastRendered = true,
            };
        } else if (!rectApproxEqual(animation.to, dynamicRect, 0.5)) {
            const double existingProgress = progressFor(animation);
            if (directlyResizedWindow && existingProgress < 1.0) {
                // The selected column's goal rect can update every frame while Hyprland is
                // still applying colresize.  Restarting from the current frame each time
                // makes growth lag, shrink, then expand again.  Keep the original start
                // point and only update the destination so growth stays linear.
                animation.to = dynamicRect;
            } else {
                const Rect visibleNow = currentVisibleRect();
                animation = {
                    .from = visibleNow,
                    .to = dynamicRect,
                    .lastRendered = visibleNow,
                    .start = now,
                    .hasLastRendered = true,
                };
            }
        }

        const double progress = progressFor(animation);
        if (progress >= 1.0) {
            animation.from = dynamicRect;
            animation.to = dynamicRect;
            animation.lastRendered = dynamicRect;
            animation.start = now - std::chrono::milliseconds(static_cast<int>(RELAYOUT_DURATION_MS));
            animation.hasLastRendered = true;
            if (traceTwoColumnSwap && consumeTwoColumnSwapPreviewTrace(workspace)) {
                std::ostringstream out;
                out << "[hymission] swapcol preview dynamic-tiled"
                    << " reason=complete"
                    << " window=" << debugWindowLabel(window.window)
                    << " rect=" << rectToString(dynamicRect)
                    << " activeBase=" << rectToString(activeBaseRect())
                    << " source=" << rectToString(rowGeometry->sourceGlobal)
                    << " ownSizeChanged=" << (ownSizeChanged ? 1 : 0)
                    << " anchorSizeChanged=" << (anchorSizeChanged ? 1 : 0);
                debugLog(out.str());
            }
            return dynamicRect;
        }

        if (directlyResizedWindow) {
            // The resized column already has a live Hyprland size animation.  Adding a
            // second overview-side interpolation on top of it causes visible jitter when
            // the column grows.  Render the live corrected rect directly and keep the
            // animation cache synchronized so outer windows can still slide smoothly.
            animation.from = dynamicRect;
            animation.to = dynamicRect;
            animation.lastRendered = dynamicRect;
            animation.start = now - std::chrono::milliseconds(static_cast<int>(RELAYOUT_DURATION_MS));
            animation.hasLastRendered = true;
            if (traceTwoColumnSwap && consumeTwoColumnSwapPreviewTrace(workspace)) {
                std::ostringstream out;
                out << "[hymission] swapcol preview dynamic-tiled"
                    << " reason=direct-resize"
                    << " window=" << debugWindowLabel(window.window)
                    << " rect=" << rectToString(dynamicRect)
                    << " activeBase=" << rectToString(activeBaseRect())
                    << " source=" << rectToString(rowGeometry->sourceGlobal)
                    << " ownSizeChanged=" << (ownSizeChanged ? 1 : 0)
                    << " anchorSizeChanged=" << (anchorSizeChanged ? 1 : 0);
                debugLog(out.str());
            }
            return dynamicRect;
        }

        Rect animatedRect = lerpRect(animation.from, animation.to, easeOutCubic(progress));

        animation.lastRendered = animatedRect;
        animation.hasLastRendered = true;
        if (traceTwoColumnSwap && consumeTwoColumnSwapPreviewTrace(workspace)) {
            std::ostringstream out;
            out << "[hymission] swapcol preview dynamic-tiled"
                << " reason=animated"
                << " window=" << debugWindowLabel(window.window)
                << " rect=" << rectToString(animatedRect)
                << " target=" << rectToString(dynamicRect)
                << " activeBase=" << rectToString(activeBaseRect())
                << " source=" << rectToString(rowGeometry->sourceGlobal)
                << " ownSizeChanged=" << (ownSizeChanged ? 1 : 0)
                << " anchorSizeChanged=" << (anchorSizeChanged ? 1 : 0);
            debugLog(out.str());
        }
        return animatedRect;
    };

    const auto dynamicNiriFloatingResizeRect = [&]() -> std::optional<Rect> {
        if (m_state.phase != Phase::Active || !usesDirectNiriScrollingOverview(m_state) || !window.window || !window.window->m_isMapped)
            return std::nullopt;

        PHLWINDOW floatingWindow = selectedWindow();
        const auto isPinnedOrWasPinnedFloating = [&](const PHLWINDOW& candidate) {
            const auto* managed = managedWindowFor(m_state, candidate, true);
            return candidate && (candidate->m_pinned || (managed && managed->isPinned));
        };
        const auto isFloatingOrPinnedCandidate = [&](const PHLWINDOW& candidate) {
            return candidate && (isFloatingOverviewWindow(candidate) || isPinnedOrWasPinnedFloating(candidate));
        };

        if ((!floatingWindow || !isFloatingOrPinnedCandidate(floatingWindow)) && m_state.focusDuringOverview)
            floatingWindow = m_state.focusDuringOverview;
        if (!isFloatingOrPinnedCandidate(floatingWindow))
            return std::nullopt;

        const auto* floatingManaged = managedWindowFor(m_state, floatingWindow, true);
        if (!floatingManaged || !floatingManaged->targetMonitor)
            return std::nullopt;

        const bool floatingUsesFocusedWorkspace = floatingWindow->m_pinned || floatingManaged->isPinned;
        const auto floatingWorkspace = floatingUsesFocusedWorkspace ? focusedStripWorkspaceForPreviewMonitor(floatingManaged->targetMonitor) : floatingWindow->m_workspace;
        if (!floatingWorkspace || !isScrollingWorkspace(floatingWorkspace))
            return std::nullopt;

        const bool windowUsesFocusedWorkspace = window.window->m_pinned || window.isPinned;
        const auto currentWorkspace = windowUsesFocusedWorkspace ? focusedStripWorkspaceForPreviewMonitor(window.targetMonitor) : window.window->m_workspace;
        if (currentWorkspace != floatingWorkspace)
            return std::nullopt;

        const Rect floatingLive = renderGlobalRectForWindow(floatingWindow);
        const auto overlapArea = [](const Rect& lhs, const Rect& rhs) {
            const double x1 = std::max(lhs.x, rhs.x);
            const double y1 = std::max(lhs.y, rhs.y);
            const double x2 = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
            const double y2 = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
            if (x2 <= x1 || y2 <= y1)
                return 0.0;
            return (x2 - x1) * (y2 - y1);
        };

        const ManagedWindow* anchorManaged = nullptr;
        double               bestOverlap = -1.0;
        double               bestDistance = std::numeric_limits<double>::infinity();
        for (const auto& candidateManaged : m_state.windows) {
            const auto candidate = candidateManaged.window;
            if (!candidate || candidate == floatingWindow || !candidate->m_isMapped || candidate->m_workspace != floatingWorkspace || candidate->m_pinned)
                continue;

            const auto target = candidate->layoutTarget();
            if (!target || target->floating() || isFloatingOverviewWindow(candidate))
                continue;

            const Rect candidateLive = renderGlobalRectForWindow(candidate);
            const double overlap = overlapArea(floatingLive, candidateLive);
            const double dx = candidateLive.centerX() - floatingLive.centerX();
            const double dy = candidateLive.centerY() - floatingLive.centerY();
            const double distance = dx * dx + dy * dy;
            if (!anchorManaged || overlap > bestOverlap + 1.0 || (std::abs(overlap - bestOverlap) <= 1.0 && distance < bestDistance)) {
                anchorManaged = &candidateManaged;
                bestOverlap = overlap;
                bestDistance = distance;
            }
        }

        if (!anchorManaged)
            return std::nullopt;

        const Rect anchorPreview = m_state.relayoutActive ? lerpRect(anchorManaged->relayoutFromGlobal, anchorManaged->targetGlobal, relayoutVisualProgress()) : anchorManaged->targetGlobal;
        const Rect floatingPreview = m_state.relayoutActive ? lerpRect(floatingManaged->relayoutFromGlobal, floatingManaged->targetGlobal, relayoutVisualProgress()) : floatingManaged->targetGlobal;
        const double shiftX = floatingPreview.centerX() - anchorPreview.centerX();
        const double shiftY = floatingPreview.centerY() - anchorPreview.centerY();

        if (window.window == floatingWindow) {
            const double scaleX = floatingManaged->naturalGlobal.width > 1.0 ? floatingManaged->targetGlobal.width / floatingManaged->naturalGlobal.width : 1.0;
            const double scaleY = floatingManaged->naturalGlobal.height > 1.0 ? floatingManaged->targetGlobal.height / floatingManaged->naturalGlobal.height : 1.0;
            const double width = std::max(1.0, floatingLive.width * scaleX);
            const double height = std::max(1.0, floatingLive.height * scaleY);
            return makeRect(floatingPreview.centerX() - width * 0.5, floatingPreview.centerY() - height * 0.5, width, height);
        }

        return translateRect(activeBaseRect(), shiftX, shiftY);
    };

    switch (m_state.phase) {
        case Phase::Opening:
            return lerpRect(window.naturalGlobal, window.targetGlobal, visualProgress());
        case Phase::Active:
            if (usesDirectNiriScrollingOverview(m_state)) {
                if (m_state.relayoutActive)
                    return activeBaseRect();
                if (const auto liveRect = livePreviewRectForManagedWindow(window); liveRect)
                    return *liveRect;
                if (const auto dynamicRect = dynamicNiriFloatingResizeRect(); dynamicRect)
                    return *dynamicRect;
                if (const auto dynamicRect = dynamicNiriTiledResizeRect(); dynamicRect)
                    return *dynamicRect;
                return window.targetGlobal;
            }
            if (swapColumnBackendPreviewFrozenFor(window) || pendingSwapColumnRelayoutOwnsPreviewFor(window)) {
                if (debugLogsEnabled() && window.window && window.window->m_workspace && consumeTwoColumnSwapPreviewTrace(window.window->m_workspace)) {
                    std::ostringstream out;
                    out << "[hymission] swapcol preview source=overview-owned"
                        << " window=" << debugWindowLabel(window.window)
                        << " rect=" << rectToString(activeBaseRect())
                        << " targetGlobal=" << rectToString(window.targetGlobal)
                        << " relayoutFrom=" << rectToString(window.relayoutFromGlobal)
                        << " relayoutActive=" << (m_state.relayoutActive ? 1 : 0)
                        << " pendingCommit=" << (pendingSwapColumnRelayoutOwnsPreviewFor(window) ? 1 : 0)
                        << " frozen=" << (swapColumnBackendPreviewFrozenFor(window) ? 1 : 0);
                    debugLog(out.str());
                }
                return activeBaseRect();
            }
            if (const auto dynamicRect = dynamicNiriFloatingResizeRect(); dynamicRect) {
                if (debugLogsEnabled() && window.window && window.window->m_workspace && consumeTwoColumnSwapPreviewTrace(window.window->m_workspace)) {
                    std::ostringstream out;
                    out << "[hymission] swapcol preview source=floating-dynamic"
                        << " window=" << debugWindowLabel(window.window)
                        << " rect=" << rectToString(*dynamicRect)
                        << " targetGlobal=" << rectToString(window.targetGlobal)
                        << " relayoutFrom=" << rectToString(window.relayoutFromGlobal)
                        << " relayoutActive=" << (m_state.relayoutActive ? 1 : 0);
                    debugLog(out.str());
                }
                return *dynamicRect;
            }
            if (const auto dynamicRect = dynamicNiriTiledResizeRect(); dynamicRect) {
                if (debugLogsEnabled() && window.window && window.window->m_workspace && consumeTwoColumnSwapPreviewTrace(window.window->m_workspace)) {
                    std::ostringstream out;
                    out << "[hymission] swapcol preview source=tiled-dynamic"
                        << " window=" << debugWindowLabel(window.window)
                        << " rect=" << rectToString(*dynamicRect)
                        << " targetGlobal=" << rectToString(window.targetGlobal)
                        << " relayoutFrom=" << rectToString(window.relayoutFromGlobal)
                        << " relayoutActive=" << (m_state.relayoutActive ? 1 : 0);
                    debugLog(out.str());
                }
                return *dynamicRect;
            }
            if (debugLogsEnabled() && window.window && window.window->m_workspace && consumeTwoColumnSwapPreviewTrace(window.window->m_workspace)) {
                const Rect base = activeBaseRect();
                std::ostringstream out;
                out << "[hymission] swapcol preview source=active-base"
                    << " window=" << debugWindowLabel(window.window)
                    << " rect=" << rectToString(base)
                    << " targetGlobal=" << rectToString(window.targetGlobal)
                    << " relayoutFrom=" << rectToString(window.relayoutFromGlobal)
                    << " relayoutActive=" << (m_state.relayoutActive ? 1 : 0);
                debugLog(out.str());
            }
            return activeBaseRect();
        case Phase::ClosingSettle:
            return lerpRect(window.exitGlobal, window.targetGlobal, visualProgress());
        case Phase::Closing:
            return lerpRect(window.exitGlobal, window.targetGlobal, visualProgress());
        case Phase::Inactive:
            return window.naturalGlobal;
    }

    return window.targetGlobal;
}
Rect OverviewController::emptyOverviewPlaceholderLocalRect(const PHLMONITOR& monitor, const PHLWORKSPACE& workspace, const Rect& content, const State& state) const {
    if (!monitor || content.width <= 0.0 || content.height <= 0.0)
        return {};

    double cardWidth = content.width * 0.62;
    double cardHeight = content.height * 0.62;

    if (niriModeAppliesToState(state) && workspace && workspace->m_space && isScrollingWorkspace(workspace)) {
        const CBox workAreaBox = workspace->m_space->workArea();
        Rect       baseGlobal = makeRect(workAreaBox.x, workAreaBox.y, workAreaBox.width, workAreaBox.height);
        if (state.collectionPolicy.onlyActiveWorkspace && getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 0) {
            baseGlobal = makeRect(monitor->m_position.x, monitor->m_position.y, monitor->m_size.x, monitor->m_size.y);
        }
        if (baseGlobal.width > 1.0 && baseGlobal.height > 1.0) {
            const auto overflowAxis = axisForScrollingLayoutDirection(scrollingLayoutDirection());
            const LayoutConfig config = layoutConfigForState(state);
            double niriScale = 1.0;
            if (state.collectionPolicy.onlyActiveWorkspace) {
                niriScale = std::min(content.width / baseGlobal.width, content.height / baseGlobal.height);
                niriScale *= niriLayoutScale();
            } else {
                niriScale = niriOverviewPreviewScale(content, baseGlobal, config.maxPreviewScale, config.minSlotScale, overflowAxis);
                const double viewportScale = content.width / std::max(1.0, baseGlobal.width * 4.0);
                niriScale = std::min({niriScale, niriMultiWorkspaceScale(), viewportScale});
                niriScale *= niriLayoutScale();
            }
            niriScale = std::max(config.minSlotScale, niriScale);
            cardWidth = baseGlobal.width * niriScale;
            cardHeight = baseGlobal.height * niriScale;
        }
    }

    return makeRect(content.centerX() - cardWidth * 0.5, content.centerY() - cardHeight * 0.5, cardWidth, cardHeight);
}
Rect OverviewController::currentEmptyWorkspacePlaceholderRect(const EmptyWorkspacePlaceholder& placeholder) const {
    constexpr double WALLPAPER_NATIVE_HANDOFF_VISUAL_EPSILON = 0.004;
    const auto stableExitRect = [&]() {
        if (placeholder.naturalGlobal.width > 1.0 && placeholder.naturalGlobal.height > 1.0 &&
            (placeholder.exitGlobal.width <= 1.0 || placeholder.exitGlobal.height <= 1.0 ||
             placeholder.exitGlobal.width < placeholder.targetGlobal.width * 1.05 || placeholder.exitGlobal.height < placeholder.targetGlobal.height * 1.05))
            return placeholder.naturalGlobal;

        return placeholder.exitGlobal;
    };

    if (m_gestureSession.active)
        return m_gestureSession.opening ? lerpRect(placeholder.naturalGlobal, placeholder.targetGlobal, visualProgress()) :
                                          lerpRect(stableExitRect(), placeholder.targetGlobal, visualProgress());

    switch (m_state.phase) {
        case Phase::Opening:
            return lerpRect(placeholder.naturalGlobal, placeholder.targetGlobal, visualProgress());
        case Phase::ClosingSettle:
        case Phase::Closing: {
            const double progress = visualProgress();
            if (m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state) && progress <= WALLPAPER_NATIVE_HANDOFF_VISUAL_EPSILON &&
                placeholder.naturalGlobal.width > 1.0 && placeholder.naturalGlobal.height > 1.0)
                return placeholder.naturalGlobal;

            return lerpRect(stableExitRect(), placeholder.targetGlobal, progress);
        }
        case Phase::Inactive:
            return placeholder.naturalGlobal;
        case Phase::Active:
            break;
    }

    if (m_state.relayoutActive)
        return lerpRect(placeholder.relayoutFromGlobal, placeholder.targetGlobal, relayoutVisualProgress());
    return placeholder.targetGlobal;
}
PHLWORKSPACE OverviewController::niriWorkspaceForBackground(const State& state, const EmptyWorkspacePlaceholder& background) const {
    PHLWORKSPACE workspace = background.workspace;
    const auto workspaceMatchesMonitor = [&](const PHLWORKSPACE& candidate) {
        return candidate && candidate->m_space && candidate->m_monitor.lock() == background.monitor && isScrollingWorkspace(candidate);
    };

    if (!workspaceMatchesMonitor(workspace) && background.workspaceId != WORKSPACE_INVALID) {
        if (const auto byId = g_pCompositor->getWorkspaceByID(background.workspaceId); workspaceMatchesMonitor(byId))
            workspace = byId;
    }

    if (!workspaceMatchesMonitor(workspace)) {
        const auto it = std::find_if(state.managedWorkspaces.begin(), state.managedWorkspaces.end(), [&](const PHLWORKSPACE& candidate) {
            return workspaceMatchesMonitor(candidate) && candidate->m_id == background.workspaceId;
        });
        if (it != state.managedWorkspaces.end())
            workspace = *it;
    }

    if (!workspaceMatchesMonitor(workspace)) {
        if (state.focusDuringOverview && !state.focusDuringOverview->m_pinned &&
            workspaceMatchesMonitor(state.focusDuringOverview->m_workspace))
            workspace = state.focusDuringOverview->m_workspace;
        else if (workspaceMatchesMonitor(state.ownerWorkspace))
            workspace = state.ownerWorkspace;
        else if (background.monitor && workspaceMatchesMonitor(background.monitor->m_activeWorkspace))
            workspace = background.monitor->m_activeWorkspace;
        else {
            const auto it = std::find_if(state.managedWorkspaces.begin(), state.managedWorkspaces.end(), workspaceMatchesMonitor);
            workspace = it == state.managedWorkspaces.end() ? PHLWORKSPACE{} : *it;
        }
    }

    return workspaceMatchesMonitor(workspace) ? workspace : PHLWORKSPACE{};
}
Rect OverviewController::niriWorkspaceSurfaceRect(const State& state, const EmptyWorkspacePlaceholder& background, const Rect& viewportRect,
                                                  const Rect& surfaceRect) const {
    if (!background.monitor || viewportRect.width <= 1.0 || viewportRect.height <= 1.0)
        return viewportRect;

    const auto workspace = niriWorkspaceForBackground(state, background);
    if (!workspace)
        return viewportRect;

    const CBox workspaceBox = workspace->m_space->workArea();
    const Rect workspaceRect = makeRect(workspaceBox.x, workspaceBox.y, workspaceBox.width, workspaceBox.height);
    if (workspaceRect.width <= 1.0 || workspaceRect.height <= 1.0 || surfaceRect.width <= 1.0 || surfaceRect.height <= 1.0)
        return viewportRect;

    return transformLiveOverviewRect(surfaceRect, workspaceRect, viewportRect);
}
Rect OverviewController::niriWorkspaceBackgroundRect(const State& state, const EmptyWorkspacePlaceholder& background, const Rect& viewportRect) const {
    if (!background.monitor)
        return viewportRect;

    const CBox desktopBox = background.monitor->logicalBox();
    return niriWorkspaceSurfaceRect(state, background, viewportRect,
                                    makeRect(desktopBox.x, desktopBox.y, desktopBox.width, desktopBox.height));
}
void OverviewController::renderNiriWorkspaceBackgrounds() const {
    const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!niriWallpaperZoomAppliesToMonitor(m_state, renderMonitor))
        return;

    constexpr double phaseAlpha = 1.0;

    const auto wallpaperTexture = niriWallpaperTextureForMonitor(renderMonitor);
    const auto renderBackground = [&](const Rect& globalRect, double alpha) {
        const Rect renderRect = scaleRectForRender(rectToMonitorLocal(globalRect, renderMonitor), renderMonitor);
        const float renderAlpha = static_cast<float>(clampUnit(alpha));
        if (renderRect.width <= 0.0 || renderRect.height <= 0.0 || renderAlpha <= 0.001F)
            return;

        if (wallpaperTexture) {
            g_pHyprOpenGL->renderTexture(wallpaperTexture, toBox(renderRect), {.a = renderAlpha});
            return;
        }

        auto color = niriModeWallpaperZoomBackgroundColor();
        color.a *= renderAlpha;
        g_pHyprOpenGL->renderRect(toBox(renderRect), color, {});
    };
    const auto renderWorkspace = [&](const State& state, const EmptyWorkspacePlaceholder& background, const Rect& viewportRect, double alpha) {
        renderBackground(niriWorkspaceBackgroundRect(state, background, viewportRect), alpha);

        const bool hasWorkspaceSpecificProxy = std::any_of(m_hiddenStripLayerProxies.begin(), m_hiddenStripLayerProxies.end(), [&](const HiddenStripLayerProxy& proxy) {
            return proxy.layer && proxy.monitor == renderMonitor && proxy.niriWallpaperLayoutLayer &&
                proxy.niriWallpaperWorkspaceId == background.workspaceId;
        });
        WORKSPACEID fallbackWorkspaceId = WORKSPACE_INVALID;
        if (state.ownerWorkspace && state.ownerWorkspace->m_monitor.lock() == renderMonitor && !state.ownerWorkspace->m_isSpecialWorkspace)
            fallbackWorkspaceId = state.ownerWorkspace->m_id;
        else if (renderMonitor->m_activeWorkspace && !renderMonitor->m_activeWorkspace->m_isSpecialWorkspace)
            fallbackWorkspaceId = renderMonitor->m_activeWorkspace->m_id;

        for (const auto& proxy : m_hiddenStripLayerProxies) {
            if (!proxy.layer || proxy.monitor != renderMonitor || !proxy.niriWallpaperLayoutLayer)
                continue;
            if (proxy.niriWallpaperWorkspaceId != WORKSPACE_INVALID) {
                if (hasWorkspaceSpecificProxy) {
                    if (proxy.niriWallpaperWorkspaceId != background.workspaceId)
                        continue;
                } else if (fallbackWorkspaceId != WORKSPACE_INVALID && proxy.niriWallpaperWorkspaceId != fallbackWorkspaceId) {
                    // If this adjacent empty workspace has not been focused yet,
                    // there is no workspace-specific capture for its bar/dock
                    // layers.  Reuse the active/owner workspace's layout-layer
                    // proxy for the same monitor so leaf/+1 empty lanes show the
                    // layer stack immediately.  Once that workspace is focused,
                    // its exact proxy wins through hasWorkspaceSpecificProxy.
                    continue;
                }
            } else if (hasWorkspaceSpecificProxy) {
                continue;
            }

            auto* framebuffer = proxy.framebuffer ? proxy.framebuffer.get() : nullptr;
            if (!framebuffer || !framebuffer->isAllocated() || !framebuffer->getTexture())
                continue;

            const Rect layerGlobal = niriWorkspaceSurfaceRect(state, background, viewportRect, proxy.capturedRectGlobal);
            const Rect layerRender = scaleRectForRender(rectToMonitorLocal(layerGlobal, renderMonitor), renderMonitor);
            const float layerAlpha = static_cast<float>(clampUnit(alpha));
            if (layerRender.width <= 0.0 || layerRender.height <= 0.0 || layerAlpha <= 0.001F)
                continue;

            const double proxyWidth = std::max(1.0, proxy.proxyRectGlobal.width);
            const double proxyHeight = std::max(1.0, proxy.proxyRectGlobal.height);
            const Vector2D uvTopLeft{
                std::clamp((proxy.capturedRectGlobal.x - proxy.proxyRectGlobal.x) / proxyWidth, 0.0, 1.0),
                std::clamp((proxy.capturedRectGlobal.y - proxy.proxyRectGlobal.y) / proxyHeight, 0.0, 1.0),
            };
            const Vector2D uvBottomRight{
                std::clamp((proxy.capturedRectGlobal.x + proxy.capturedRectGlobal.width - proxy.proxyRectGlobal.x) / proxyWidth, 0.0, 1.0),
                std::clamp((proxy.capturedRectGlobal.y + proxy.capturedRectGlobal.height - proxy.proxyRectGlobal.y) / proxyHeight, 0.0, 1.0),
            };
            g_pHyprOpenGL->renderTexture(framebuffer->getTexture(), toBox(layerRender),
                                         {
                                             .a = layerAlpha,
                                             .allowCustomUV = true,
                                             .primarySurfaceUVTopLeft = uvTopLeft,
                                             .primarySurfaceUVBottomRight = uvBottomRight,
                                         });
        }
    };

    if (!m_workspaceTransition.active || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle) {
        // Close must use the same camera-exit rects as the window-backed path.
        // The workspace-transition renderer ignores placeholder.exitGlobal and
        // keeps translating source/target viewports, which is the separate
        // no-window path that made empty wallpaper lanes collapse together while
        // closing.
        for (const auto& background : m_state.emptyWorkspacePlaceholders) {
            if (background.monitor == renderMonitor)
                renderWorkspace(m_state, background, currentEmptyWorkspacePlaceholderRect(background), phaseAlpha);
        }
        return;
    }

    const auto transitionMonitor = m_workspaceTransition.monitor;
    if (!transitionMonitor || transitionMonitor != renderMonitor)
        return;

    const double clampedDelta = std::clamp(m_workspaceTransition.delta, -m_workspaceTransition.distance, m_workspaceTransition.distance);
    const double sourceOffset = -clampedDelta;
    const int targetDirection =
        clampedDelta < -0.0001 ? -1 : clampedDelta > 0.0001 ? 1 : (m_workspaceTransition.step < 0 ? -1 : 1);
    const double targetOffset = sourceOffset + static_cast<double>(targetDirection) * m_workspaceTransition.distance;
    const double progress =
        m_workspaceTransition.distance > 0.0 ? clampUnit(std::abs(clampedDelta) / m_workspaceTransition.distance) : 1.0;
    const auto translated = [&](const Rect& rect, double offset) {
        return m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical ? translateRect(rect, 0.0, offset) :
                                                                                translateRect(rect, offset, 0.0);
    };
    const auto backgroundForWorkspace = [&](const State& state, WORKSPACEID workspaceId) -> const EmptyWorkspacePlaceholder* {
        const EmptyWorkspacePlaceholder* fallback = nullptr;
        for (const auto& background : state.emptyWorkspacePlaceholders) {
            if (background.monitor != renderMonitor || background.workspaceId != workspaceId)
                continue;

            if (!fallback)
                fallback = &background;

            // A window-backed scrolling workspace stores its wallpaper viewport as
            // backingOnly.  When movetoworkspace empties the source workspace, the
            // same workspace becomes a non-backing empty placeholder in the target
            // state.  Treat both as the same viewport identity so the wallpaper lane
            // scrolls from the old row to the new row instead of fading/snapping.
            if (background.backingOnly)
                return &background;
        }

        return fallback;
    };

    std::vector<WORKSPACEID> backgroundWorkspaceIds;
    const auto addBackgroundWorkspaceId = [&](const EmptyWorkspacePlaceholder& background) {
        if (background.monitor != renderMonitor || background.workspaceId == WORKSPACE_INVALID)
            return;

        if (!containsHandle(backgroundWorkspaceIds, background.workspaceId))
            backgroundWorkspaceIds.push_back(background.workspaceId);
    };

    for (const auto& background : m_workspaceTransition.sourceState.emptyWorkspacePlaceholders)
        addBackgroundWorkspaceId(background);
    for (const auto& background : m_workspaceTransition.targetState.emptyWorkspacePlaceholders)
        addBackgroundWorkspaceId(background);

    const bool directNiriRowTransition = m_workspaceTransition.sourceState.collectionPolicy.onlyActiveWorkspace &&
        (niriModeAppliesToState(m_workspaceTransition.sourceState) || niriModeAppliesToState(m_workspaceTransition.targetState));

    std::optional<Vector2D> rowShift;
    if (directNiriRowTransition) {
        double bestPrimary = -1.0;
        for (const auto workspaceId : backgroundWorkspaceIds) {
            const auto* source = backgroundForWorkspace(m_workspaceTransition.sourceState, workspaceId);
            const auto* target = backgroundForWorkspace(m_workspaceTransition.targetState, workspaceId);
            if (!source || !target)
                continue;

            const double dx = target->targetGlobal.x - source->targetGlobal.x;
            const double dy = target->targetGlobal.y - source->targetGlobal.y;
            const double primary = m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical ? std::abs(dy) : std::abs(dx);
            if (primary > bestPrimary) {
                bestPrimary = primary;
                rowShift = Vector2D{dx, dy};
            }
        }
    }

    for (const auto workspaceId : backgroundWorkspaceIds) {
        const auto* source = backgroundForWorkspace(m_workspaceTransition.sourceState, workspaceId);
        const auto* target = backgroundForWorkspace(m_workspaceTransition.targetState, workspaceId);
        if (!source && !target)
            continue;

        Rect viewportRect;
        double alpha = phaseAlpha;
        if (directNiriRowTransition) {
            if (source && target) {
                // Direct single-workspace Niri mode already stores each workspace
                // wallpaper viewport in its overview-row coordinates. Interpolate
                // those row rects directly. Adding the native full-workspace
                // renderOffset here makes target-only lanes move independently
                // from the windows and then snap into the row at commit.
                viewportRect = lerpRect(source->targetGlobal, target->targetGlobal, progress);
            } else if (source && rowShift) {
                viewportRect = lerpRect(source->targetGlobal, translateRect(source->targetGlobal, rowShift->x, rowShift->y), progress);
            } else if (target && rowShift) {
                viewportRect = lerpRect(translateRect(target->targetGlobal, -rowShift->x, -rowShift->y), target->targetGlobal, progress);
            } else if (source) {
                viewportRect = translated(source->targetGlobal, sourceOffset);
                alpha *= 1.0 - progress;
            } else {
                viewportRect = translated(target->targetGlobal, targetOffset);
                alpha *= progress;
            }
        } else if (source && target) {
            viewportRect = lerpRect(translated(source->targetGlobal, sourceOffset), translated(target->targetGlobal, targetOffset), progress);
        } else if (source) {
            viewportRect = translated(source->targetGlobal, sourceOffset);
            alpha *= 1.0 - progress;
        } else {
            viewportRect = translated(target->targetGlobal, targetOffset);
            alpha *= progress;
        }

        if (target)
            renderWorkspace(m_workspaceTransition.targetState, *target, viewportRect, alpha);
        else
            renderWorkspace(m_workspaceTransition.sourceState, *source, viewportRect, alpha);
    }
}
void OverviewController::renderEmptyOverviewPlaceholder(bool backingOnlyPass) const {
    const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!renderMonitor)
        return;

    if (niriWallpaperZoomAppliesToMonitor(m_state, renderMonitor))
        return;

    if (!shouldRenderEmptyOverviewPlaceholder(m_state, renderMonitor))
        return;

    const double progress = visualProgress();
    const double phaseAlpha = clampUnit(progress);
    constexpr double PLACEHOLDER_BASE_ALPHA = 0.24;
    const auto renderWorkspaceBackground = [&](const Rect& rect, double alpha) {
        if (rect.width <= 0.0 || rect.height <= 0.0 || alpha <= 0.001)
            return;

        g_pHyprOpenGL->renderRect(toBox(rect), CHyprColor(0.03, 0.07, 0.14, alpha * PLACEHOLDER_BASE_ALPHA),
                                  {.blur = true, .blurA = static_cast<float>(clampUnit(alpha))});
    };
    if (progress <= 0.0 && m_state.phase != Phase::Opening && m_state.phase != Phase::Closing && m_state.phase != Phase::ClosingSettle)
        return;

    bool renderedStatePlaceholder = false;
    if (!backingOnlyPass && m_workspaceTransition.active) {
        const auto monitor = m_workspaceTransition.monitor;
        if (monitor && monitor == renderMonitor) {
            const auto clampedDelta = std::clamp(m_workspaceTransition.delta, -m_workspaceTransition.distance, m_workspaceTransition.distance);
            const double sourceOffset = -clampedDelta;
            const int targetDirection = clampedDelta < -0.0001 ? -1 : clampedDelta > 0.0001 ? 1 : (m_workspaceTransition.step < 0 ? -1 : 1);
            const double targetOffset = sourceOffset + static_cast<double>(targetDirection) * m_workspaceTransition.distance;
            const double t = m_workspaceTransition.distance > 0.0 ? clampUnit(std::abs(clampedDelta) / m_workspaceTransition.distance) : 1.0;
            const auto translated = [&](const Rect& rect, double offset) {
                return m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical ? translateRect(rect, 0.0, offset) : translateRect(rect, offset, 0.0);
            };
            const auto placeholderForWorkspace = [&](const State& state, WORKSPACEID workspaceId) -> const EmptyWorkspacePlaceholder* {
                const auto it = std::find_if(state.emptyWorkspacePlaceholders.begin(), state.emptyWorkspacePlaceholders.end(),
                                             [&](const EmptyWorkspacePlaceholder& placeholder) {
                                                 return !placeholder.backingOnly && placeholder.monitor && placeholder.monitor == renderMonitor &&
                                                     placeholder.workspaceId == workspaceId;
                                             });
                return it == state.emptyWorkspacePlaceholders.end() ? nullptr : &*it;
            };

            std::unordered_set<WORKSPACEID> placeholderWorkspaceIds;
            for (const auto& placeholder : m_workspaceTransition.sourceState.emptyWorkspacePlaceholders) {
                if (!placeholder.backingOnly && placeholder.monitor && placeholder.monitor == renderMonitor)
                    placeholderWorkspaceIds.insert(placeholder.workspaceId);
            }
            for (const auto& placeholder : m_workspaceTransition.targetState.emptyWorkspacePlaceholders) {
                if (!placeholder.backingOnly && placeholder.monitor && placeholder.monitor == renderMonitor)
                    placeholderWorkspaceIds.insert(placeholder.workspaceId);
            }

            const bool directNiriRowTransition = m_workspaceTransition.sourceState.collectionPolicy.onlyActiveWorkspace &&
                (niriModeAppliesToState(m_workspaceTransition.sourceState) || niriModeAppliesToState(m_workspaceTransition.targetState));

            std::optional<Vector2D> rowShift;
            if (directNiriRowTransition) {
                double bestPrimary = -1.0;
                for (const auto workspaceId : placeholderWorkspaceIds) {
                    const auto* sourcePlaceholder = placeholderForWorkspace(m_workspaceTransition.sourceState, workspaceId);
                    const auto* targetPlaceholder = placeholderForWorkspace(m_workspaceTransition.targetState, workspaceId);
                    if (!sourcePlaceholder || !targetPlaceholder)
                        continue;

                    const double dx = targetPlaceholder->targetGlobal.x - sourcePlaceholder->targetGlobal.x;
                    const double dy = targetPlaceholder->targetGlobal.y - sourcePlaceholder->targetGlobal.y;
                    const double primary = m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical ? std::abs(dy) : std::abs(dx);
                    if (primary > bestPrimary) {
                        bestPrimary = primary;
                        rowShift = Vector2D{dx, dy};
                    }
                }
            }

            for (const auto workspaceId : placeholderWorkspaceIds) {
                const auto* sourcePlaceholder = placeholderForWorkspace(m_workspaceTransition.sourceState, workspaceId);
                const auto* targetPlaceholder = placeholderForWorkspace(m_workspaceTransition.targetState, workspaceId);
                if (!sourcePlaceholder && !targetPlaceholder)
                    continue;

                Rect transitionGlobal;
                double alpha = phaseAlpha;
                if (directNiriRowTransition) {
                    if (sourcePlaceholder && targetPlaceholder) {
                        transitionGlobal = lerpRect(sourcePlaceholder->targetGlobal, targetPlaceholder->targetGlobal, t);
                    } else if (sourcePlaceholder && rowShift) {
                        transitionGlobal = lerpRect(sourcePlaceholder->targetGlobal,
                                                    translateRect(sourcePlaceholder->targetGlobal, rowShift->x, rowShift->y), t);
                    } else if (targetPlaceholder && rowShift) {
                        transitionGlobal = lerpRect(translateRect(targetPlaceholder->targetGlobal, -rowShift->x, -rowShift->y),
                                                    targetPlaceholder->targetGlobal, t);
                    } else if (sourcePlaceholder) {
                        transitionGlobal = translated(sourcePlaceholder->targetGlobal, sourceOffset);
                        alpha *= 1.0 - t;
                    } else {
                        transitionGlobal = translated(targetPlaceholder->targetGlobal, targetOffset);
                        alpha *= t;
                    }
                } else if (sourcePlaceholder && targetPlaceholder) {
                    const Rect sourceRect = translated(sourcePlaceholder->targetGlobal, sourceOffset);
                    const Rect targetRect = translated(targetPlaceholder->targetGlobal, targetOffset);
                    transitionGlobal = lerpRect(sourceRect, targetRect, t);
                } else if (sourcePlaceholder) {
                    transitionGlobal = translated(sourcePlaceholder->targetGlobal, sourceOffset);
                    alpha *= 1.0 - t;
                } else {
                    transitionGlobal = translated(targetPlaceholder->targetGlobal, targetOffset);
                    alpha *= t;
                }

                const Rect targetLocal = rectToMonitorLocal(transitionGlobal, renderMonitor);
                const Rect placeholderRender = scaleRectForRender(targetLocal, renderMonitor);
                if (placeholderRender.width <= 0.0 || placeholderRender.height <= 0.0 || alpha <= 0.001)
                    continue;

                renderWorkspaceBackground(placeholderRender, alpha);
                renderedStatePlaceholder = true;
            }
        }
    } else {
        for (const auto& placeholder : m_state.emptyWorkspacePlaceholders) {
            if (placeholder.backingOnly != backingOnlyPass || !placeholder.monitor || placeholder.monitor != renderMonitor)
                continue;

            const Rect targetLocal = rectToMonitorLocal(currentEmptyWorkspacePlaceholderRect(placeholder), renderMonitor);
            const Rect placeholderRender = scaleRectForRender(targetLocal, renderMonitor);
            if (placeholderRender.width <= 0.0 || placeholderRender.height <= 0.0)
                continue;

            const double alpha = phaseAlpha;
            if (alpha <= 0.001)
                continue;

            renderWorkspaceBackground(placeholderRender, alpha);
            renderedStatePlaceholder = true;
        }
    }
    if (renderedStatePlaceholder)
        return;

    if (backingOnlyPass)
        return;

    const bool hasWindowOnRenderMonitor = std::ranges::any_of(m_state.windows, [&](const ManagedWindow& managed) {
        return managed.window && managed.targetMonitor == renderMonitor;
    });
    if (hasWindowOnRenderMonitor)
        return;

    const Rect content = overviewContentRectForMonitor(renderMonitor, m_state);
    if (content.width <= 0.0 || content.height <= 0.0)
        return;

    Rect   sourceLocal = makeRect(0.0, 0.0, renderMonitor->m_size.x, renderMonitor->m_size.y);

    const auto workspace = m_state.ownerWorkspace ? m_state.ownerWorkspace : renderMonitor->m_activeWorkspace;
    if (niriModeAppliesToState(m_state) && workspace && workspace->m_space && isScrollingWorkspace(workspace)) {
        const CBox workAreaBox = workspace->m_space->workArea();
        const Rect baseGlobal = makeRect(workAreaBox.x, workAreaBox.y, workAreaBox.width, workAreaBox.height);
        if (baseGlobal.width > 1.0 && baseGlobal.height > 1.0) {
            sourceLocal = rectToMonitorLocal(baseGlobal, renderMonitor);
        }
    }

    const Rect placeholderLocal = emptyOverviewPlaceholderLocalRect(renderMonitor, workspace, content, m_state);
    Rect       currentLocal = placeholderLocal;
    if (m_gestureSession.active) {
        currentLocal = lerpRect(sourceLocal, placeholderLocal, progress);
    } else {
        switch (m_state.phase) {
            case Phase::Opening:
                currentLocal = lerpRect(sourceLocal, placeholderLocal, progress);
                break;
            case Phase::ClosingSettle:
            case Phase::Closing:
                currentLocal = lerpRect(sourceLocal, placeholderLocal, progress);
                break;
            case Phase::Active:
                currentLocal = placeholderLocal;
                break;
            case Phase::Inactive:
                currentLocal = sourceLocal;
                break;
        }
    }
    const Rect placeholderRender = scaleRectForRender(currentLocal, renderMonitor);

    const double alpha = phaseAlpha;
    if (alpha <= 0.001)
        return;

    renderWorkspaceBackground(placeholderRender, alpha);
}
void OverviewController::buildWorkspaceStripEntries(State& state) const {
    state.stripEntries.clear();
    state.hoveredStripIndex.reset();

    if (!workspaceStripEnabled(state))
        return;

    const std::string            anchor = workspaceStripAnchor();
    const WorkspaceStripEmptyMode emptyMode = workspaceStripEmptyMode();
    const bool                   horizontal = anchor == "top";
    const double                 stripGap = std::clamp(workspaceStripGap() * 0.5, 8.0, 24.0);
    const double                 padding = 12.0;
    std::unordered_set<WORKSPACEID> workspacesWithWindows;
    for (const auto& window : g_pCompositor->m_windows) {
        if (!window || !window->m_workspace || !window->m_isMapped || window->m_fadingOut || window->isHidden())
            continue;

        workspacesWithWindows.insert(window->m_workspace->m_id);
    }

    for (const auto& monitor : state.participatingMonitors) {
        if (!monitor)
            continue;

        PHLWORKSPACE stripActiveWorkspace = monitor->m_activeWorkspace;
        if (niriModeEnabled() && state.collectionPolicy.onlyActiveWorkspace && state.ownerWorkspace &&
            state.ownerWorkspace->m_monitor.lock() == monitor && isScrollingWorkspace(state.ownerWorkspace))
            stripActiveWorkspace = state.ownerWorkspace;

        const bool singleWorkspaceScrollingNiri =
            niriModeEnabled() && state.collectionPolicy.onlyActiveWorkspace && isScrollingWorkspace(stripActiveWorkspace);

        std::unordered_set<WORKSPACEID> forcedStripWorkspaceIds;
        const auto forceStripWorkspaceId = [&](WORKSPACEID workspaceId) {
            if (workspaceId != WORKSPACE_INVALID)
                forcedStripWorkspaceIds.insert(workspaceId);
        };

        if (singleWorkspaceScrollingNiri) {
            if (stripActiveWorkspace) {
                forceStripWorkspaceId(stripActiveWorkspace->m_id);
                if (stripActiveWorkspace->m_id > 1)
                    forceStripWorkspaceId(stripActiveWorkspace->m_id - 1);
                forceStripWorkspaceId(stripActiveWorkspace->m_id + 1);
            }

            for (const auto workspaceId : niri_scrolling_detail::retainedDirectNiriWorkspaceLaneIds(monitor->m_id))
                forceStripWorkspaceId(workspaceId);

            // Preserve the lanes that were already rendered in the active overview
            // state.  Niri keeps workspace rows visually stable while windows move
            // between workspaces; trimming the previous empty/source row during a
            // post-move rebuild is what makes the adjacent workspace disappear.
            if (isVisible()) {
                for (const auto& entry : m_state.stripEntries) {
                    if (entry.monitor == monitor && !entry.newWorkspaceSlot)
                        forceStripWorkspaceId(entry.workspaceId);
                }

                for (const auto& placeholder : m_state.emptyWorkspacePlaceholders) {
                    if (placeholder.monitor == monitor && placeholder.workspaceId != WORKSPACE_INVALID)
                        forceStripWorkspaceId(placeholder.workspaceId);
                }
            }
        }

        std::vector<PHLWORKSPACE> normalWorkspaces;
        const auto allWorkspaces = g_pCompositor->getWorkspacesCopy();
        normalWorkspaces.reserve(allWorkspaces.size());
        for (const auto& workspace : allWorkspaces) {
            if (!workspace || workspace->m_isSpecialWorkspace)
                continue;

            const auto workspaceMonitor = workspace->m_monitor.lock();
            if (workspaceMonitor == monitor)
                normalWorkspaces.push_back(workspace);
        }

        if (stripActiveWorkspace && !stripActiveWorkspace->m_isSpecialWorkspace && !containsHandle(normalWorkspaces, stripActiveWorkspace))
            normalWorkspaces.push_back(stripActiveWorkspace);

        if (singleWorkspaceScrollingNiri) {
            for (const auto& window : g_pCompositor->m_windows) {
                if (!window || !window->m_isMapped || window->m_fadingOut || !window->m_workspace || window->m_workspace->m_isSpecialWorkspace)
                    continue;

                const auto workspaceMonitor = window->m_workspace->m_monitor.lock();
                if (workspaceMonitor == monitor && !containsHandle(normalWorkspaces, window->m_workspace))
                    normalWorkspaces.push_back(window->m_workspace);
            }
        }

        std::stable_sort(normalWorkspaces.begin(), normalWorkspaces.end(), [](const PHLWORKSPACE& lhs, const PHLWORKSPACE& rhs) {
            if (!lhs || !rhs)
                return static_cast<bool>(lhs);
            return lhs->m_id < rhs->m_id;
        });

        std::unordered_map<WORKSPACEID, PHLWORKSPACE> normalById;
        for (const auto& workspace : normalWorkspaces) {
            if (workspace)
                normalById.emplace(workspace->m_id, workspace);
        }

        if (singleWorkspaceScrollingNiri) {
            // Object permanence for the niri overview starts with the strip:
            // every real workspace on this monitor must stay represented, even
            // if the active centered workspace is the only one Hyprland just
            // exposed through the live focus path.
            for (const auto& workspace : normalWorkspaces) {
                if (workspace)
                    forceStripWorkspaceId(workspace->m_id);
            }
        }

        std::vector<int64_t> workspaceIds;
        workspaceIds.reserve(normalWorkspaces.size() + forcedStripWorkspaceIds.size());
        for (const auto& workspace : normalWorkspaces) {
            if (workspace)
                workspaceIds.push_back(workspace->m_id);
        }

        for (const auto workspaceId : forcedStripWorkspaceIds) {
            const auto id = static_cast<int64_t>(workspaceId);
            if (std::find(workspaceIds.begin(), workspaceIds.end(), id) == workspaceIds.end())
                workspaceIds.push_back(id);
        }

        std::sort(workspaceIds.begin(), workspaceIds.end());
        workspaceIds.erase(std::unique(workspaceIds.begin(), workspaceIds.end()), workspaceIds.end());

        // Direct Niri overview rows must be compact and index-based, like Niri's
        // Monitor::workspaces_render_geo().  Expanding numeric workspace ids here
        // creates fake holes (for example 1..10), which makes the real neighboring
        // workspaces get assigned far-away row indexes and visibly slide offscreen.
        const auto stripWorkspaceIds = singleWorkspaceScrollingNiri ? workspaceIds : expandWorkspaceStripWorkspaceIds(workspaceIds, emptyMode);

        std::vector<WorkspaceStripEntry> monitorEntries;
        for (const auto rawWorkspaceId : stripWorkspaceIds) {
            const auto workspaceId = static_cast<WORKSPACEID>(rawWorkspaceId);
            const auto it = normalById.find(workspaceId);
            if (it != normalById.end()) {
                monitorEntries.push_back({
                    .monitor = monitor,
                    .workspace = it->second,
                    .workspaceId = workspaceId,
                    .workspaceName = it->second->m_name,
                    .syntheticEmpty = false,
                    .newWorkspaceSlot = false,
                    .active = stripActiveWorkspace == it->second,
                });
            } else {
                monitorEntries.push_back({
                    .monitor = monitor,
                    .workspace = {},
                    .workspaceId = workspaceId,
                    .workspaceName = std::to_string(workspaceId),
                    .syntheticEmpty = true,
                    .newWorkspaceSlot = false,
                    .active = false,
                });
            }
        }

        if (singleWorkspaceScrollingNiri) {
            const auto trimEdgeEntries = [&](bool fromFront) {
                while (!monitorEntries.empty()) {
                    const auto& entry = fromFront ? monitorEntries.front() : monitorEntries.back();
                    if (entry.active || workspacesWithWindows.contains(entry.workspaceId) || forcedStripWorkspaceIds.contains(entry.workspaceId))
                        break;

                    if (fromFront)
                        monitorEntries.erase(monitorEntries.begin());
                    else
                        monitorEntries.pop_back();
                }
            };
            trimEdgeEntries(true);
            trimEdgeEntries(false);
        }

        WORKSPACEID nextWorkspaceId = stripWorkspaceIds.empty() ? 1 : static_cast<WORKSPACEID>(std::max<int64_t>(stripWorkspaceIds.back(), 0) + 1);
        while (g_pCompositor->getWorkspaceByID(nextWorkspaceId))
            ++nextWorkspaceId;

        monitorEntries.push_back({
            .monitor = monitor,
            .workspace = {},
            .workspaceId = nextWorkspaceId,
            .workspaceName = std::to_string(nextWorkspaceId),
            .syntheticEmpty = true,
            .newWorkspaceSlot = true,
            .active = false,
        });

        if (monitorEntries.empty())
            continue;

        const Rect band = workspaceStripBandRectForMonitor(monitor, state);
        if (singleWorkspaceScrollingNiri) {
            std::optional<std::size_t> activeIndex;
            for (std::size_t index = 0; index < monitorEntries.size(); ++index) {
                if (monitorEntries[index].active) {
                    activeIndex = index;
                    break;
                }
            }

            const double aspect = monitor->m_size.y > 0.0 ? static_cast<double>(monitor->m_size.x) / static_cast<double>(monitor->m_size.y) : (16.0 / 9.0);
            const double scale = niriWorkspaceScale();
            const auto slots =
                layoutNiriWorkspaceStripSlots(band, parseWorkspaceStripAnchor(anchor), monitorEntries.size(), activeIndex, stripGap, padding, aspect, scale);
            for (std::size_t index = 0; index < std::min(slots.size(), monitorEntries.size()); ++index) {
                monitorEntries[index].rect = slots[index];
            }

            state.stripEntries.insert(state.stripEntries.end(), monitorEntries.begin(), monitorEntries.end());
            continue;
        }

        const double innerWidth = std::max(1.0, band.width - padding * 2.0);
        const double innerHeight = std::max(1.0, band.height - padding * 2.0);
        double scale = horizontal ? innerHeight / static_cast<double>(monitor->m_size.y) : innerWidth / static_cast<double>(monitor->m_size.x);
        double entryWidth = std::max(24.0, static_cast<double>(monitor->m_size.x) * scale);
        double entryHeight = std::max(24.0, static_cast<double>(monitor->m_size.y) * scale);

        if (horizontal) {
            const double totalWidth = entryWidth * monitorEntries.size() + stripGap * std::max<std::size_t>(monitorEntries.size() - 1, 0);
            if (totalWidth > innerWidth) {
                const double fitScale = innerWidth / std::max(1.0, totalWidth);
                entryWidth *= fitScale;
                entryHeight *= fitScale;
            }

            const double effectiveGap = monitorEntries.size() > 1 ? std::min(stripGap, std::max(4.0, (innerWidth - entryWidth * monitorEntries.size()) / (monitorEntries.size() - 1))) : 0.0;
            const double contentWidth = entryWidth * monitorEntries.size() + effectiveGap * std::max<std::size_t>(monitorEntries.size() - 1, 0);
            double cursorX = band.x + padding + std::max(0.0, (innerWidth - contentWidth) * 0.5);
            const double y = band.y + (band.height - entryHeight) * 0.5;
            for (auto& entry : monitorEntries) {
                entry.rect = makeRect(cursorX, y, entryWidth, entryHeight);
                cursorX += entryWidth + effectiveGap;
            }
        } else {
            const double totalHeight = entryHeight * monitorEntries.size() + stripGap * std::max<std::size_t>(monitorEntries.size() - 1, 0);
            if (totalHeight > innerHeight) {
                const double fitScale = innerHeight / std::max(1.0, totalHeight);
                entryWidth *= fitScale;
                entryHeight *= fitScale;
            }

            const double effectiveGap = monitorEntries.size() > 1 ? std::min(stripGap, std::max(4.0, (innerHeight - entryHeight * monitorEntries.size()) / (monitorEntries.size() - 1))) : 0.0;
            const double contentHeight = entryHeight * monitorEntries.size() + effectiveGap * std::max<std::size_t>(monitorEntries.size() - 1, 0);
            const double x = band.x + (band.width - entryWidth) * 0.5;
            double cursorY = band.y + padding + std::max(0.0, (innerHeight - contentHeight) * 0.5);
            for (auto& entry : monitorEntries) {
                entry.rect = makeRect(x, cursorY, entryWidth, entryHeight);
                cursorY += entryHeight + effectiveGap;
            }
        }

        state.stripEntries.insert(state.stripEntries.end(), monitorEntries.begin(), monitorEntries.end());
    }

    const auto focusWindow = state.focusDuringOverview ? state.focusDuringOverview : Desktop::focusState()->window();
    for (const auto& window : g_pCompositor->m_windows) {
        if (!window || !window->m_isMapped || window->m_fadingOut || window->isHidden())
            continue;

        if (!windowHasUsableStateGeometry(window))
            continue;

        const bool useGoalGeometry = shouldUseGoalGeometryForStateSnapshot(window);
        const auto naturalGlobal = stateSnapshotGlobalRectForWindow(window, useGoalGeometry);
        const auto previewAlpha = std::clamp(window->alphaTotal(), 0.0F, 1.0F);
        const auto targetMonitor = preferredMonitorForWindow(window, state);

        for (auto& entry : state.stripEntries) {
            if (!entry.monitor)
                continue;

            if (window->m_pinned) {
                if (entry.monitor == targetMonitor && entry.workspace == entry.monitor->m_activeWorkspace) {
                    entry.windows.push_back({
                        .window = window,
                        .naturalGlobal = naturalGlobal,
                        .alpha = previewAlpha,
                        .focused = window == focusWindow,
                    });
                }
                continue;
            }

            if (window->m_workspace && entry.workspace == window->m_workspace) {
                entry.windows.push_back({
                    .window = window,
                    .naturalGlobal = naturalGlobal,
                    .alpha = previewAlpha,
                    .focused = window == focusWindow,
                });
            }
        }
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] strip build entries=" << state.stripEntries.size() << " monitors=" << state.participatingMonitors.size() << " anchor=" << anchor;
        for (std::size_t i = 0; i < state.stripEntries.size() && i < 8; ++i) {
            const auto& entry = state.stripEntries[i];
            out << " | #" << i << " mon=" << (entry.monitor ? entry.monitor->m_name : "?") << " ws=" << entry.workspaceId
                << " rect=" << rectToString(entry.rect) << " windows=" << entry.windows.size() << " active=" << (entry.active ? 1 : 0)
                << " new=" << (entry.newWorkspaceSlot ? 1 : 0);
        }
        debugLog(out.str());
    }
}
OverviewController::State OverviewController::buildState(const PHLMONITOR& monitor, ScopeOverride requestedScope, const std::vector<WorkspaceOverride>& workspaceOverrides,
                                                         bool keepEmptyParticipatingMonitors, bool suppressWorkspaceStrip,
                                                         PHLWINDOW preferredSelectedWindow, bool refreshLayoutSnapshots) const {
    State state;
    if (!monitor)
        return state;

    const bool preserveExistingOrder =
        workspaceOverrides.empty() && isVisible() && requestedScope == m_state.collectionPolicy.requestedScope && (!m_state.ownerMonitor || monitor == m_state.ownerMonitor);

    state.ownerMonitor = monitor;
    state.ownerWorkspace = monitor->m_activeWorkspace;
    state.collectionPolicy = loadCollectionPolicy(requestedScope);

    const auto niriOwnerWorkspaceCandidateForWindow = [&](const PHLWINDOW& candidate) -> PHLWORKSPACE {
        if (!niriModeEnabled() || !state.collectionPolicy.onlyActiveWorkspace || g_niriStripSnapshotSingleWorkspaceOnly)
            return {};
        if (!candidate || !candidate->m_isMapped || candidate->m_pinned || !candidate->m_workspace || candidate->m_workspace->m_isSpecialWorkspace)
            return {};

        const auto candidateWorkspace = candidate->m_workspace;
        if (!isScrollingWorkspace(candidateWorkspace))
            return {};

        const auto candidateMonitor = candidateWorkspace->m_monitor.lock();
        if (candidateMonitor != monitor)
            return {};

        // While the overview is open, the selected window is allowed to become the live
        // single-workspace owner. When opening from outside overview, only trust the
        // focused window if Hyprland already activated its workspace; this prevents stale
        // focus after silent workspace moves from pulling the overview to the wrong lane.
        if (!isVisible() && monitor->m_activeWorkspace != candidateWorkspace)
            return {};

        return candidateWorkspace;
    };

    if (const auto ownerFromPreferred = niriOwnerWorkspaceCandidateForWindow(preferredSelectedWindow); ownerFromPreferred)
        state.ownerWorkspace = ownerFromPreferred;
    else if (const auto ownerFromFocus = niriOwnerWorkspaceCandidateForWindow(Desktop::focusState()->window()); ownerFromFocus)
        state.ownerWorkspace = ownerFromFocus;

    PHLWORKSPACE stripPreviewWorkspace;
    if (g_niriStripSnapshotSingleWorkspaceOnly) {
        const auto stripOverride = std::find_if(workspaceOverrides.begin(), workspaceOverrides.end(),
                                                [&](const WorkspaceOverride& override) { return override.monitorId == monitor->m_id && override.workspace; });
        if (stripOverride != workspaceOverrides.end())
            stripPreviewWorkspace = stripOverride->workspace;
    }
    const auto niriDirectWorkspace = stripPreviewWorkspace ? stripPreviewWorkspace : state.ownerWorkspace;
    const bool niriDirectSingleWorkspaceOverview =
        niriModeEnabled() && state.collectionPolicy.onlyActiveWorkspace && isScrollingWorkspace(niriDirectWorkspace);
    const bool niriExpandsSingleWorkspaceOverview = niriDirectSingleWorkspaceOverview && !g_niriStripSnapshotSingleWorkspaceOnly;
    if (niriDirectSingleWorkspaceOverview)
        state.collectionPolicy.onlyActiveMonitor = true;
    state.suppressWorkspaceStrip = suppressWorkspaceStrip;
    const auto addMonitor = [&](const PHLMONITOR& candidate) {
        if (!candidate || containsHandle(state.participatingMonitors, candidate))
            return;
        state.participatingMonitors.push_back(candidate);
    };

    if (state.collectionPolicy.onlyActiveMonitor) {
        addMonitor(monitor);
    } else {
        for (const auto& candidate : g_pCompositor->m_monitors)
            addMonitor(candidate);
    }

    const auto addWorkspace = [&](const PHLWORKSPACE& workspace) {
        if (!workspace || containsHandle(state.managedWorkspaces, workspace))
            return;
        state.managedWorkspaces.push_back(workspace);
    };

    const auto overrideForMonitor = [&](const PHLMONITOR& candidateMonitor) -> const WorkspaceOverride* {
        const auto it = std::find_if(workspaceOverrides.begin(), workspaceOverrides.end(),
                                     [&](const WorkspaceOverride& override) { return candidateMonitor && override.monitorId == candidateMonitor->m_id; });
        return it == workspaceOverrides.end() ? nullptr : &*it;
    };

    if (state.collectionPolicy.onlyActiveWorkspace && !niriExpandsSingleWorkspaceOverview) {
        for (const auto& candidateMonitor : state.participatingMonitors) {
            if (!candidateMonitor)
                continue;

            if (const auto* override = overrideForMonitor(candidateMonitor); override) {
                if (override->workspace)
                    addWorkspace(override->workspace);
                continue;
            }

            if (candidateMonitor->m_activeWorkspace)
                addWorkspace(candidateMonitor->m_activeWorkspace);
        }
    } else {
        for (const auto& workspace : g_pCompositor->getWorkspacesCopy()) {
            if (!workspace || workspace->m_isSpecialWorkspace)
                continue;

            const auto workspaceMonitor = workspace->m_monitor.lock();
            if (workspaceMonitor && containsHandle(state.participatingMonitors, workspaceMonitor))
                addWorkspace(workspace);
        }
    }

    if (state.collectionPolicy.includeSpecial) {
        for (const auto& candidateMonitor : state.participatingMonitors) {
            if (candidateMonitor && candidateMonitor->m_activeSpecialWorkspace)
                addWorkspace(candidateMonitor->m_activeSpecialWorkspace);
        }
    }

    if (workspaceRowsEnabled(m_handle)) {
        std::stable_sort(state.managedWorkspaces.begin(), state.managedWorkspaces.end(), [](const PHLWORKSPACE& lhs, const PHLWORKSPACE& rhs) {
            if (!lhs || !rhs)
                return static_cast<bool>(lhs);

            if (lhs->m_isSpecialWorkspace != rhs->m_isSpecialWorkspace)
                return !lhs->m_isSpecialWorkspace;

            return lhs->m_id < rhs->m_id;
        });
    }

    const auto focusedWindow = Desktop::focusState()->window();
    const auto scopedFocusedWindow = windowMatchesOverviewScope(focusedWindow, state, false) ? focusedWindow : PHLWINDOW{};
    const auto scopedPreferredWindow = windowMatchesOverviewScope(preferredSelectedWindow, state, false) ? preferredSelectedWindow : PHLWINDOW{};
    state.focusBeforeOpen = scopedFocusedWindow;
    state.focusDuringOverview = scopedPreferredWindow ? scopedPreferredWindow : scopedFocusedWindow;
    if (!g_niriStripSnapshotSingleWorkspaceOnly && niriModeEnabled() && state.collectionPolicy.onlyActiveWorkspace && state.focusDuringOverview &&
        !state.focusDuringOverview->m_pinned && state.focusDuringOverview->m_workspace &&
        !state.focusDuringOverview->m_workspace->m_isSpecialWorkspace && isScrollingWorkspace(state.focusDuringOverview->m_workspace) &&
        state.focusDuringOverview->m_workspace->m_monitor.lock() == monitor &&
        (isVisible() || monitor->m_activeWorkspace == state.focusDuringOverview->m_workspace))
        state.ownerWorkspace = state.focusDuringOverview->m_workspace;

    const auto focusedStripWorkspaceForMonitor = [&](const PHLMONITOR& candidateMonitor) -> PHLWORKSPACE {
        if (!candidateMonitor)
            return {};

        if (const auto* override = overrideForMonitor(candidateMonitor); override) {
            if (override->workspace)
                return override->workspace;

            if (override->workspaceId != WORKSPACE_INVALID) {
                const auto it = std::find_if(state.managedWorkspaces.begin(),
                                             state.managedWorkspaces.end(),
                                             [&](const PHLWORKSPACE& workspace) { return workspace && workspace->m_id == override->workspaceId; });
                if (it != state.managedWorkspaces.end())
                    return *it;
            }
        }

        if (state.focusDuringOverview && !state.focusDuringOverview->m_pinned && state.focusDuringOverview->m_workspace) {
            const auto focusedWorkspaceMonitor = state.focusDuringOverview->m_workspace->m_monitor.lock();
            if (focusedWorkspaceMonitor == candidateMonitor)
                return state.focusDuringOverview->m_workspace;
        }

        if (candidateMonitor->m_activeWorkspace)
            return candidateMonitor->m_activeWorkspace;

        if (state.ownerWorkspace && state.ownerWorkspace->m_monitor.lock() == candidateMonitor)
            return state.ownerWorkspace;

        return {};
    };

    for (const auto& workspace : state.managedWorkspaces) {
        if (!workspace)
            continue;

        FullscreenWorkspaceBackup backup;
        backup.workspace = workspace;
        backup.hadFullscreenWindow = workspace->m_hasFullscreenWindow;
        backup.fullscreenMode = workspace->m_hasFullscreenWindow ? workspace->m_fullscreenMode : FSMODE_NONE;
        if (workspace->m_hasFullscreenWindow) {
            backup.originalFullscreenWindow = workspace->getFullscreenWindow();
            backup.originalFullscreenMode = workspace->m_fullscreenMode;
        }

        if (!backup.originalFullscreenWindow || backup.originalFullscreenMode == FSMODE_NONE) {
            for (const auto& window : g_pCompositor->m_windows) {
                if (!window || !window->m_isMapped || window->m_workspace != workspace || window->m_fullscreenState.internal == FSMODE_NONE)
                    continue;

                backup.originalFullscreenWindow = window;
                backup.originalFullscreenMode = window->m_fullscreenState.internal;
                backup.hadFullscreenWindow = true;
                backup.fullscreenMode = window->m_fullscreenState.internal;
                break;
            }
        }

        state.fullscreenBackups.push_back(backup);
    }

    std::vector<PHLWINDOW> candidates;
    candidates.reserve(g_pCompositor->m_windows.size());

    const auto appendCandidate = [&](const PHLWINDOW& window) {
        if (!window || containsHandle(candidates, window))
            return;
        candidates.push_back(window);
    };

    for (const auto& workspace : state.managedWorkspaces) {
        if (!workspace || !workspace->m_space)
            continue;

        for (const auto& targetRef : workspace->m_space->targets()) {
            const auto target = targetRef.lock();
            if (!target)
                continue;

            const auto targetWindow = target->window();
            if (!targetWindow || targetWindow->m_workspace != workspace)
                continue;

            const auto liveTarget = targetWindow->layoutTarget();
            if (!liveTarget || liveTarget != target)
                continue;

            appendCandidate(targetWindow);
        }
    }

    for (const auto& window : g_pCompositor->m_windows) {
        if (!window)
            continue;

        appendCandidate(window);
    }

    if (preserveExistingOrder && !m_state.windows.empty()) {
        std::unordered_map<PHLWINDOW, std::size_t> previousOrder;
        previousOrder.reserve(m_state.windows.size());

        std::vector<std::size_t> visibleOrder;
        visibleOrder.reserve(m_state.windows.size());
        for (std::size_t i = 0; i < m_state.windows.size(); ++i) {
            if (m_state.windows[i].window)
                visibleOrder.push_back(i);
        }

        // Preserve the currently visible preview order rather than the raw
        // storage order. Selection-emphasis relayouts should only resize and
        // nudge neighbors, not reshuffle previews that are already on screen.
        std::stable_sort(visibleOrder.begin(), visibleOrder.end(), [&](std::size_t lhsIndex, std::size_t rhsIndex) {
            const auto& lhs = m_state.windows[lhsIndex];
            const auto& rhs = m_state.windows[rhsIndex];
            const auto lhsMonitorId = lhs.targetMonitor ? lhs.targetMonitor->m_id : MONITOR_INVALID;
            const auto rhsMonitorId = rhs.targetMonitor ? rhs.targetMonitor->m_id : MONITOR_INVALID;
            if (lhsMonitorId != rhsMonitorId)
                return lhsMonitorId < rhsMonitorId;

            const Rect lhsRect = stablePreviewOrderRect(lhs);
            const Rect rhsRect = stablePreviewOrderRect(rhs);
            if (std::abs(lhsRect.y - rhsRect.y) > 0.5)
                return lhsRect.y < rhsRect.y;
            if (std::abs(lhsRect.x - rhsRect.x) > 0.5)
                return lhsRect.x < rhsRect.x;
            return lhsIndex < rhsIndex;
        });

        for (std::size_t order = 0; order < visibleOrder.size(); ++order) {
            const auto& managed = m_state.windows[visibleOrder[order]];
            if (managed.window)
                previousOrder.emplace(managed.window, order);
        }

        std::stable_sort(candidates.begin(), candidates.end(), [&](const PHLWINDOW& lhs, const PHLWINDOW& rhs) {
            const auto lhsIt = previousOrder.find(lhs);
            const auto rhsIt = previousOrder.find(rhs);
            const bool lhsKnown = lhsIt != previousOrder.end();
            const bool rhsKnown = rhsIt != previousOrder.end();

            if (lhsKnown != rhsKnown)
                return lhsKnown;
            if (!lhsKnown)
                return false;

            return lhsIt->second < rhsIt->second;
        });
    }

    const bool orderByRecentUse = !preserveExistingOrder && shouldUseRecentWindowOrdering(state);
    if (orderByRecentUse && !m_windowMruSerials.empty()) {
        std::stable_sort(candidates.begin(), candidates.end(), [&](const PHLWINDOW& lhs, const PHLWINDOW& rhs) {
            const auto lhsIt = m_windowMruSerials.find(lhs);
            const auto rhsIt = m_windowMruSerials.find(rhs);
            const bool lhsKnown = lhsIt != m_windowMruSerials.end();
            const bool rhsKnown = rhsIt != m_windowMruSerials.end();

            if (lhsKnown != rhsKnown)
                return lhsKnown;
            if (!lhsKnown)
                return false;
            if (lhsIt->second != rhsIt->second)
                return lhsIt->second > rhsIt->second;

            return false;
        });
    }

    std::unordered_map<MONITORID, std::vector<WindowInput>> inputsByMonitor;
    std::unordered_map<MONITORID, std::size_t> directNiriOverviewWindowsByMonitor;
    std::unordered_map<WORKSPACEID, Rect> niriWorkspaceLaneById;
    std::unordered_map<MONITORID, std::vector<WORKSPACEID>> niriSyntheticLaneWorkspaceIdsByMonitor;
    std::unordered_map<MONITORID, std::unordered_set<WORKSPACEID>> directNiriVisibleWorkspaceIdsByMonitor;
    std::unordered_set<WORKSPACEID> niriFitBackingPlaceholderWorkspaces;
    const bool useWorkspaceRows = workspaceRowsEnabled(m_handle);
    LayoutConfig config = layoutConfigForState(state);
    config.preserveInputOrder = preserveExistingOrder || orderByRecentUse;
    config.forceRowGroups = useWorkspaceRows;
    config.rankScaleByInputOrder = orderByRecentUse;
    const bool allowDirectNiriOverviewLayout = niriDirectSingleWorkspaceOverview;
    const double niriActiveWorkspaceLayoutScale =
        niriModeAppliesToState(state) && !g_niriStripSnapshotSingleWorkspaceOnly ? niriLayoutScale() : 1.0;
    std::unordered_map<MONITORID, std::pair<WORKSPACEID, WORKSPACEID>> placeholderRangeByMonitor;

    if (allowDirectNiriOverviewLayout) {
        std::unordered_map<MONITORID, std::unordered_set<WORKSPACEID>> directNiriWorkspacesWithWindowsByMonitor;
        for (const auto& window : candidates) {
            if (!shouldManageWindow(window, state) || !window->m_isMapped)
                continue;

            const auto targetMonitor = preferredMonitorForWindow(window, state);
            if (!targetMonitor)
                continue;

            const auto workspace = window->m_pinned ? focusedStripWorkspaceForMonitor(targetMonitor) : window->m_workspace;
            if (workspace && isScrollingWorkspace(workspace))
                directNiriWorkspacesWithWindowsByMonitor[targetMonitor->m_id].insert(workspace->m_id);
        }

        for (const auto& candidateMonitor : state.participatingMonitors) {
            if (!candidateMonitor)
                continue;

            std::vector<PHLWORKSPACE> monitorWorkspaces;
            for (const auto& workspace : state.managedWorkspaces) {
                if (!workspace || workspace->m_isSpecialWorkspace)
                    continue;

                const auto workspaceMonitor = workspace->m_monitor.lock();
                if (workspaceMonitor == candidateMonitor)
                    monitorWorkspaces.push_back(workspace);
            }

            for (const auto& workspace : g_pCompositor->getWorkspacesCopy()) {
                if (!workspace || workspace->m_isSpecialWorkspace)
                    continue;

                const auto workspaceMonitor = workspace->m_monitor.lock();
                if (workspaceMonitor == candidateMonitor && !containsHandle(monitorWorkspaces, workspace))
                    monitorWorkspaces.push_back(workspace);
            }

            for (const auto& window : g_pCompositor->m_windows) {
                if (!window || !window->m_isMapped || window->m_fadingOut || !window->m_workspace || window->m_workspace->m_isSpecialWorkspace)
                    continue;

                const auto workspaceMonitor = window->m_workspace->m_monitor.lock();
                if (workspaceMonitor == candidateMonitor && !containsHandle(monitorWorkspaces, window->m_workspace))
                    monitorWorkspaces.push_back(window->m_workspace);
            }

            if (monitorWorkspaces.empty()) {
                if (state.ownerWorkspace && state.ownerWorkspace->m_monitor.lock() == candidateMonitor && !state.ownerWorkspace->m_isSpecialWorkspace)
                    monitorWorkspaces.push_back(state.ownerWorkspace);
                else if (candidateMonitor->m_activeWorkspace && !candidateMonitor->m_activeWorkspace->m_isSpecialWorkspace)
                    monitorWorkspaces.push_back(candidateMonitor->m_activeWorkspace);
            }

            std::stable_sort(monitorWorkspaces.begin(), monitorWorkspaces.end(), [](const PHLWORKSPACE& lhs, const PHLWORKSPACE& rhs) {
                if (!lhs || !rhs)
                    return static_cast<bool>(lhs);
                return lhs->m_id < rhs->m_id;
            });

            if (monitorWorkspaces.empty())
                continue;

            std::unordered_map<WORKSPACEID, PHLWORKSPACE> workspaceById;
            std::vector<int64_t> realWorkspaceIds;
            workspaceById.reserve(monitorWorkspaces.size());
            realWorkspaceIds.reserve(monitorWorkspaces.size());
            for (const auto& workspace : monitorWorkspaces) {
                if (!workspace)
                    continue;

                workspaceById.emplace(workspace->m_id, workspace);
                realWorkspaceIds.push_back(workspace->m_id);
            }

            const auto appendLaneWorkspaceId = [&](WORKSPACEID workspaceId) {
                if (workspaceId == WORKSPACE_INVALID)
                    return;

                const auto id = static_cast<int64_t>(workspaceId);
                if (std::find(realWorkspaceIds.begin(), realWorkspaceIds.end(), id) == realWorkspaceIds.end())
                    realWorkspaceIds.push_back(id);
            };

            for (const auto& override : workspaceOverrides) {
                if (override.monitorId != candidateMonitor->m_id || !override.syntheticEmpty || override.workspaceId == WORKSPACE_INVALID)
                    continue;

                appendLaneWorkspaceId(override.workspaceId);
            }

            const auto forceVisibleLaneId = [&](WORKSPACEID workspaceId) {
                if (workspaceId == WORKSPACE_INVALID)
                    return;

                appendLaneWorkspaceId(workspaceId);
            };

            for (const auto workspaceId : niri_scrolling_detail::retainedDirectNiriWorkspaceLaneIds(candidateMonitor->m_id))
                forceVisibleLaneId(workspaceId);

            if (preserveExistingOrder && isVisible()) {
                if (m_state.ownerWorkspace && m_state.ownerWorkspace->m_monitor.lock() == candidateMonitor)
                    forceVisibleLaneId(m_state.ownerWorkspace->m_id);

                for (const auto& entry : m_state.stripEntries) {
                    if (entry.monitor == candidateMonitor && entry.workspaceId != WORKSPACE_INVALID && !entry.newWorkspaceSlot)
                        forceVisibleLaneId(entry.workspaceId);
                }

                for (const auto& placeholder : m_state.emptyWorkspacePlaceholders) {
                    if (placeholder.monitor == candidateMonitor && placeholder.workspaceId != WORKSPACE_INVALID)
                        forceVisibleLaneId(placeholder.workspaceId);
                }
            }

            WORKSPACEID centerWorkspaceId = WORKSPACE_INVALID;
            if (const auto* override = overrideForMonitor(candidateMonitor); override && override->workspaceId != WORKSPACE_INVALID) {
                centerWorkspaceId = override->workspaceId;
            } else if (state.focusDuringOverview && state.focusDuringOverview->m_workspace &&
                       state.focusDuringOverview->m_workspace->m_monitor.lock() == candidateMonitor) {
                centerWorkspaceId = state.focusDuringOverview->m_workspace->m_id;
            } else if (state.ownerWorkspace && state.ownerWorkspace->m_monitor.lock() == candidateMonitor) {
                centerWorkspaceId = state.ownerWorkspace->m_id;
            } else if (candidateMonitor->m_activeWorkspace) {
                centerWorkspaceId = candidateMonitor->m_activeWorkspace->m_id;
            }

            if (centerWorkspaceId != WORKSPACE_INVALID)
                appendLaneWorkspaceId(centerWorkspaceId);

            std::vector<int64_t> occupiedWorkspaceIds;
            if (const auto occupiedIt = directNiriWorkspacesWithWindowsByMonitor.find(candidateMonitor->m_id);
                occupiedIt != directNiriWorkspacesWithWindowsByMonitor.end())
                occupiedWorkspaceIds.assign(occupiedIt->second.begin(), occupiedIt->second.end());

            const auto emptyMode =
                niriModeShowEmptyWorkspacesBetweenEnabled() ? WorkspaceStripEmptyMode::Continuous : WorkspaceStripEmptyMode::Existing;
            const auto laneWorkspaceIds = niriEmptyWorkspaceLaneIds(realWorkspaceIds, occupiedWorkspaceIds, emptyMode);
            auto& syntheticLaneWorkspaceIds = niriSyntheticLaneWorkspaceIdsByMonitor[candidateMonitor->m_id];
            for (const auto rawWorkspaceId : laneWorkspaceIds) {
                const auto workspaceId = static_cast<WORKSPACEID>(rawWorkspaceId);
                if (!workspaceById.contains(workspaceId))
                    syntheticLaneWorkspaceIds.push_back(workspaceId);
            }

            std::size_t activeIndex = 0;
            if (centerWorkspaceId != WORKSPACE_INVALID) {
                const auto it = std::find(laneWorkspaceIds.begin(), laneWorkspaceIds.end(), static_cast<int64_t>(centerWorkspaceId));
                if (it != laneWorkspaceIds.end())
                    activeIndex = static_cast<std::size_t>(std::distance(laneWorkspaceIds.begin(), it));
            }

            const auto placeholderRange = niriEmptyWorkspacePlaceholderRange(
                laneWorkspaceIds, occupiedWorkspaceIds,
                centerWorkspaceId == WORKSPACE_INVALID ? std::nullopt : std::optional<int64_t>{centerWorkspaceId});
            if (placeholderRange) {
                const auto [firstPlaceholderId, lastPlaceholderId] = *placeholderRange;
                placeholderRangeByMonitor[candidateMonitor->m_id] = {
                    static_cast<WORKSPACEID>(firstPlaceholderId),
                    static_cast<WORKSPACEID>(lastPlaceholderId),
                };
                auto& visibleWorkspaceIds = directNiriVisibleWorkspaceIdsByMonitor[candidateMonitor->m_id];
                for (const auto workspaceId : laneWorkspaceIds) {
                    if (workspaceId >= firstPlaceholderId && workspaceId <= lastPlaceholderId)
                        visibleWorkspaceIds.insert(static_cast<WORKSPACEID>(workspaceId));
                }
            }

            const Rect content = overviewContentRectForMonitor(candidateMonitor, state);
            const double visibleScale = niriMultiWorkspaceScale();
            const double laneHeight = std::max(1.0, content.height * visibleScale);
            const bool fitFocusMethod = getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 1;
            const double baseGap = niriWorkspaceGap();
            const double zoomedLaneHeight = laneHeight * niriActiveWorkspaceLayoutScale;
            double gap = baseGap;
            if (fitFocusMethod) {
                PHLWORKSPACE baseWorkspace;
                if (const auto workspaceIt = workspaceById.find(centerWorkspaceId); workspaceIt != workspaceById.end())
                    baseWorkspace = workspaceIt->second;
                if (!baseWorkspace && !monitorWorkspaces.empty())
                    baseWorkspace = monitorWorkspaces.front();

                if (baseWorkspace && baseWorkspace->m_space) {
                    const CBox workAreaBox = baseWorkspace->m_space->workArea();
                    const Rect baseGlobal = makeRect(workAreaBox.x, workAreaBox.y, workAreaBox.width, workAreaBox.height);
                    const Rect lanePreview = makeRect(content.x, content.y, content.width, laneHeight);
                    if (baseGlobal.width > 1.0 && baseGlobal.height > 1.0) {
                        const auto overflowAxis = axisForScrollingLayoutDirection(scrollingLayoutDirection());
                        double centerScale = niriOverviewPreviewScale(lanePreview, baseGlobal, config.maxPreviewScale, config.minSlotScale, overflowAxis);
                        const double viewportScale = lanePreview.width / std::max(1.0, baseGlobal.width * 4.0);
                        centerScale = std::max(config.minSlotScale, std::min({centerScale, visibleScale, viewportScale}));
                        centerScale *= niriActiveWorkspaceLayoutScale;

                        const double fitScale =
                            std::max(config.minSlotScale, std::min(lanePreview.width / baseGlobal.width, lanePreview.height / baseGlobal.height)) *
                            niriActiveWorkspaceLayoutScale;
                        const double centerViewportHeight = baseGlobal.height * centerScale;
                        const double fitViewportHeight = baseGlobal.height * fitScale;
                        gap = std::max(baseGap, baseGap + fitViewportHeight - centerViewportHeight);
                    }
                }
            }
            const double laneStep = std::max(1.0, zoomedLaneHeight + gap);
            const double centerY = content.centerY();
            for (std::size_t index = 0; index < laneWorkspaceIds.size(); ++index) {
                const auto workspaceId = static_cast<WORKSPACEID>(laneWorkspaceIds[index]);
                const double rowOffset = static_cast<double>(index) - static_cast<double>(activeIndex);
                niriWorkspaceLaneById[workspaceId] = makeRect(content.x, centerY + rowOffset * laneStep - laneHeight * 0.5, content.width, laneHeight);
            }
        }
    }

    const auto rowGroupForWindow = [&](const PHLWINDOW& window) -> std::size_t {
        if (!useWorkspaceRows)
            return 0;

        const auto workspace = window && window->m_workspace ? window->m_workspace : state.ownerWorkspace;
        const auto it = std::find(state.managedWorkspaces.begin(), state.managedWorkspaces.end(), workspace);
        if (it != state.managedWorkspaces.end())
            return static_cast<std::size_t>(std::distance(state.managedWorkspaces.begin(), it));

        if (state.ownerWorkspace) {
            const auto ownerIt = std::find(state.managedWorkspaces.begin(), state.managedWorkspaces.end(), state.ownerWorkspace);
            if (ownerIt != state.managedWorkspaces.end())
                return static_cast<std::size_t>(std::distance(state.managedWorkspaces.begin(), ownerIt));
        }

        return state.managedWorkspaces.size();
    };
    std::unordered_map<std::size_t, Rect> niriWorkspaceViewportGlobalByWindowIndex;

    const auto closestScrollingAnchorForFloatingWindow = [&](const PHLWINDOW& window, const PHLWORKSPACE& layoutWorkspace, const Rect& floatingSourceGlobal) -> std::optional<Rect> {
        if (!window || !layoutWorkspace || !isScrollingWorkspace(layoutWorkspace))
            return std::nullopt;

        const auto overlapArea = [](const Rect& lhs, const Rect& rhs) {
            const double x1 = std::max(lhs.x, rhs.x);
            const double y1 = std::max(lhs.y, rhs.y);
            const double x2 = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
            const double y2 = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
            if (x2 <= x1 || y2 <= y1)
                return 0.0;
            return (x2 - x1) * (y2 - y1);
        };

        struct AnchorCandidate {
            Rect   rect;
            bool   tiled = false;
            double overlap = 0.0;
            double distance = std::numeric_limits<double>::infinity();
        };

        std::optional<AnchorCandidate> bestTiled;

        const auto betterAnchor = [](const AnchorCandidate& candidate, const std::optional<AnchorCandidate>& current) {
            if (!current)
                return true;

            // Resize is the important case here.  A floating window can keep the
            // same center while its edges move across a different column/window.
            // Center-distance alone then keeps picking the old anchor, so the
            // overview card only changes size in-place.  Prefer the window/column
            // with the largest live overlap; use distance only as the fallback.
            if (candidate.overlap > current->overlap + 1.0)
                return true;
            if (candidate.overlap + 1.0 < current->overlap)
                return false;

            return candidate.distance < current->distance;
        };

        for (const auto& candidate : candidates) {
            if (!candidate || candidate == window || !candidate->m_isMapped || candidate->m_fadingOut || candidate->isHidden() || candidate->m_pinned ||
                candidate->m_workspace != layoutWorkspace)
                continue;

            const bool candidateUseGoalGeometry = shouldUseGoalGeometryForStateSnapshot(candidate);
            const Rect candidateNaturalGlobal = stateSnapshotGlobalRectForWindow(candidate, candidateUseGoalGeometry);
            Rect       candidateAnchorGlobal = candidateNaturalGlobal;

            const auto target = candidate->layoutTarget();
            const bool candidateIsTiledScrolling = target && !target->floating() && !isFloatingOverviewWindow(candidate);
            if (candidateIsTiledScrolling) {
                candidateAnchorGlobal = scrollingOverviewSourceGlobalRectForWindow(candidate, candidateNaturalGlobal);
                if (const auto rowGeometry = scrollingOverviewTapeRowGeometryForWindow(candidate, candidateAnchorGlobal))
                    candidateAnchorGlobal = rowGeometry->anchorGlobal;
            } else {
                candidateAnchorGlobal = floatingOverviewSourceGlobalRectForWindow(candidate, renderGlobalRectForWindow(candidate, candidateUseGoalGeometry));
            }

            const double dx = candidateAnchorGlobal.centerX() - floatingSourceGlobal.centerX();
            const double dy = candidateAnchorGlobal.centerY() - floatingSourceGlobal.centerY();
            AnchorCandidate anchor{
                .rect = candidateAnchorGlobal,
                .tiled = candidateIsTiledScrolling,
                .overlap = overlapArea(floatingSourceGlobal, candidateAnchorGlobal),
                .distance = dx * dx + dy * dy,
            };

            if (candidateIsTiledScrolling && betterAnchor(anchor, bestTiled))
                bestTiled = anchor;
        }

        if (bestTiled)
            return bestTiled->rect;

        // Floating windows are not part of the scrolling tape.  When a
        // workspace has no tiled targets, using another floating window as the
        // anchor makes the workspace viewport and wallpaper follow that floating
        // window's off-center position.  In that case, leave the anchor unset so
        // the caller can center the 1.0 workspace viewport and place the floating
        // window inside it with its real offset.
        return std::nullopt;
    };

    const auto niriOverviewSlotForSource = [&](const PHLWINDOW& window, const PHLMONITOR& targetMonitor, const Rect& sourceGlobal, const Rect& baseGlobal,
                                               std::size_t windowIndex, bool allowPinned,
                                               std::optional<GestureAxis> overflowAxis,
                                               std::optional<Rect> anchorOverride) -> std::optional<WindowSlot> {
        if (!allowDirectNiriOverviewLayout)
            return std::nullopt;

        if (!niriModeEnabled() || !window || !targetMonitor || (window->m_pinned && !allowPinned))
            return std::nullopt;

        const auto layoutWorkspace = window->m_pinned ? focusedStripWorkspaceForMonitor(targetMonitor) : window->m_workspace;
        if (!layoutWorkspace || !layoutWorkspace->m_space || !isScrollingWorkspace(layoutWorkspace))
            return std::nullopt;

        auto* const scrolling = scrollingAlgorithmForWorkspace(layoutWorkspace);
        if (!scrolling || !scrolling->m_scrollingData)
            return std::nullopt;

        Rect previewArea = overviewContentRectForMonitor(targetMonitor, state);
        if (const auto laneIt = niriWorkspaceLaneById.find(layoutWorkspace->m_id); laneIt != niriWorkspaceLaneById.end())
            previewArea = laneIt->second;
        if (previewArea.width <= 1.0 || previewArea.height <= 1.0)
            return std::nullopt;

        if (baseGlobal.width <= 1.0 || baseGlobal.height <= 1.0)
            return std::nullopt;

        const bool fitModeViewport = !g_niriStripSnapshotSingleWorkspaceOnly && overflowAxis && getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 1;
        double fitModeViewportScale = 0.0;
        double scale = niriOverviewPreviewScale(previewArea, baseGlobal, config.maxPreviewScale, config.minSlotScale, overflowAxis);
        if (overflowAxis) {
            if (g_niriStripSnapshotSingleWorkspaceOnly) {
                const double stripZoom = std::clamp(getConfigFloat(m_handle, "plugin:hymission:niri_strip_workspace_zoom", 2.0), 0.05, 4.0);
                const double visibleViewportCount = std::max(0.05, 0.92 / stripZoom);
                const double previewLength = *overflowAxis == GestureAxis::Vertical ? previewArea.height : previewArea.width;
                const double baseLength = *overflowAxis == GestureAxis::Vertical ? baseGlobal.height : baseGlobal.width;
                const double viewportScale = previewLength / std::max(1.0, baseLength * visibleViewportCount);
                scale = std::max(config.minSlotScale, std::min(scale * stripZoom, viewportScale));
            } else if (fitModeViewport) {
                const double fitScale = std::min(previewArea.width / baseGlobal.width, previewArea.height / baseGlobal.height);
                fitModeViewportScale = fitScale;
                scale = std::max(config.minSlotScale, fitScale);
            } else {
                const double maxNiriScale = niriMultiWorkspaceScale();
                const double visibleViewportCount = 4.0;
                const double viewportScale = previewArea.width / std::max(1.0, baseGlobal.width * visibleViewportCount);
                scale = std::max(config.minSlotScale, std::min({scale, maxNiriScale, viewportScale}));
            }
        } else {
            const double maxNiriScale = g_niriStripSnapshotSingleWorkspaceOnly ?
                std::clamp(getConfigFloat(m_handle, "plugin:hymission:niri_strip_workspace_scale", 1.30), 0.05, 2.0) :
                niriMultiWorkspaceScale();
            scale = std::max(config.minSlotScale, std::min(scale, maxNiriScale));
        }
        if (scale <= 0.0)
            return std::nullopt;
        if (std::abs(niriActiveWorkspaceLayoutScale - 1.0) > 0.001)
            scale *= niriActiveWorkspaceLayoutScale;

        Rect anchorSourceGlobal = anchorOverride.value_or(sourceGlobal);
        if (g_niriStripSnapshotSingleWorkspaceOnly) {
            if (!anchorOverride) {
                const Rect centeredBaseAnchor =
                    makeRect(baseGlobal.centerX() - anchorSourceGlobal.width * 0.5, baseGlobal.centerY() - anchorSourceGlobal.height * 0.5,
                             anchorSourceGlobal.width, anchorSourceGlobal.height);
                anchorSourceGlobal = centerAnchorOnWorkspaceStripAxis(centeredBaseAnchor, centeredBaseAnchor, parseWorkspaceStripAnchor(workspaceStripAnchor()));
            }
        } else {
            PHLWINDOW anchorWindow;
            if (!anchorOverride && layoutWorkspace) {
                if (state.focusDuringOverview && !state.focusDuringOverview->m_pinned && state.focusDuringOverview->m_workspace == layoutWorkspace)
                    anchorWindow = state.focusDuringOverview;
                else
                    anchorWindow = focusCandidateForWorkspace(layoutWorkspace);

                if (anchorWindow && anchorWindow->m_workspace == layoutWorkspace) {
                    const bool anchorUseGoalGeometry = shouldUseGoalGeometryForStateSnapshot(anchorWindow);
                    const Rect anchorNaturalGlobal = stateSnapshotGlobalRectForWindow(anchorWindow, anchorUseGoalGeometry);
                    anchorSourceGlobal = scrollingOverviewSourceGlobalRectForWindow(anchorWindow, anchorNaturalGlobal);
                    if (const auto anchorRowGeometry = scrollingOverviewTapeRowGeometryForWindow(anchorWindow, anchorSourceGlobal, anchorWindow))
                        anchorSourceGlobal = anchorRowGeometry->anchorGlobal;
                }
            }
        }

        const double viewportX = previewArea.centerX() - (anchorSourceGlobal.centerX() - baseGlobal.x) * scale;
        const double viewportY = previewArea.centerY() - (anchorSourceGlobal.centerY() - baseGlobal.y) * scale;
        const double targetWidth = sourceGlobal.width * scale;
        const double targetHeight = sourceGlobal.height * scale;
        const double targetCenterX = viewportX + (sourceGlobal.centerX() - baseGlobal.x) * scale;
        const double targetCenterY = viewportY + (sourceGlobal.centerY() - baseGlobal.y) * scale;
        Rect targetLocal = makeRect(targetCenterX - targetWidth * 0.5, targetCenterY - targetHeight * 0.5, targetWidth, targetHeight);
        Rect workspaceViewportLocal = makeRect(viewportX, viewportY, baseGlobal.width * scale, baseGlobal.height * scale);
        if (fitModeViewport) {
            const double viewportScale = fitModeViewportScale > 0.0 ? fitModeViewportScale * niriActiveWorkspaceLayoutScale : scale;
            Rect viewportLocal = makeRect(previewArea.centerX() - baseGlobal.width * viewportScale * 0.5,
                                          previewArea.centerY() - baseGlobal.height * viewportScale * 0.5,
                                          baseGlobal.width * viewportScale,
                                          baseGlobal.height * viewportScale);
            workspaceViewportLocal = viewportLocal;
            targetLocal = makeRect(viewportLocal.centerX() + (sourceGlobal.centerX() - baseGlobal.centerX()) * scale - targetWidth * 0.5,
                                   viewportLocal.centerY() + (sourceGlobal.centerY() - baseGlobal.centerY()) * scale - targetHeight * 0.5,
                                   targetWidth,
                                   targetHeight);

        }
        // Pinned windows are monitor-global overlays. They must not claim the
        // per-workspace wallpaper/backing placeholder, because their slot path
        // does not carry the scrolling tape overflow axis. If a pinned window is
        // processed before a real tiled scrolling window, claiming this here
        // makes the wallpaper use the pinned/floating overlay scale and appear
        // smaller than the rest of the workspace viewports. Let tiled scrolling
        // windows create the backing placeholder; if the workspace has only
        // pinned overlays, the empty-workspace placeholder pass below creates a
        // stable workspace-sized viewport.
        if (!window->m_pinned && niriFitBackingPlaceholderWorkspaces.insert(layoutWorkspace->m_id).second) {
            const Rect targetGlobal = makeRect(targetMonitor->m_position.x + workspaceViewportLocal.x,
                                               targetMonitor->m_position.y + workspaceViewportLocal.y,
                                               workspaceViewportLocal.width,
                                               workspaceViewportLocal.height);
            state.emptyWorkspacePlaceholders.push_back({
                .monitor = targetMonitor,
                .workspace = layoutWorkspace,
                .workspaceId = layoutWorkspace->m_id,
                .naturalGlobal = baseGlobal,
                .exitGlobal = baseGlobal,
                .targetGlobal = targetGlobal,
                .relayoutFromGlobal = targetGlobal,
                .backingOnly = true,
            });
        }
        const double stripPreviewGapBoost = g_niriStripSnapshotSingleWorkspaceOnly ? 2.0 : 0.0;
        if (overflowAxis) {
            const double previewGap = niriWindowGapsForWorkspace(layoutWorkspace, *overflowAxis) + stripPreviewGapBoost;
            if (*overflowAxis == GestureAxis::Horizontal) {
                const double width = std::max(1.0, targetLocal.width - previewGap);
                targetLocal = makeRect(targetLocal.centerX() - width * 0.5, targetLocal.y, width, targetLocal.height);
            } else {
                const double height = std::max(1.0, targetLocal.height - previewGap);
                targetLocal = makeRect(targetLocal.x, targetLocal.centerY() - height * 0.5, targetLocal.width, height);
            }
        }
        if (stripPreviewGapBoost > 0.0) {
            const double width = std::max(1.0, targetLocal.width - stripPreviewGapBoost);
            const double height = std::max(1.0, targetLocal.height - stripPreviewGapBoost);
            targetLocal = makeRect(targetLocal.centerX() - width * 0.5, targetLocal.centerY() - height * 0.5, width, height);
        }

        if (window->m_pinned) {
            // Pinned windows are monitor-global overlays, not scrolling-tape
            // members. Map them into the same stable workspace viewport used by
            // empty/tiled scrolling lanes, instead of using the floating overlay
            // slot scale. This keeps the wallpaper viewport the same size as the
            // neighboring workspaces and prevents flicker when the centered lane
            // changes away from the pinned window's stored/original workspace.
            const Rect stableViewportLocal = emptyOverviewPlaceholderLocalRect(targetMonitor, layoutWorkspace, previewArea, state);
            if (stableViewportLocal.width > 1.0 && stableViewportLocal.height > 1.0) {
                const Rect stableViewportGlobal = makeRect(targetMonitor->m_position.x + stableViewportLocal.x,
                                                           targetMonitor->m_position.y + stableViewportLocal.y,
                                                           stableViewportLocal.width,
                                                           stableViewportLocal.height);
                const Rect mappedPinnedGlobal = transformLiveOverviewRect(sourceGlobal, baseGlobal, stableViewportGlobal);
                if (mappedPinnedGlobal.width > 1.0 && mappedPinnedGlobal.height > 1.0) {
                    workspaceViewportLocal = stableViewportLocal;
                    targetLocal = makeRect(mappedPinnedGlobal.x - targetMonitor->m_position.x,
                                           mappedPinnedGlobal.y - targetMonitor->m_position.y,
                                           mappedPinnedGlobal.width,
                                           mappedPinnedGlobal.height);
                }
            }
        }

        if (isFloatingOverviewWindow(window) || window->m_pinned) {
            // Floating and pinned windows can live outside the scrolling tape.
            // Keep their overview cards inside the mapped 1.0 workspace viewport
            // so they do not spill into neighboring Niri overview lanes.
            targetLocal = clampRectInsidePreservingAspect(targetLocal, workspaceViewportLocal, scale);
            niriWorkspaceViewportGlobalByWindowIndex[windowIndex] = makeRect(targetMonitor->m_position.x + workspaceViewportLocal.x,
                                                                            targetMonitor->m_position.y + workspaceViewportLocal.y,
                                                                            workspaceViewportLocal.width,
                                                                            workspaceViewportLocal.height);
        }

        return WindowSlot{
            .index = windowIndex,
            .natural =
                {
                    sourceGlobal.x - targetMonitor->m_position.x,
                    sourceGlobal.y - targetMonitor->m_position.y,
                    sourceGlobal.width,
                    sourceGlobal.height,
                },
            .target = targetLocal,
            .scale = scale,
        };
    };
    const auto niriFloatingOverviewSlotForWindow = [&](const PHLWINDOW& window, const PHLMONITOR& targetMonitor, const Rect& sourceGlobal,
                                                       std::size_t windowIndex, Rect& resolvedSourceGlobal) -> std::optional<WindowSlot> {
        if (!isFloatingOverviewWindow(window) && !(window && window->m_pinned))
            return std::nullopt;

        resolvedSourceGlobal = sourceGlobal;

        Rect baseGlobal = niriFloatingOverviewBaseGlobalRect(targetMonitor);
        const auto layoutWorkspace = window && window->m_pinned ? focusedStripWorkspaceForMonitor(targetMonitor) : (window ? window->m_workspace : PHLWORKSPACE{});
        if (layoutWorkspace && layoutWorkspace->m_space) {
            const CBox workAreaBox = layoutWorkspace->m_space->workArea();
            const Rect workArea = makeRect(workAreaBox.x, workAreaBox.y, workAreaBox.width, workAreaBox.height);
            if (workArea.width > 1.0 && workArea.height > 1.0)
                baseGlobal = workArea;
        }

        std::optional<Rect> anchorOverride;
        if (window && window->m_pinned) {
            if (baseGlobal.width > 1.0 && baseGlobal.height > 1.0 && sourceGlobal.width > 1.0 && sourceGlobal.height > 1.0) {
                // Pinned windows are monitor-global overlays, not scrolling-tape
                // members. Never let a pinned window's compositor position, or
                // its stale stored workspace, become the wallpaper/viewport
                // anchor. Keep the 1.0 workspace viewport centered, then place
                // the pinned card over that viewport below.
                anchorOverride = makeRect(baseGlobal.centerX() - sourceGlobal.width * 0.5,
                                          baseGlobal.centerY() - sourceGlobal.height * 0.5,
                                          sourceGlobal.width,
                                          sourceGlobal.height);
            }
        } else if (const auto anchor = closestScrollingAnchorForFloatingWindow(window, layoutWorkspace, sourceGlobal)) {
            anchorOverride = *anchor;
            resolvedSourceGlobal = makeRect(anchor->centerX() - sourceGlobal.width * 0.5,
                                            anchor->centerY() - sourceGlobal.height * 0.5,
                                            sourceGlobal.width,
                                            sourceGlobal.height);
        } else if (baseGlobal.width > 1.0 && baseGlobal.height > 1.0 && sourceGlobal.width > 1.0 && sourceGlobal.height > 1.0) {
            // Floating-only scrolling workspaces still need a stable workspace
            // viewport.  Center the viewport on the workspace itself; the
            // floating window target is then derived from its real position
            // relative to that centered viewport.
            anchorOverride = makeRect(baseGlobal.centerX() - sourceGlobal.width * 0.5,
                                      baseGlobal.centerY() - sourceGlobal.height * 0.5,
                                      sourceGlobal.width,
                                      sourceGlobal.height);
        }

        return niriOverviewSlotForSource(window, targetMonitor, resolvedSourceGlobal, baseGlobal, windowIndex, true, std::nullopt, anchorOverride);
    };
    const auto niriScrollingOverviewSlotForWindow = [&](const PHLWINDOW& window, const PHLMONITOR& targetMonitor, const Rect& sourceGlobal,
                                                        std::size_t windowIndex, Rect& resolvedSourceGlobal) -> std::optional<WindowSlot> {
        const auto target = window ? window->layoutTarget() : nullptr;
        if (!target || target->floating())
            return std::nullopt;

        Rect sourceForOverview = sourceGlobal;
        Rect baseGlobal;
        std::optional<Rect> anchorOverride;
        std::optional<GestureAxis> overflowAxis;
        PHLWINDOW layoutAnchorWindow;
        if (state.focusDuringOverview && !state.focusDuringOverview->m_pinned && state.focusDuringOverview->m_workspace == window->m_workspace)
            layoutAnchorWindow = state.focusDuringOverview;
        else
            layoutAnchorWindow = focusCandidateForWorkspace(window->m_workspace);

        if (const auto rowGeometry = scrollingOverviewTapeRowGeometryForWindow(window, sourceGlobal, layoutAnchorWindow)) {
            sourceForOverview = rowGeometry->sourceGlobal;
            baseGlobal = rowGeometry->baseGlobal;
            if (g_niriStripSnapshotSingleWorkspaceOnly)
                anchorOverride = rowGeometry->anchorGlobal;
            overflowAxis = rowGeometry->primaryAxis;
        } else {
            const CBox workAreaBox = window && window->m_workspace && window->m_workspace->m_space ? window->m_workspace->m_space->workArea() : CBox{};
            baseGlobal = makeRect(workAreaBox.x, workAreaBox.y, workAreaBox.width, workAreaBox.height);
        }

        auto slot = niriOverviewSlotForSource(window, targetMonitor, sourceForOverview, baseGlobal, windowIndex, false, overflowAxis, anchorOverride);
        if (slot)
            resolvedSourceGlobal = sourceForOverview;
        return slot;
    };

    if (refreshLayoutSnapshots) {
        for (const auto& workspace : state.managedWorkspaces)
            refreshWorkspaceLayoutSnapshot(workspace);
    }

    for (const auto& window : candidates) {
        if (!shouldManageWindow(window, state))
            continue;

        const auto targetMonitor = preferredMonitorForWindow(window, state);
        if (!targetMonitor)
            continue;

        if (allowDirectNiriOverviewLayout && !window->m_pinned && window->m_workspace && isScrollingWorkspace(window->m_workspace)) {
            const auto visibleIt = directNiriVisibleWorkspaceIdsByMonitor.find(targetMonitor->m_id);
            if (visibleIt != directNiriVisibleWorkspaceIdsByMonitor.end() && !visibleIt->second.contains(window->m_workspace->m_id))
                continue;
        }

        const bool useGoalGeometry = shouldUseGoalGeometryForStateSnapshot(window);
        const Rect naturalGlobal = stateSnapshotGlobalRectForWindow(window, useGoalGeometry);
        const Rect layoutGlobal = layoutAnchorGlobalRectForWindow(window, useGoalGeometry);
        const std::size_t windowIndex = state.windows.size();
        Rect directNiriSourceGlobal = naturalGlobal;
        std::optional<WindowSlot> directNiriSlot;
        bool directNiriFloatingOverlay = false;

        const Rect floatingSourceGlobal = floatingOverviewSourceGlobalRectForWindow(window, renderGlobalRectForWindow(window, useGoalGeometry));
        Rect resolvedFloatingSourceGlobal = floatingSourceGlobal;
        directNiriSlot = niriFloatingOverviewSlotForWindow(window, targetMonitor, floatingSourceGlobal, windowIndex, resolvedFloatingSourceGlobal);
        if (directNiriSlot) {
            directNiriSourceGlobal = resolvedFloatingSourceGlobal;
            directNiriFloatingOverlay = true;
        } else {
            const Rect scrollingSourceGlobal = scrollingOverviewSourceGlobalRectForWindow(window, naturalGlobal);
            Rect       resolvedScrollingSourceGlobal = scrollingSourceGlobal;
            directNiriSlot = niriScrollingOverviewSlotForWindow(window, targetMonitor, scrollingSourceGlobal, windowIndex, resolvedScrollingSourceGlobal);
            if (directNiriSlot)
                directNiriSourceGlobal = resolvedScrollingSourceGlobal;
        }

        state.windows.push_back({
            .window = window,
            .targetMonitor = targetMonitor,
            .title = window->m_title,
            .naturalGlobal = directNiriSlot ? directNiriSourceGlobal : naturalGlobal,
            .exitGlobal = directNiriSlot ? directNiriSourceGlobal : naturalGlobal,
            .previewAlpha = std::clamp(window->alphaTotal(), 0.0F, 1.0F),
            .isFloating = window->m_isFloating,
            .isPinned = window->m_pinned,
            .isNiriFloatingOverlay = directNiriFloatingOverlay,
        });

        if (directNiriSlot) {
            auto& managed = state.windows.back();
            managed.slot = *directNiriSlot;
            managed.targetGlobal = makeRect(targetMonitor->m_position.x + directNiriSlot->target.x,
                                            targetMonitor->m_position.y + directNiriSlot->target.y, directNiriSlot->target.width,
                                            directNiriSlot->target.height);
            managed.relayoutFromGlobal = managed.targetGlobal;
            state.slots.push_back(*directNiriSlot);
            ++directNiriOverviewWindowsByMonitor[targetMonitor->m_id];
            if (debugLogsEnabled() && directNiriOverviewWindowsByMonitor[targetMonitor->m_id] <= 8) {
                std::ostringstream out;
                out << "[hymission] niri " << (directNiriFloatingOverlay ? "floating overview overlay" : "scrolling overview direct")
                    << " window=" << debugWindowLabel(window)
                    << " floating=" << (window->m_isFloating ? 1 : 0)
                    << " source=" << rectToString(directNiriSourceGlobal)
                    << " target=" << rectToString(managed.targetGlobal)
                    << " scale=" << directNiriSlot->scale;
                debugLog(out.str());
            }
            continue;
        }

        inputsByMonitor[targetMonitor->m_id].push_back({
            .index = windowIndex,
            .natural =
                {
                    layoutGlobal.x - targetMonitor->m_position.x,
                    layoutGlobal.y - targetMonitor->m_position.y,
                    layoutGlobal.width,
                    layoutGlobal.height,
                },
            .label = window->m_title,
            .rowGroup = rowGroupForWindow(window),
            .layoutEmphasis = 1.0,
        });
    }

    std::vector<PHLMONITOR> activeParticipatingMonitors;
    MissionControlLayout engine;
    for (const auto& candidateMonitor : state.participatingMonitors) {
        if (!candidateMonitor)
            continue;

        const auto inputsIt = inputsByMonitor.find(candidateMonitor->m_id);
        const auto directIt = directNiriOverviewWindowsByMonitor.find(candidateMonitor->m_id);
        const bool hasDirectNiriOverviewWindows = directIt != directNiriOverviewWindowsByMonitor.end() && directIt->second > 0;
        const bool hasStripWorkspace = workspaceStripEnabled(state) && static_cast<bool>(candidateMonitor->m_activeWorkspace);
        const bool keepOwnerMonitorForEmptyOverview = candidateMonitor == state.ownerMonitor;
        const bool keepMonitor = keepEmptyParticipatingMonitors && overrideForMonitor(candidateMonitor);
        if ((inputsIt == inputsByMonitor.end() || inputsIt->second.empty()) && !hasDirectNiriOverviewWindows && !keepMonitor && !hasStripWorkspace &&
            !keepOwnerMonitorForEmptyOverview)
            continue;

        activeParticipatingMonitors.push_back(candidateMonitor);
        if (inputsIt == inputsByMonitor.end() || inputsIt->second.empty())
            continue;

        const Rect previewArea = overviewContentRectForMonitor(candidateMonitor, state);
        const auto slots =
            engine.compute(inputsIt->second,
                           previewArea,
                           config);
        const auto scaleSlotTargetAroundPreviewCenter = [&](const Rect& target) {
            const double width = target.width * niriActiveWorkspaceLayoutScale;
            const double height = target.height * niriActiveWorkspaceLayoutScale;
            const double centerX = previewArea.centerX() + (target.centerX() - previewArea.centerX()) * niriActiveWorkspaceLayoutScale;
            const double centerY = previewArea.centerY() + (target.centerY() - previewArea.centerY()) * niriActiveWorkspaceLayoutScale;
            return makeRect(centerX - width * 0.5, centerY - height * 0.5, width, height);
        };
        for (auto slot : slots) {
            if (slot.index >= state.windows.size())
                continue;

            if (std::abs(niriActiveWorkspaceLayoutScale - 1.0) > 0.001) {
                slot.target = scaleSlotTargetAroundPreviewCenter(slot.target);
                slot.scale *= niriActiveWorkspaceLayoutScale;
            }

            auto& managed = state.windows[slot.index];
            if (managed.targetMonitor != candidateMonitor)
                continue;

            managed.slot = slot;
            managed.targetGlobal =
                makeRect(candidateMonitor->m_position.x + slot.target.x, candidateMonitor->m_position.y + slot.target.y, slot.target.width, slot.target.height);
            managed.relayoutFromGlobal = managed.targetGlobal;
            state.slots.push_back(slot);
        }
    }

    if (allowDirectNiriOverviewLayout) {
        const auto placeholderSourceGlobalForWorkspace = [&](const PHLMONITOR& targetMonitor, const PHLWORKSPACE& workspace) {
            if (workspace && workspace->m_space && isScrollingWorkspace(workspace)) {
                const CBox workAreaBox = workspace->m_space->workArea();
                const Rect workArea = makeRect(workAreaBox.x, workAreaBox.y, workAreaBox.width, workAreaBox.height);
                if (workArea.width > 1.0 && workArea.height > 1.0)
                    return workArea;
            }

            return makeRect(targetMonitor->m_position.x, targetMonitor->m_position.y, targetMonitor->m_size.x, targetMonitor->m_size.y);
        };

        const auto placeholderProxyWorkspaceForMonitor = [&](const PHLMONITOR& targetMonitor) -> PHLWORKSPACE {
            if (state.focusDuringOverview && !state.focusDuringOverview->m_pinned && state.focusDuringOverview->m_workspace &&
                state.focusDuringOverview->m_workspace->m_monitor.lock() == targetMonitor && isScrollingWorkspace(state.focusDuringOverview->m_workspace))
                return state.focusDuringOverview->m_workspace;

            if (state.ownerWorkspace && state.ownerWorkspace->m_monitor.lock() == targetMonitor && isScrollingWorkspace(state.ownerWorkspace))
                return state.ownerWorkspace;

            if (targetMonitor && targetMonitor->m_activeWorkspace && isScrollingWorkspace(targetMonitor->m_activeWorkspace))
                return targetMonitor->m_activeWorkspace;

            return state.ownerWorkspace;
        };

        for (const auto& workspace : state.managedWorkspaces) {
            if (!workspace || workspace->m_isSpecialWorkspace)
                continue;

            const auto targetMonitor = workspace->m_monitor.lock();
            if (!targetMonitor)
                continue;

            if (const auto rangeIt = placeholderRangeByMonitor.find(targetMonitor->m_id); rangeIt != placeholderRangeByMonitor.end()) {
                const auto [minPlaceholderId, maxPlaceholderId] = rangeIt->second;
                if (workspace->m_id < minPlaceholderId || workspace->m_id > maxPlaceholderId)
                    continue;
            }

            const bool hasManagedWindow = std::ranges::any_of(state.windows, [&](const ManagedWindow& managed) {
                if (!managed.window || managed.targetMonitor != targetMonitor)
                    return false;

                // Pinned windows are overlays on the monitor, not occupants of
                // the focused scrolling workspace lane. Counting them here
                // suppresses the stable empty/backing viewport for pinned-only
                // workspaces, which then leaves the pinned card's smaller
                // overlay-derived viewport as the only wallpaper rect.
                if (managed.window->m_pinned || managed.isPinned)
                    return false;

                return managed.window->m_workspace == workspace;
            });
            if (hasManagedWindow)
                continue;

            Rect content = overviewContentRectForMonitor(targetMonitor, state);
            if (const auto laneIt = niriWorkspaceLaneById.find(workspace->m_id); laneIt != niriWorkspaceLaneById.end())
                content = laneIt->second;

            const Rect targetLocal = emptyOverviewPlaceholderLocalRect(targetMonitor, workspace, content, state);
            if (targetLocal.width <= 0.0 || targetLocal.height <= 0.0)
                continue;

            const Rect targetGlobal = makeRect(targetMonitor->m_position.x + targetLocal.x, targetMonitor->m_position.y + targetLocal.y, targetLocal.width,
                                               targetLocal.height);
            const Rect sourceGlobal = placeholderSourceGlobalForWorkspace(targetMonitor, workspace);
            state.emptyWorkspacePlaceholders.push_back({
                .monitor = targetMonitor,
                .workspace = workspace,
                .workspaceId = workspace->m_id,
                .naturalGlobal = sourceGlobal,
                .exitGlobal = sourceGlobal,
                .targetGlobal = targetGlobal,
                .relayoutFromGlobal = targetGlobal,
            });
        }

        for (const auto& [monitorId, workspaceIds] : niriSyntheticLaneWorkspaceIdsByMonitor) {
            const auto targetMonitorIt = std::find_if(state.participatingMonitors.begin(), state.participatingMonitors.end(),
                                                      [&](const PHLMONITOR& candidate) { return candidate && candidate->m_id == monitorId; });
            if (targetMonitorIt == state.participatingMonitors.end() || !*targetMonitorIt)
                continue;

            const auto targetMonitor = *targetMonitorIt;
            for (const auto workspaceId : workspaceIds) {
                if (const auto rangeIt = placeholderRangeByMonitor.find(targetMonitor->m_id); rangeIt != placeholderRangeByMonitor.end()) {
                    const auto [minPlaceholderId, maxPlaceholderId] = rangeIt->second;
                    if (workspaceId < minPlaceholderId || workspaceId > maxPlaceholderId)
                        continue;
                }

                const bool alreadyHasPlaceholder = std::ranges::any_of(state.emptyWorkspacePlaceholders, [&](const EmptyWorkspacePlaceholder& placeholder) {
                    return placeholder.monitor == targetMonitor && placeholder.workspaceId == workspaceId;
                });
                if (alreadyHasPlaceholder)
                    continue;

                Rect content = overviewContentRectForMonitor(targetMonitor, state);
                if (const auto laneIt = niriWorkspaceLaneById.find(workspaceId); laneIt != niriWorkspaceLaneById.end())
                    content = laneIt->second;

                const auto proxyWorkspace = placeholderProxyWorkspaceForMonitor(targetMonitor);
                const Rect targetLocal = emptyOverviewPlaceholderLocalRect(targetMonitor, proxyWorkspace, content, state);
                if (targetLocal.width <= 0.0 || targetLocal.height <= 0.0)
                    continue;

                const Rect targetGlobal = makeRect(targetMonitor->m_position.x + targetLocal.x, targetMonitor->m_position.y + targetLocal.y, targetLocal.width,
                                                   targetLocal.height);
                const Rect sourceGlobal = placeholderSourceGlobalForWorkspace(targetMonitor, proxyWorkspace);
                state.emptyWorkspacePlaceholders.push_back({
                    .monitor = targetMonitor,
                    .workspace = {},
                    .workspaceId = workspaceId,
                    .naturalGlobal = sourceGlobal,
                    .exitGlobal = sourceGlobal,
                    .targetGlobal = targetGlobal,
                    .relayoutFromGlobal = targetGlobal,
                });
            }
        }

        for (const auto& override : workspaceOverrides) {
            if (!override.syntheticEmpty || override.workspace || override.workspaceId == WORKSPACE_INVALID)
                continue;

            const auto targetMonitorIt = std::find_if(state.participatingMonitors.begin(), state.participatingMonitors.end(),
                                                      [&](const PHLMONITOR& candidate) { return candidate && candidate->m_id == override.monitorId; });
            if (targetMonitorIt == state.participatingMonitors.end() || !*targetMonitorIt)
                continue;

            const auto targetMonitor = *targetMonitorIt;
            const bool alreadyHasPlaceholder = std::ranges::any_of(state.emptyWorkspacePlaceholders, [&](const EmptyWorkspacePlaceholder& placeholder) {
                return placeholder.monitor == targetMonitor && placeholder.workspaceId == override.workspaceId;
            });
            if (alreadyHasPlaceholder)
                continue;

            Rect content = overviewContentRectForMonitor(targetMonitor, state);
            const auto proxyWorkspace = placeholderProxyWorkspaceForMonitor(targetMonitor);
            const Rect targetLocal = emptyOverviewPlaceholderLocalRect(targetMonitor, proxyWorkspace, content, state);
            if (targetLocal.width <= 0.0 || targetLocal.height <= 0.0)
                continue;

            const Rect targetGlobal = makeRect(targetMonitor->m_position.x + targetLocal.x, targetMonitor->m_position.y + targetLocal.y, targetLocal.width,
                                               targetLocal.height);
            const Rect sourceGlobal = placeholderSourceGlobalForWorkspace(targetMonitor, proxyWorkspace);
            state.emptyWorkspacePlaceholders.push_back({
                .monitor = targetMonitor,
                .workspace = {},
                .workspaceId = override.workspaceId,
                .naturalGlobal = sourceGlobal,
                .exitGlobal = sourceGlobal,
                .targetGlobal = targetGlobal,
                .relayoutFromGlobal = targetGlobal,
            });
        }
    }

    const auto settleNiriFloatingOverlayOverlaps = [&]() {
        const double gap = std::max(2.0, std::min(12.0, std::min(config.columnSpacing, config.rowSpacing) * 0.25));
        for (const auto& candidateMonitor : activeParticipatingMonitors) {
            if (!candidateMonitor)
                continue;

            std::vector<std::size_t> floatingIndexes;
            for (std::size_t index = 0; index < state.windows.size(); ++index) {
                const auto& managed = state.windows[index];
                if (managed.targetMonitor == candidateMonitor && managed.isNiriFloatingOverlay)
                    floatingIndexes.push_back(index);
            }

            if (floatingIndexes.empty())
                continue;

            const Rect contentBoundsLocal = overviewContentRectForMonitor(candidateMonitor, state);
            const Rect contentBoundsGlobal = makeRect(candidateMonitor->m_position.x + contentBoundsLocal.x,
                                                      candidateMonitor->m_position.y + contentBoundsLocal.y,
                                                      contentBoundsLocal.width,
                                                      contentBoundsLocal.height);
            if (contentBoundsGlobal.width <= 1.0 || contentBoundsGlobal.height <= 1.0)
                continue;

            std::size_t staticObstacleCount = 0;
            std::vector<Rect> placedFloatingRects;
            placedFloatingRects.reserve(floatingIndexes.size());
            for (const auto& managed : state.windows) {
                if (managed.targetMonitor == candidateMonitor && !managed.isNiriFloatingOverlay)
                    ++staticObstacleCount;
            }

            std::stable_sort(floatingIndexes.begin(), floatingIndexes.end(), [&](std::size_t lhs, std::size_t rhs) {
                const Rect& lhsRect = state.windows[lhs].targetGlobal;
                const Rect& rhsRect = state.windows[rhs].targetGlobal;
                const double lhsArea = lhsRect.width * lhsRect.height;
                const double rhsArea = rhsRect.width * rhsRect.height;
                const auto   lhsAffinity = edgeMotionAffinityForRect(lhsRect, contentBoundsGlobal);
                const auto   rhsAffinity = edgeMotionAffinityForRect(rhsRect, contentBoundsGlobal);
                const double lhsAnchor = lhsAffinity.corner * 4.0 + std::max(lhsAffinity.verticalEdge, lhsAffinity.horizontalEdge) * 2.0;
                const double rhsAnchor = rhsAffinity.corner * 4.0 + std::max(rhsAffinity.verticalEdge, rhsAffinity.horizontalEdge) * 2.0;
                if (std::abs(lhsAnchor - rhsAnchor) > 0.01)
                    return lhsAnchor > rhsAnchor;
                if (std::abs(lhsArea - rhsArea) > 1.0)
                    return lhsArea > rhsArea;
                return lhs < rhs;
            });

            const auto overlapArea = [](const Rect& lhs, const Rect& rhs) {
                const double x1 = std::max(lhs.x, rhs.x);
                const double y1 = std::max(lhs.y, rhs.y);
                const double x2 = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
                const double y2 = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
                if (x2 <= x1 || y2 <= y1)
                    return 0.0;
                return (x2 - x1) * (y2 - y1);
            };

            const auto totalOverlapArea = [&](const Rect& target, const std::vector<Rect>& obstacles) {
                double overlap = 0.0;
                for (const auto& obstacle : obstacles)
                    overlap += overlapArea(target, obstacle);
                return overlap;
            };

            struct FloatingResolveResult {
                Rect   target;
                double scale = 1.0;
                double overlap = 0.0;
            };

            const auto resolveFloatingTarget = [&](const Rect& desired, const std::vector<Rect>& obstacles, double currentSlotScale,
                                                       const Rect& boundsGlobal) -> FloatingResolveResult {
                double initialScale = 1.0;
                const Rect clampedDesired = clampRectInsidePreservingAspect(desired, boundsGlobal, initialScale);
                if (obstacles.empty())
                    return {.target = clampedDesired, .scale = initialScale, .overlap = 0.0};

                std::vector<Rect> candidates;
                std::vector<double> candidateScales;
                candidates.reserve(128);
                candidateScales.reserve(128);
                const auto appendCandidate = [&](const Rect& candidate, double scale) {
                    const Rect clamped = clampRectInside(candidate, boundsGlobal);
                    if (std::ranges::any_of(candidates, [&](const Rect& existing) { return rectApproxEqual(existing, clamped, 0.5); }))
                        return;
                    candidates.push_back(clamped);
                    candidateScales.push_back(initialScale * scale);
                };

                const double absoluteScaleFloor = std::max(0.30, config.minSlotScale);
                const double scaleFloorBySlot = currentSlotScale > 0.001 ? absoluteScaleFloor / currentSlotScale : 0.72;
                const double scaleFloorByReadableSize =
                    config.minPreviewShortEdge / std::max(1.0, std::min(clampedDesired.width, clampedDesired.height));
                const double scaleFloor = std::min(1.0, std::max({0.52, scaleFloorBySlot, scaleFloorByReadableSize}));

                std::vector<double> scales;
                scales.push_back(1.0);
                for (double scale = 0.96; scale > scaleFloor + 0.001; scale -= 0.04)
                    scales.push_back(scale);
                if (scales.back() > scaleFloor + 0.001)
                    scales.push_back(scaleFloor);

                std::vector<std::pair<double, double>> baseOffsets;
                baseOffsets.push_back({0.0, 0.0});
                const double maxNudge = std::max(8.0, std::min(72.0, std::min(boundsGlobal.width, boundsGlobal.height) * 0.04));
                for (double radius : {gap, gap * 2.0, 16.0, 32.0, maxNudge}) {
                    if (radius <= 0.5 || radius > maxNudge + 0.5)
                        continue;
                    baseOffsets.push_back({radius, 0.0});
                    baseOffsets.push_back({-radius, 0.0});
                    baseOffsets.push_back({0.0, radius});
                    baseOffsets.push_back({0.0, -radius});
                    const double diagonal = radius * 0.70710678118;
                    baseOffsets.push_back({diagonal, diagonal});
                    baseOffsets.push_back({diagonal, -diagonal});
                    baseOffsets.push_back({-diagonal, diagonal});
                    baseOffsets.push_back({-diagonal, -diagonal});
                }

                for (const double scale : scales) {
                    const double width = clampedDesired.width * scale;
                    const double height = clampedDesired.height * scale;
                    const Rect   scaledDesired =
                        makeRect(clampedDesired.centerX() - width * 0.5, clampedDesired.centerY() - height * 0.5, width, height);
                    std::vector<std::pair<double, double>> offsets = baseOffsets;
                    const auto appendOffset = [&](double dx, double dy) {
                        offsets.push_back({dx, dy});
                    };
                    std::vector<double> candidateXs;
                    std::vector<double> candidateYs;
                    candidateXs.reserve(obstacles.size() * 3 + 4);
                    candidateYs.reserve(obstacles.size() * 3 + 4);
                    const auto appendX = [&](double x) {
                        if (!std::ranges::any_of(candidateXs, [&](double existing) { return std::abs(existing - x) <= 0.5; }))
                            candidateXs.push_back(x);
                    };
                    const auto appendY = [&](double y) {
                        if (!std::ranges::any_of(candidateYs, [&](double existing) { return std::abs(existing - y) <= 0.5; }))
                            candidateYs.push_back(y);
                    };
                    appendX(scaledDesired.x);
                    appendX(boundsGlobal.x);
                    appendX(boundsGlobal.x + boundsGlobal.width - width);
                    appendY(scaledDesired.y);
                    appendY(boundsGlobal.y);
                    appendY(boundsGlobal.y + boundsGlobal.height - height);
                    for (const auto& obstacle : obstacles) {
                        appendX(obstacle.x - width - gap);
                        appendX(obstacle.x + obstacle.width + gap);
                        appendX(obstacle.centerX() - width * 0.5);
                        appendY(obstacle.y - height - gap);
                        appendY(obstacle.y + obstacle.height + gap);
                        appendY(obstacle.centerY() - height * 0.5);
                        if (!rectsOverlap(scaledDesired, obstacle))
                            continue;

                        const double moveRight = obstacle.x + obstacle.width - scaledDesired.x;
                        const double moveLeft = obstacle.x - (scaledDesired.x + scaledDesired.width);
                        const double moveDown = obstacle.y + obstacle.height - scaledDesired.y;
                        const double moveUp = obstacle.y - (scaledDesired.y + scaledDesired.height);

                        appendOffset(moveRight, 0.0);
                        appendOffset(moveLeft, 0.0);
                        appendOffset(0.0, moveDown);
                        appendOffset(0.0, moveUp);
                        appendOffset(moveRight, moveDown);
                        appendOffset(moveRight, moveUp);
                        appendOffset(moveLeft, moveDown);
                        appendOffset(moveLeft, moveUp);
                    }
                    for (const auto& [dx, dy] : offsets) {
                        appendCandidate(makeRect(clampedDesired.centerX() + dx - width * 0.5, clampedDesired.centerY() + dy - height * 0.5, width, height), scale);
                    }
                    for (const double x : candidateXs) {
                        for (const double y : candidateYs)
                            appendCandidate(makeRect(x, y, width, height), scale);
                    }
                }

                const bool desiredOnRight = desired.centerX() >= boundsGlobal.centerX();
                const bool desiredOnBottom = desired.centerY() >= boundsGlobal.centerY();
                const double boundsDiag2 = boundsGlobal.width * boundsGlobal.width + boundsGlobal.height * boundsGlobal.height;

                FloatingResolveResult best{
                    .target = clampedDesired,
                    .scale = initialScale,
                    .overlap = totalOverlapArea(clampedDesired, obstacles),
                };
                double bestScore = std::numeric_limits<double>::max();
                for (std::size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
                    const auto& candidate = candidates[candidateIndex];
                    const double scale = candidateIndex < candidateScales.size() ? candidateScales[candidateIndex] : 1.0;
                    const double motionDistance2 = edgeAwareMotionDistance2(desired, candidate, boundsGlobal);
                    const double totalOverlap = totalOverlapArea(candidate, obstacles);
                    const double shrink = std::max(0.0, 1.0 - scale);

                    double score = motionDistance2 + shrink * shrink * 45000.0;
                    if ((candidate.centerX() >= boundsGlobal.centerX()) != desiredOnRight)
                        score += boundsDiag2 * 4.0;
                    if ((candidate.centerY() >= boundsGlobal.centerY()) != desiredOnBottom)
                        score += boundsDiag2;

                    if (totalOverlap < best.overlap - 0.5 || (std::abs(totalOverlap - best.overlap) <= 0.5 && score < bestScore)) {
                        best = {.target = candidate, .scale = scale, .overlap = totalOverlap};
                        bestScore = score;
                    }
                }

                return best;
            };

            for (const auto index : floatingIndexes) {
                const Rect desired = state.windows[index].targetGlobal;
                Rect workspaceBounds = contentBoundsGlobal;
                if (const auto viewportIt = niriWorkspaceViewportGlobalByWindowIndex.find(index); viewportIt != niriWorkspaceViewportGlobalByWindowIndex.end() &&
                    viewportIt->second.width > 1.0 && viewportIt->second.height > 1.0) {
                    workspaceBounds = viewportIt->second;
                }

                const Rect obstacleBounds = inflateRect(workspaceBounds, gap, gap);
                std::vector<Rect> relevantObstacles;
                relevantObstacles.reserve(placedFloatingRects.size());
                for (const auto& obstacle : placedFloatingRects) {
                    if (rectsOverlap(obstacle, obstacleBounds))
                        relevantObstacles.push_back(obstacle);
                }

                const auto resolved = resolveFloatingTarget(desired, relevantObstacles, state.windows[index].slot.scale, workspaceBounds);
                const Rect target = resolved.target;

                if (!rectApproxEqual(target, desired, 0.5) || std::abs(resolved.scale - 1.0) > 0.001) {
                    auto& managed = state.windows[index];
                    managed.targetGlobal = target;
                    managed.relayoutFromGlobal = target;
                    managed.slot.target = makeRect(target.x - candidateMonitor->m_position.x, target.y - candidateMonitor->m_position.y, target.width, target.height);
                    managed.slot.scale *= resolved.scale;
                    for (auto& slot : state.slots) {
                        if (slot.index == index) {
                            slot = managed.slot;
                            break;
                        }
                    }

                    if (debugLogsEnabled()) {
                        std::ostringstream out;
                        out << "[hymission] niri floating overview adjust window=" << debugWindowLabel(managed.window)
                            << " desired=" << rectToString(desired)
                            << " target=" << rectToString(target)
                            << " scaleAdjust=" << resolved.scale
                            << " remainingOverlap=" << resolved.overlap
                            << " staticObstaclesIgnored=" << staticObstacleCount
                            << " floatingObstacles=" << relevantObstacles.size();
                        debugLog(out.str());
                    }
                }

                placedFloatingRects.push_back(inflateRect(state.windows[index].targetGlobal, gap, gap));
            }
        }
    };
    settleNiriFloatingOverlayOverlaps();

    state.participatingMonitors = std::move(activeParticipatingMonitors);
    buildWorkspaceStripEntries(state);

    const auto selectionTarget = preferredSelectedWindow ? preferredSelectedWindow : focusedWindow;
    const auto* centeredEmptyPlaceholder = centeredEmptyWorkspacePlaceholder(state);
    const auto  centeredEmptyWorkspace = centeredEmptyPlaceholder ? centeredEmptyPlaceholder->workspace : PHLWORKSPACE{};
    const auto edgeCameraFocusCandidate = selectionTarget ? selectionTarget : focusedWindow;
    const bool preserveEdgeCamera = directNiriOwnerEdgeCameraActive(state) &&
        (!edgeCameraFocusCandidate || !edgeCameraFocusCandidate->m_isMapped || edgeCameraFocusCandidate->m_pinned ||
         edgeCameraFocusCandidate->m_workspace != state.ownerWorkspace);
    if (!preserveEdgeCamera && !selectWindowInState(state, selectionTarget) && !selectWindowInState(state, focusedWindow) && !state.windows.empty() &&
        !centeredEmptyPlaceholder) {
        state.selectedIndex = 0;
        state.focusDuringOverview = state.windows[*state.selectedIndex].window;
    }
    if (preserveEdgeCamera || (centeredEmptyPlaceholder && (!state.focusDuringOverview || state.focusDuringOverview->m_workspace != centeredEmptyWorkspace))) {
        state.selectedIndex.reset();
        state.focusDuringOverview.reset();
    }

    return state;
}

} // namespace hymission
