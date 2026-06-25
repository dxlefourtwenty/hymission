#include "overview_controller.hpp"
#include "overview_controller_niri_scrolling.hpp"

#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <cctype>
#include <expected>
#include <fstream>
#include <functional>
#include <limits>
#include <linux/input-event-codes.h>
#include <numeric>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#define private public
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#undef private

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/history/WorkspaceHistoryTracker.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ScrollMoveGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprutils/math/Region.hpp>

#include "overview_logic.hpp"

namespace hymission {

using Render::GL::g_pHyprOpenGL;
using Render::RENDER_MODE_FULL_FAKE;
using niri_scrolling_detail::consumeTwoColumnSwapPreviewTrace;
using niri_scrolling_detail::shouldWrapWorkspaceIds;
using niri_scrolling_detail::twoColumnSwapTraceActive;
using niri_scrolling_detail::windowsMoveAnimationConfig;
using niri_scrolling_detail::workspaceAnimationConfig;

namespace niri_scrolling_detail {
extern std::chrono::steady_clock::time_point workspaceSwitchDispatcherBlockUntil;
extern std::chrono::steady_clock::time_point overviewOpenInputBlockUntil;
extern std::chrono::steady_clock::time_point overviewHeavyEditInputBlockUntil;
extern bool workspaceSwitchDispatcherBlockRelayout;
bool directNiriWorkspaceTransferRenderGuardActive(const PHLWINDOW& window);
}

class OverviewOverlayPassElement final : public IPassElement {
  public:
    OverviewOverlayPassElement(OverviewController* controller, const PHLMONITOR& monitor, bool chromeOnly = false)
        : m_controller(controller), m_monitor(monitor), m_chromeOnly(chromeOnly) {
    }

    std::vector<UP<IPassElement>> draw() override {
        const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!m_controller || !renderMonitor)
            return {};

        const auto expectedMonitor = m_monitor.lock();
        if (!expectedMonitor || renderMonitor != expectedMonitor)
            return {};

        if (m_chromeOnly) {
            m_controller->renderSelectionChrome();
            return {};
        }

        m_controller->renderHiddenStripLayerProxies();
        m_controller->renderEmptyOverviewPlaceholder();
        m_controller->renderSelectionChrome();
        m_controller->renderNiriDragHint();
        m_controller->renderWorkspaceStrip();
        return {};
    }

    bool needsLiveBlur() override {
        return false;
    }

    bool needsPrecomputeBlur() override {
        return false;
    }

    bool undiscardable() override {
        return true;
    }

    std::optional<CBox> boundingBox() override {
        const auto monitor = m_monitor.lock();
        if (!monitor)
            return std::nullopt;

        return CBox{{}, monitor->m_size};
    }

    CRegion opaqueRegion() override {
        return {};
    }

    const char* passName() override {
        return "OverviewOverlayPassElement";
    }

    ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    OverviewController* m_controller = nullptr;
    PHLMONITORREF       m_monitor;
    bool                m_chromeOnly = false;
};

class OverviewWallpaperPassElement final : public IPassElement {
  public:
    OverviewWallpaperPassElement(OverviewController* controller, const PHLMONITOR& monitor) : m_controller(controller), m_monitor(monitor) {
    }

    std::vector<UP<IPassElement>> draw() override {
        const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        const auto expectedMonitor = m_monitor.lock();
        if (!m_controller || !renderMonitor || !expectedMonitor || renderMonitor != expectedMonitor)
            return {};

        m_controller->renderBackdrop();
        m_controller->renderNiriWorkspaceBackgrounds();
        m_controller->renderEmptyOverviewPlaceholder(true);
        if (m_controller->m_state.emptyWorkspacePlaceholders.empty())
            m_controller->renderEmptyOverviewPlaceholder();
        return {};
    }

    bool needsLiveBlur() override {
        return false;
    }

    bool needsPrecomputeBlur() override {
        return false;
    }

    bool undiscardable() override {
        return true;
    }

    std::optional<CBox> boundingBox() override {
        const auto monitor = m_monitor.lock();
        if (!monitor)
            return std::nullopt;

        return CBox{{}, monitor->m_size};
    }

    CRegion opaqueRegion() override {
        return {};
    }

    const char* passName() override {
        return "OverviewWallpaperPassElement";
    }

    ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    OverviewController* m_controller = nullptr;
    PHLMONITORREF       m_monitor;
};

namespace {

constexpr double OPEN_DURATION_MS = 180.0;
constexpr double CLOSE_DURATION_MS = 140.0;
constexpr auto   DIRECT_NIRI_OPEN_DISPATCHER_BLOCK_DURATION = std::chrono::milliseconds(750);
constexpr auto   DIRECT_NIRI_OPEN_INPUT_BLOCK_FALLBACK = std::chrono::milliseconds(1200);
constexpr auto   DIRECT_NIRI_OPEN_INPUT_POST_SETTLE = std::chrono::milliseconds(80);
constexpr auto   DIRECT_NIRI_HEAVY_EDIT_EXTRA_DELAY = std::chrono::milliseconds(250);
constexpr auto   DIRECT_NIRI_HEAVY_EDIT_POST_SETTLE = std::chrono::milliseconds(430);
constexpr double RELAYOUT_DURATION_MS = 140.0;
constexpr double WORKSPACE_TRANSITION_DURATION_MS = 180.0;
constexpr double CLOSE_SETTLE_TIMEOUT_MS = 80.0;
constexpr auto   NATIVE_ANIMATION_DISABLE_DURATION = std::chrono::milliseconds(320);
constexpr double CLOSE_SETTLE_EPSILON = 0.75;
constexpr std::size_t CLOSE_SETTLE_STABLE_FRAMES = 2;
constexpr double BACKDROP_ALPHA = 0.42;
constexpr double OUTLINE_THICKNESS = 4.0;
constexpr double HOVER_THICKNESS = 2.0;
constexpr double TITLE_PADDING = 12.0;
constexpr double STRIP_CARD_PADDING = 0.0;
constexpr double STRIP_THUMB_PADDING = 0.0;
constexpr double STRIP_LABEL_HEIGHT = 0.0;
constexpr double STRIP_MIN_THUMB_LENGTH = 12.0;
constexpr double RECOMMAND_STAGE_TRANSFER = 0.18;
constexpr double SELECTED_WINDOW_LAYOUT_EMPHASIS = 1.18;
constexpr double HOVER_SELECTION_RETARGET_DISTANCE = 18.0;
constexpr auto   HOVER_SELECTION_RETARGET_COOLDOWN = std::chrono::milliseconds(static_cast<int>(RELAYOUT_DURATION_MS + 48.0));
constexpr auto   HOVER_SELECTION_RETARGET_DWELL = std::chrono::milliseconds(48);
constexpr auto   TOGGLE_SWITCH_RELEASE_POLL_INTERVAL = std::chrono::milliseconds(16);
constexpr std::size_t WAYBAR_CURSOR_SHAPE_RESET_FRAMES = 12;
constexpr std::size_t STRIP_THEME_SURFACE_FEEDBACK_FRAMES = 8;
constexpr auto   POST_CLOSE_CURSOR_SHAPE_RESET_INTERVAL = std::chrono::milliseconds(16);
constexpr std::size_t POST_CLOSE_CURSOR_SHAPE_RESET_TICKS = 8;
constexpr auto   DEFERRED_OPEN_POLL_INTERVAL = std::chrono::milliseconds(16);
constexpr auto   THEME_SURFACE_FEEDBACK_INTERVAL = std::chrono::milliseconds(16);
constexpr std::size_t THEME_WORKSPACE_FEEDBACK_FRAMES = 3;
constexpr auto   MISSION_CONTROL_WORKSPACE_NAME = "Mission Control";
constexpr auto   MISSION_CONTROL_HIDDEN_WORKSPACE_PREFIX = "__hymission_hidden__:";
constexpr auto   DEFAULT_HIDE_BAR_NAMESPACES = "hypr-dock,waybar,chromack,wardnc,wardbnc,dashboard,rofi";
constexpr auto   DEFAULT_HIDE_OVERVIEW_LAYER_NAMESPACES = "chromack,wardnc,wardbnc,dashboard,rofi";
constexpr double DIRECT_NIRI_NATIVE_HANDOFF_VISUAL_EPSILON = 0.004;
constexpr auto   DIRECT_NIRI_NATIVE_HANDOFF_GUARD_DURATION = std::chrono::milliseconds(120);
OverviewController* g_controller = nullptr;
std::chrono::steady_clock::time_point g_directNiriNativeHandoffUntil;
constexpr std::size_t DIRECT_NIRI_CLOSE_FINAL_RENDER_FRAMES = 3;
std::unordered_map<const OverviewController*, std::size_t> g_directNiriCloseFinalRenderFrames;

void clearDirectNiriCloseFinalRenderFrames(const OverviewController* controller) {
    if (!controller)
        return;

    g_directNiriCloseFinalRenderFrames.erase(controller);
}

void armDirectNiriCloseFinalRenderFrames(const OverviewController* controller) {
    if (!controller)
        return;

    g_directNiriCloseFinalRenderFrames[controller] = DIRECT_NIRI_CLOSE_FINAL_RENDER_FRAMES;
}

std::optional<std::size_t> consumeDirectNiriCloseFinalRenderFrame(const OverviewController* controller) {
    if (!controller)
        return std::nullopt;

    const auto it = g_directNiriCloseFinalRenderFrames.find(controller);
    if (it == g_directNiriCloseFinalRenderFrames.end())
        return std::nullopt;

    if (it->second == 0) {
        g_directNiriCloseFinalRenderFrames.erase(it);
        return 0;
    }

    --it->second;
    return it->second;
}

bool directNiriNativeHandoffActive() {
    if (g_directNiriNativeHandoffUntil == std::chrono::steady_clock::time_point{})
        return false;

    if (std::chrono::steady_clock::now() < g_directNiriNativeHandoffUntil)
        return true;

    g_directNiriNativeHandoffUntil = {};
    return false;
}

void armDirectNiriNativeHandoffGuard() {
    const auto until = std::chrono::steady_clock::now() + DIRECT_NIRI_NATIVE_HANDOFF_GUARD_DURATION;
    if (until > g_directNiriNativeHandoffUntil)
        g_directNiriNativeHandoffUntil = until;
}
std::unordered_map<std::string, std::function<SDispatchResult(std::string)>> g_openingDispatcherGateOriginals;

using LayoutMessageActionFn = Config::Actions::ActionResult (*)(const std::string&);
using MoveFocusActionFn = Config::Actions::ActionResult (*)(Math::eDirection);
using MoveInDirectionActionFn = Config::Actions::ActionResult (*)(Math::eDirection, std::optional<PHLWINDOW>);
using SwapInDirectionActionFn = Config::Actions::ActionResult (*)(Math::eDirection, std::optional<PHLWINDOW>);
using ResizeActionFn = Config::Actions::ActionResult (*)(const Vector2D&, bool, std::optional<PHLWINDOW>);

CFunctionHook* g_layoutMessageActionHook = nullptr;
CFunctionHook* g_moveFocusActionHook = nullptr;
CFunctionHook* g_moveInDirectionActionHook = nullptr;
CFunctionHook* g_swapInDirectionActionHook = nullptr;
CFunctionHook* g_resizeActionHook = nullptr;
LayoutMessageActionFn g_layoutMessageActionOriginal = nullptr;
MoveFocusActionFn g_moveFocusActionOriginal = nullptr;
MoveInDirectionActionFn g_moveInDirectionActionOriginal = nullptr;
SwapInDirectionActionFn g_swapInDirectionActionOriginal = nullptr;
ResizeActionFn g_resizeActionOriginal = nullptr;

bool& g_niriStripSnapshotSingleWorkspaceOnly = niri_scrolling_detail::stripSnapshotSingleWorkspaceOnly;

bool isOverviewEditingDispatcherCandidate(std::string_view name) {
    std::string lowered{name};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lowered == "movewindow" || lowered == "movewindoworgroup" || lowered == "swapwindow" || lowered == "movetoworkspace" ||
        lowered == "movetoworkspacesilent" || lowered == "moveactive" || lowered == "resizeactive" || lowered == "swapactive" ||
        lowered == "movecol" || lowered == "movecolumn" || lowered == "swapcol" || lowered == "swapcolumn" ||
        lowered == "resizecol" || lowered == "resizecolumn" || lowered == "resizewindow" ||
        lowered == "togglefloating" || lowered == "setfloating" || lowered == "settiled" || lowered == "pin" ||
        lowered.starts_with("movewindow") || lowered.starts_with("swapwindow") || lowered.starts_with("movetoworkspace") ||
        lowered.starts_with("movecol") || lowered.starts_with("movecolumn") || lowered.starts_with("swapcol") || lowered.starts_with("swapcolumn") ||
        lowered.starts_with("resizecol") || lowered.starts_with("resizecolumn") || lowered.starts_with("resizewindow") || lowered.starts_with("togglefloating") || lowered.starts_with("setfloating") ||
        lowered.starts_with("settiled") || lowered.starts_with("pin") ||
        lowered.find("window.move") != std::string::npos || lowered.find("window.swap") != std::string::npos ||
        lowered.find("window.resize") != std::string::npos || lowered.find("window.workspace") != std::string::npos ||
        lowered.find("window.float") != std::string::npos || lowered.find("window.pin") != std::string::npos;
}


bool isOverviewToggleControlDispatcher(std::string_view dispatcherName) {
    std::string lowered{dispatcherName};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lowered == "hymission:toggle" || lowered == "hymission.open" || lowered == "hymission:open" ||
        lowered == "hymission.close" || lowered == "hymission:close" || lowered == "hymission.toggle";
}


bool isDelayedOverviewOpenEditCommand(std::string_view dispatcherName, std::string_view dispatcherArgs) {
    std::string name{dispatcherName};
    std::string args{dispatcherArgs};
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(args.begin(), args.end(), args.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    const bool layoutMessage = name == "layoutmsg" || name == "layout";
    const bool swapColumnLayoutMessage = layoutMessage &&
        (args == "swapcol" || args.starts_with("swapcol ") || args.starts_with("swapcol,") || args.find("swapcol") != std::string::npos ||
         args == "swap col" || args.starts_with("swap col ") || args.starts_with("swap col,") ||
         args == "swap +col" || args.starts_with("swap +col ") || args.starts_with("swap +col,") ||
         args == "swap -col" || args.starts_with("swap -col ") || args.starts_with("swap -col,"));
    const bool resizeColumnLayoutMessage = layoutMessage &&
        (args == "resizecol" || args.starts_with("resizecol ") || args.starts_with("resizecol,") ||
         args == "resize +col" || args.starts_with("resize +col ") || args.starts_with("resize +col,") ||
         args == "resize col" || args.starts_with("resize col ") || args.starts_with("resize col,") ||
         args == "resize -col" || args.starts_with("resize -col ") || args.starts_with("resize -col,") ||
         args.find("resizecol") != std::string::npos || args.find("resize col") != std::string::npos || args.find("colresize") != std::string::npos);

    return name == "swapcol" || name == "swapcolumn" || name == "resizeactive" ||
        name == "resizecol" || name == "resizecolumn" || name == "resizewindow" ||
        name.starts_with("swapcol") || name.starts_with("swapcolumn") ||
        name.starts_with("resizeactive") || name.starts_with("resizecol") ||
        name.starts_with("resizecolumn") || name.starts_with("resizewindow") ||
        name.find("window.resize") != std::string::npos || swapColumnLayoutMessage || resizeColumnLayoutMessage;
}

bool overviewOpenInputBarrierActive() {
    return niri_scrolling_detail::overviewOpenInputBlockUntil != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() < niri_scrolling_detail::overviewOpenInputBlockUntil;
}

bool overviewHeavyEditInputBarrierActive() {
    return niri_scrolling_detail::overviewHeavyEditInputBlockUntil != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() < niri_scrolling_detail::overviewHeavyEditInputBlockUntil;
}

void armOverviewOpenInputBarrier(std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    if (niri_scrolling_detail::overviewOpenInputBlockUntil == std::chrono::steady_clock::time_point{} ||
        until > niri_scrolling_detail::overviewOpenInputBlockUntil)
        niri_scrolling_detail::overviewOpenInputBlockUntil = until;
}

void armOverviewHeavyEditInputBarrier(std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    if (niri_scrolling_detail::overviewHeavyEditInputBlockUntil == std::chrono::steady_clock::time_point{} ||
        until > niri_scrolling_detail::overviewHeavyEditInputBlockUntil)
        niri_scrolling_detail::overviewHeavyEditInputBlockUntil = until;
}

void settleOverviewOpenInputBarrier() {
    niri_scrolling_detail::overviewOpenInputBlockUntil = std::chrono::steady_clock::now() + DIRECT_NIRI_OPEN_INPUT_POST_SETTLE;
}

void settleOverviewHeavyEditInputBarrier() {
    niri_scrolling_detail::overviewHeavyEditInputBlockUntil = std::chrono::steady_clock::now() + DIRECT_NIRI_HEAVY_EDIT_POST_SETTLE;
}

[[maybe_unused]] void clearExpiredOverviewOpenInputBarrier() {
    if (niri_scrolling_detail::overviewOpenInputBlockUntil != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= niri_scrolling_detail::overviewOpenInputBlockUntil)
        niri_scrolling_detail::overviewOpenInputBlockUntil = {};
}

void clearExpiredOverviewHeavyEditInputBarrier() {
    if (niri_scrolling_detail::overviewHeavyEditInputBlockUntil != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= niri_scrolling_detail::overviewHeavyEditInputBlockUntil)
        niri_scrolling_detail::overviewHeavyEditInputBlockUntil = {};
}

bool shouldSuppressNativeActionDuringOverviewOpen(bool delayedHeavyEdit = false) {
    clearExpiredOverviewOpenInputBarrier();
    clearExpiredOverviewHeavyEditInputBarrier();
    return g_controller && (overviewOpenInputBarrierActive() || (delayedHeavyEdit && overviewHeavyEditInputBarrierActive()));
}

Config::Actions::ActionResult hkLayoutMessageAction(const std::string& msg) {
    if (shouldSuppressNativeActionDuringOverviewOpen(isDelayedOverviewOpenEditCommand("layoutmsg", msg)))
        return {};

    return g_layoutMessageActionOriginal ? g_layoutMessageActionOriginal(msg) : Config::Actions::ActionResult{};
}

Config::Actions::ActionResult hkMoveFocusAction(Math::eDirection direction) {
    if (shouldSuppressNativeActionDuringOverviewOpen())
        return {};

    return g_moveFocusActionOriginal ? g_moveFocusActionOriginal(direction) : Config::Actions::ActionResult{};
}

Config::Actions::ActionResult hkMoveInDirectionAction(Math::eDirection direction, std::optional<PHLWINDOW> window) {
    if (shouldSuppressNativeActionDuringOverviewOpen())
        return {};

    return g_moveInDirectionActionOriginal ? g_moveInDirectionActionOriginal(direction, std::move(window)) : Config::Actions::ActionResult{};
}

Config::Actions::ActionResult hkSwapInDirectionAction(Math::eDirection direction, std::optional<PHLWINDOW> window) {
    if (shouldSuppressNativeActionDuringOverviewOpen())
        return {};

    return g_swapInDirectionActionOriginal ? g_swapInDirectionActionOriginal(direction, std::move(window)) : Config::Actions::ActionResult{};
}

Config::Actions::ActionResult hkResizeAction(const Vector2D& size, bool relative, std::optional<PHLWINDOW> window) {
    if (shouldSuppressNativeActionDuringOverviewOpen(true))
        return {};

    return g_resizeActionOriginal ? g_resizeActionOriginal(size, relative, std::move(window)) : Config::Actions::ActionResult{};
}

enum class GestureDispatcherKind : uint8_t {
    Toggle,
    Open,
};

class ScopedFlag {
  public:
    explicit ScopedFlag(bool& flag, bool value = true) : m_flag(flag), m_previous(flag) {
        m_flag = value;
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


std::unordered_map<const void*, uint64_t> g_openOverviewLayoutConfigSignatures;

uint64_t mixSignatureValue(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
    return hash;
}

uint64_t stableStringSignature(std::string value) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : value) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t layoutAffectingConfigSignature(HANDLE handle) {
    uint64_t hash = 1469598103934665603ULL;

    const auto mixInt = [&](const char* name, long fallback) {
        hash = mixSignatureValue(hash, stableStringSignature(name));
        hash = mixSignatureValue(hash, static_cast<uint64_t>(getConfigInt(handle, name, fallback)));
    };
    const auto mixFloat = [&](const char* name, double fallback) {
        hash = mixSignatureValue(hash, stableStringSignature(name));
        hash = mixSignatureValue(hash, static_cast<uint64_t>(std::llround(getConfigFloat(handle, name, fallback) * 1000000.0)));
    };
    const auto mixString = [&](const char* name, const std::string& fallback) {
        hash = mixSignatureValue(hash, stableStringSignature(name));
        hash = mixSignatureValue(hash, stableStringSignature(getConfigString(handle, name, fallback)));
    };

    mixInt("plugin:hymission:outer_padding", 32);
    mixInt("plugin:hymission:outer_padding_top", 92);
    mixInt("plugin:hymission:outer_padding_right", 32);
    mixInt("plugin:hymission:outer_padding_bottom", 32);
    mixInt("plugin:hymission:outer_padding_left", 32);
    mixInt("plugin:hymission:row_spacing", 32);
    mixInt("plugin:hymission:column_spacing", 32);
    mixInt("plugin:hymission:min_window_length", 120);
    mixInt("plugin:hymission:min_preview_short_edge", 32);
    mixFloat("plugin:hymission:small_window_boost", 1.35);
    mixFloat("plugin:hymission:max_preview_scale", 0.95);
    mixFloat("plugin:hymission:workspace_overview_max_preview_scale", 0.95);
    mixFloat("plugin:hymission:min_slot_scale", 0.10);
    mixFloat("plugin:hymission:natural_scale_flex", 0.22);
    mixFloat("plugin:hymission:layout_scale_weight", 1.0);
    mixFloat("plugin:hymission:layout_space_weight", 0.10);
    mixInt("plugin:hymission:expand_selected_window", 1);
    mixInt("plugin:hymission:multi_workspace_expand_selected_window", 1);
    mixInt("plugin:hymission:multi_workspace_sort_recent_first", 1);
    mixInt("plugin:hymission:niri_mode", 0);
    mixFloat("plugin:hymission:niri_layout_scale", 1.0);
    mixFloat("plugin:hymission:niri_overview_scale", 0.65);
    mixFloat("plugin:hymission:niri_window_gaps", -1.0);
    mixFloat("plugin:hymission:niri_single_ws_gap_multiplier", -1.0);
    mixFloat("plugin:hymission:niri_single_ws_gap_pixels", -1.0);
    mixFloat("plugin:hymission:niri_multi_ws_scale", 0.18);
    mixFloat("plugin:hymission:niri_workspace_gap", -1.0);
    mixFloat("plugin:hymission:niri_multi_ws_gap", -1.0);
    mixFloat("plugin:hymission:niri_workspace_scale", 1.0);
    mixFloat("plugin:hymission:niri_strip_workspace_scale", 1.30);
    mixFloat("plugin:hymission:niri_strip_workspace_zoom", 2.0);
    mixInt("plugin:hymission:niri_mode_show_empty_workspaces_btwn", 1);
    mixInt("plugin:hymission:niri_preview_disabled", 0);
    mixInt("plugin:hymission:niri_overview_animations", 1);
    mixFloat("plugin:hymission:niri_overview_open_close_speed_multiplier", 1.5);
    mixInt("plugin:hymission:one_workspace_per_row", 0);
    mixInt("plugin:hymission:only_active_workspace", 0);
    mixInt("plugin:hymission:only_active_monitor", 0);
    mixInt("plugin:hymission:show_special", 0);
    mixInt("plugin:hymission:workspace_strip_thickness", 160);
    mixInt("plugin:hymission:workspace_strip_gap", 24);
    mixString("plugin:hymission:layout_engine", "grid");
    mixString("plugin:hymission:workspace_strip_anchor", "left");
    mixString("plugin:hymission:workspace_strip_empty_mode", "existing");

    // Hyprland's native scrolling layout reads these live.  Hymission must
    // treat changes to them as layout mutations while its direct niri overview
    // is open, otherwise cached overview geometry can fight the newly rebuilt
    // native scrolling camera.
    mixInt("scrolling:focus_fit_method", 0);
    mixInt("scrolling:fullscreen_on_one_column", 1);
    mixInt("scrolling:follow_focus", 1);
    mixInt("scrolling:column_default_width", 0);
    mixInt("scrolling:default_column_width", 0);
    mixInt("scrolling:center_window", 0);
    mixInt("general:gaps_in", 0);
    mixInt("general:gaps_out", 0);
    mixInt("general:gaps_workspaces", 0);

    return hash;
}


SP<Hyprutils::Animation::SAnimationPropertyConfig> scaledAnimationConfig(
    const SP<Hyprutils::Animation::SAnimationPropertyConfig>& baseConfig, double speedMultiplier) {
    if (!baseConfig)
        return {};

    const auto values = baseConfig->pValues.lock();
    if (!values)
        return baseConfig;

    auto config = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    config->overridden = true;
    config->internalBezier = values->internalBezier;
    config->internalStyle = values->internalStyle;
    config->internalSpeed = values->internalSpeed / static_cast<float>(speedMultiplier);
    config->internalEnabled = values->internalEnabled;
    config->pValues = config;
    config->pParentAnimation = baseConfig;
    return config;
}

Config::CGradientValueData activeBorderGradient() {
    static auto PACTIVEBORDER = CConfigValue<Config::IComplexConfigValue>("general:col.active_border");

    if (const auto* const gradient = dynamic_cast<Config::CGradientValueData*>(PACTIVEBORDER.ptr()); gradient && !gradient->m_colors.empty()) {
        return *gradient;
    }

    return Config::CGradientValueData(CHyprColor(0.97, 0.985, 1.0, 1.0));
}

Config::CGradientValueData inactiveBorderGradient() {
    static auto PINACTIVEBORDER = CConfigValue<Config::IComplexConfigValue>("general:col.inactive_border");

    if (const auto* const gradient = dynamic_cast<Config::CGradientValueData*>(PINACTIVEBORDER.ptr()); gradient && !gradient->m_colors.empty()) {
        return *gradient;
    }

    return Config::CGradientValueData(CHyprColor(0.18, 0.09, 0.09, 1.0));
}

CHyprColor activeBorderColorWithAlpha(double alpha) {
    auto gradient = activeBorderGradient();
    if (!gradient.m_colors.empty()) {
        CHyprColor color = gradient.m_colors.front();
        color.a *= static_cast<float>(std::clamp(alpha, 0.0, 1.0));
        return color;
    }

    return CHyprColor(0.97, 0.985, 1.0, std::clamp(alpha, 0.0, 1.0));
}

uint64_t activeBorderGradientSignature() {
    const auto gradient = activeBorderGradient();
    uint64_t   hash = 1469598103934665603ULL;
    const auto mix = [&](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };

    mix(static_cast<uint64_t>(gradient.m_colors.size()));
    mix(static_cast<uint64_t>(std::llround(static_cast<double>(gradient.m_angle) * 1000000.0)));
    for (const auto& color : gradient.m_colors)
        mix(static_cast<uint64_t>(color.getAsHex()));

    return hash;
}

std::string luaStringLiteral(const std::string& value) {
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '\\' || ch == '"')
            out += '\\';
        out += ch;
    }
    out += '"';
    return out;
}

std::string luaConfigExpression(const std::string& name, const std::string& value) {
    std::vector<std::string> parts;
    std::string              current;
    for (const char ch : name) {
        if (ch == ':' || ch == '.') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }

        current += ch;
    }

    if (!current.empty())
        parts.push_back(current);

    if (parts.empty())
        return {};

    std::string expression = "hl.config({";
    for (std::size_t i = 0; i + 1 < parts.size(); ++i)
        expression += "[" + luaStringLiteral(parts[i]) + "] = {";

    expression += "[" + luaStringLiteral(parts.back()) + "] = " + value;

    for (std::size_t i = 0; i < parts.size(); ++i)
        expression += "}";

    expression += ")";
    return expression;
}

std::string setConfigKeyword(const std::string& name, const std::string& value) {
    const auto result = HyprlandAPI::invokeHyprctlCommand("keyword", name + " " + value);
    if (result == "ok")
        return {};

    if (result != "keyword can't work with non-legacy parsers. Use eval.")
        return result;

    const auto expression = luaConfigExpression(name, value);
    if (expression.empty())
        return result;

    const auto evalResult = HyprlandAPI::invokeHyprctlCommand("eval", expression);
    return evalResult == "ok" ? std::string{} : evalResult;
}

std::optional<uint32_t> parseSwitchReleaseKeycode(const std::string& value) {
    if (value.empty())
        return std::nullopt;

    const auto parseCode = [](const std::string& digits) -> std::optional<uint32_t> {
        if (digits.empty() || !std::ranges::all_of(digits, [](unsigned char ch) { return std::isdigit(ch) != 0; }))
            return std::nullopt;

        try {
            return static_cast<uint32_t>(std::stoul(digits));
        } catch (...) {
            return std::nullopt;
        }
    };

    if (value.starts_with("code:"))
        return parseCode(value.substr(5));

    const auto parsed = parseCode(value);
    if (parsed && *parsed > 9)
        return parsed;

    return std::nullopt;
}

std::string asciiLowerCopy(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

LayoutEngine parseLayoutEngine(std::string value) {
    value = asciiLowerCopy(std::move(value));
    if (value == "natural" || value == "apple" || value == "apple-like" || value == "expose" || value == "mission-control")
        return LayoutEngine::Natural;

    return LayoutEngine::Grid;
}

std::optional<uint32_t> switchReleaseModifierMask(const std::string& value) {
    const auto lowered = asciiLowerCopy(value);
    if (lowered == "shift" || lowered == "shift_l" || lowered == "shift_r")
        return HL_MODIFIER_SHIFT;
    if (lowered == "ctrl" || lowered == "control" || lowered == "control_l" || lowered == "control_r" || lowered == "ctrl_l" || lowered == "ctrl_r")
        return HL_MODIFIER_CTRL;
    if (lowered == "alt" || lowered == "alt_l" || lowered == "alt_r")
        return HL_MODIFIER_ALT;
    if (lowered == "super" || lowered == "super_l" || lowered == "super_r" || lowered == "meta" || lowered == "meta_l" || lowered == "meta_r")
        return HL_MODIFIER_META;

    return std::nullopt;
}

xkb_keysym_t keysymFromConfiguredSwitchReleaseKey(const std::string& value) {
    if (value.empty())
        return XKB_KEY_NoSymbol;

    return xkb_keysym_from_name(value.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
}

bool keyboardHasPressedKeysym(const SP<IKeyboard>& keyboard, xkb_keysym_t target) {
    if (!keyboard || !keyboard->m_xkbState || target == XKB_KEY_NoSymbol)
        return false;

    xkb_keymap* const keymap = xkb_state_get_keymap(keyboard->m_xkbState);
    if (!keymap)
        return false;

    for (xkb_keycode_t keycode = xkb_keymap_min_keycode(keymap); keycode <= xkb_keymap_max_keycode(keymap); ++keycode) {
        if (!keyboard->getPressed(keycode))
            continue;

        if (xkb_state_key_get_one_sym(keyboard->m_xkbState, keycode) == target)
            return true;
    }

    return false;
}

template <typename Predicate>
bool anyKeyboardWithState(Predicate&& predicate) {
    for (const auto& candidate : g_pInputManager->m_keyboards) {
        if (!candidate || !candidate->m_xkbState)
            continue;

        if (predicate(candidate))
            return true;
    }

    return false;
}

int recommandScopeSign(OverviewController::ScopeOverride scope) {
    return scope == OverviewController::ScopeOverride::ForceAll ? 1 : -1;
}

int signedUnit(double value) {
    if (value > 0.0001)
        return 1;
    if (value < -0.0001)
        return -1;
    return 0;
}

double swipeDistanceForDirection(const ITrackpadGesture::STrackpadGestureBegin& e) {
    if (!e.swipe)
        return 0.0;

    if (e.direction == TRACKPAD_GESTURE_DIR_LEFT || e.direction == TRACKPAD_GESTURE_DIR_RIGHT || e.direction == TRACKPAD_GESTURE_DIR_HORIZONTAL)
        return static_cast<double>(e.scale) * (e.direction == TRACKPAD_GESTURE_DIR_LEFT ? -e.swipe->delta.x : e.swipe->delta.x);

    if (e.direction == TRACKPAD_GESTURE_DIR_UP || e.direction == TRACKPAD_GESTURE_DIR_DOWN || e.direction == TRACKPAD_GESTURE_DIR_VERTICAL)
        return static_cast<double>(e.scale) * (e.direction == TRACKPAD_GESTURE_DIR_UP ? -e.swipe->delta.y : e.swipe->delta.y);

    if (e.direction == TRACKPAD_GESTURE_DIR_SWIPE)
        return static_cast<double>(e.scale) * e.swipe->delta.size();

    return static_cast<double>(e.scale) * e.swipe->delta.size();
}

double swipeDistanceForDirection(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    return swipeDistanceForDirection(ITrackpadGesture::STrackpadGestureBegin{
        .swipe = e.swipe,
        .pinch = e.pinch,
        .direction = e.direction,
        .scale = e.scale,
    });
}

double clampUnit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

bool hasUsableWindowSize(const Vector2D& size) {
    return size.x >= 1.0 && size.y >= 1.0;
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

Rect scaleRectAroundCenter(const Rect& rect, double scale) {
    const double clampedScale = std::max(0.0, scale);
    const double width = rect.width * clampedScale;
    const double height = rect.height * clampedScale;
    return makeRect(rect.centerX() - width * 0.5, rect.centerY() - height * 0.5, width, height);
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


bool rectFitsInsideBounds(const Rect& rect, const Rect& bounds, double epsilon = 0.5) {
    return rect.x >= bounds.x - epsilon && rect.y >= bounds.y - epsilon && rect.x + rect.width <= bounds.x + bounds.width + epsilon &&
        rect.y + rect.height <= bounds.y + bounds.height + epsilon;
}

double maxCenteredScaleForBounds(const Rect& rect, const Rect& bounds) {
    if (rect.width <= 1.0 || rect.height <= 1.0)
        return 1.0;

    const double halfWidth = std::max(0.0, std::min(rect.centerX() - bounds.x, bounds.x + bounds.width - rect.centerX()));
    const double halfHeight = std::max(0.0, std::min(rect.centerY() - bounds.y, bounds.y + bounds.height - rect.centerY()));
    const double maxWidth = halfWidth * 2.0;
    const double maxHeight = halfHeight * 2.0;
    return std::max(1.0, std::min(maxWidth / rect.width, maxHeight / rect.height));
}

double maxCenteredScaleForPerSideGrowth(const Rect& rect, double maxGrowXPerSide, double maxGrowYPerSide) {
    if (rect.width <= 1.0 || rect.height <= 1.0)
        return 1.0;

    const double scaleX = 1.0 + std::max(0.0, maxGrowXPerSide) * 2.0 / rect.width;
    const double scaleY = 1.0 + std::max(0.0, maxGrowYPerSide) * 2.0 / rect.height;
    return std::max(1.0, std::min(scaleX, scaleY));
}

struct EdgeMotionAffinity {
    double verticalEdge = 0.0;
    double horizontalEdge = 0.0;
    double corner = 0.0;
};

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

std::optional<double> overlapExitDistanceAlongDirection(const Rect& moving, const Rect& obstacle, double dirX, double dirY) {
    if (!rectsOverlap(moving, obstacle))
        return 0.0;

    std::optional<double> exitDistance;
    const auto consider = [&](double candidate) {
        if (candidate < 0.0)
            return;
        candidate += 0.5;
        if (!exitDistance || candidate < *exitDistance)
            exitDistance = candidate;
    };

    if (dirX > 0.001)
        consider((obstacle.x + obstacle.width - moving.x) / dirX);
    else if (dirX < -0.001)
        consider((moving.x + moving.width - obstacle.x) / -dirX);

    if (dirY > 0.001)
        consider((obstacle.y + obstacle.height - moving.y) / dirY);
    else if (dirY < -0.001)
        consider((moving.y + moving.height - obstacle.y) / -dirY);

    return exitDistance;
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

Rect rectToMonitorRenderLocal(const Rect& rect, const PHLMONITOR& monitor) {
    return scaleRectForRender(rectToMonitorLocal(rect, monitor), monitor);
}

SP<Render::IFramebuffer> createFramebuffer(const std::string& name) {
    return g_pHyprRenderer ? g_pHyprRenderer->createFB(name) : nullptr;
}

void setTextureLinearFiltering(const SP<Render::ITexture>& texture) {
    if (!texture)
        return;

    texture->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    texture->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

void setFramebufferLinearFiltering(Render::IFramebuffer& framebuffer) {
    setTextureLinearFiltering(framebuffer.getTexture());
}

std::optional<GLuint> framebufferId(Render::IFramebuffer& framebuffer) {
    auto* glFramebuffer = dynamic_cast<Render::GL::CGLFramebuffer*>(&framebuffer);
    if (!glFramebuffer)
        return std::nullopt;

    return glFramebuffer->getFBID();
}

struct FramebufferBlitRect {
    GLint left = 0;
    GLint bottom = 0;
    GLint right = 0;
    GLint top = 0;
};

std::optional<FramebufferBlitRect> rectToFramebufferBlitRect(const Rect& rect, const Vector2D& framebufferSize) {
    const GLint framebufferWidth = std::max(1, static_cast<int>(std::lround(framebufferSize.x)));
    const GLint framebufferHeight = std::max(1, static_cast<int>(std::lround(framebufferSize.y)));

    const GLint left = std::clamp(static_cast<GLint>(std::floor(rect.x)), 0, framebufferWidth);
    const GLint right = std::clamp(static_cast<GLint>(std::ceil(rect.x + rect.width)), 0, framebufferWidth);
    const GLint topFromTop = std::clamp(static_cast<GLint>(std::floor(rect.y)), 0, framebufferHeight);
    const GLint bottomFromTop = std::clamp(static_cast<GLint>(std::ceil(rect.y + rect.height)), 0, framebufferHeight);
    const GLint bottom = framebufferHeight - bottomFromTop;
    const GLint top = framebufferHeight - topFromTop;

    if (left >= right || bottom >= top)
        return std::nullopt;

    return FramebufferBlitRect{
        .left = left,
        .bottom = bottom,
        .right = right,
        .top = top,
    };
}

bool blitFramebufferRegion(Render::IFramebuffer& sourceFramebuffer, Render::IFramebuffer& targetFramebuffer, const Rect& sourceRect, const Rect& targetRect) {
    if (!sourceFramebuffer.isAllocated() || !targetFramebuffer.isAllocated())
        return false;

    const auto sourceBlitRect = rectToFramebufferBlitRect(sourceRect, sourceFramebuffer.m_size);
    const auto targetBlitRect = rectToFramebufferBlitRect(targetRect, targetFramebuffer.m_size);
    if (!sourceBlitRect || !targetBlitRect)
        return false;
    const auto sourceFramebufferId = framebufferId(sourceFramebuffer);
    const auto targetFramebufferId = framebufferId(targetFramebuffer);
    if (!sourceFramebufferId || !targetFramebufferId)
        return false;

    GLint       previousReadFramebuffer = 0;
    GLint       previousDrawFramebuffer = 0;
    GLfloat     previousClearColor[4] = {};
    const bool  scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);

    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, *targetFramebufferId);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, *sourceFramebufferId);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, *targetFramebufferId);
    glBlitFramebuffer(sourceBlitRect->left, sourceBlitRect->bottom, sourceBlitRect->right, sourceBlitRect->top, targetBlitRect->left, targetBlitRect->bottom,
                      targetBlitRect->right, targetBlitRect->top, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glClearColor(previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
    if (scissorEnabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);

    return glGetError() == GL_NO_ERROR;
}

bool renderTextureIntoFramebuffer(const PHLMONITOR& monitor, const SP<Render::IFramebuffer>& targetFramebuffer, const SP<Render::ITexture>& texture, const CBox& destinationBox) {
    if (!monitor || !g_pHyprRenderer || !g_pHyprOpenGL || !texture || !targetFramebuffer || !targetFramebuffer->isAllocated())
        return false;

    setTextureLinearFiltering(texture);
    setFramebufferLinearFiltering(*targetFramebuffer);

    const bool previousBlockScreenShader = g_pHyprRenderer->m_renderData.blockScreenShader;
    CRegion     fakeDamage{0, 0, targetFramebuffer->m_size.x, targetFramebuffer->m_size.y};
    if (!g_pHyprRenderer->beginFullFakeRender(monitor, fakeDamage, targetFramebuffer)) {
        g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockScreenShader;
        return false;
    }

    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->draw(CClearPassElement::SClearData{.color = CHyprColor{0.0, 0.0, 0.0, 0.0}}, fakeDamage);
    g_pHyprOpenGL->renderTexture(texture, destinationBox, {.a = 1.0F});
    g_pHyprRenderer->endRender();
    g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockScreenShader;
    return true;
}

struct GaussianBlurPipeline {
    GLuint program = 0;
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLint  textureLocation = -1;
    GLint  texelSizeLocation = -1;
    GLint  directionLocation = -1;
    GLint  radiusLocation = -1;
    bool   ready = false;
    bool   failed = false;
};

GaussianBlurPipeline& gaussianBlurPipeline() {
    static GaussianBlurPipeline pipeline;
    return pipeline;
}

GLuint compileShaderStage(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;

    GLint infoLogLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLength);
    std::string infoLog(std::max(1, infoLogLength), '\0');
    glGetShaderInfoLog(shader, infoLogLength, nullptr, infoLog.data());
    glDeleteShader(shader);
    if (Log::logger)
        Log::logger->log(Log::ERR, "[hymission] gaussian blur shader compile failed: " + infoLog);
    return 0;
}

bool ensureGaussianBlurPipeline() {
    auto& pipeline = gaussianBlurPipeline();
    if (pipeline.ready)
        return true;
    if (pipeline.failed)
        return false;
    if (!g_pHyprRenderer)
        return false;

    g_pHyprOpenGL->makeEGLCurrent();

    static constexpr char kVertexSource[] = R"(#version 320 es
precision highp float;
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

    static constexpr char kFragmentSource[] = R"(#version 320 es
precision highp float;
in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform vec2 uDirection;
uniform float uRadius;
void main() {
    vec2 stepVec = uTexelSize * uDirection * uRadius;
    vec4 color = texture(uTexture, vTexCoord) * 0.2270270270;
    color += texture(uTexture, vTexCoord + stepVec * 1.3846153846) * 0.3162162162;
    color += texture(uTexture, vTexCoord - stepVec * 1.3846153846) * 0.3162162162;
    color += texture(uTexture, vTexCoord + stepVec * 3.2307692308) * 0.0702702703;
    color += texture(uTexture, vTexCoord - stepVec * 3.2307692308) * 0.0702702703;
    fragColor = color;
}
)";

    pipeline.vertexShader = compileShaderStage(GL_VERTEX_SHADER, kVertexSource);
    pipeline.fragmentShader = compileShaderStage(GL_FRAGMENT_SHADER, kFragmentSource);
    if (!pipeline.vertexShader || !pipeline.fragmentShader) {
        pipeline.failed = true;
        return false;
    }

    pipeline.program = glCreateProgram();
    glAttachShader(pipeline.program, pipeline.vertexShader);
    glAttachShader(pipeline.program, pipeline.fragmentShader);
    glLinkProgram(pipeline.program);

    GLint linked = GL_FALSE;
    glGetProgramiv(pipeline.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint infoLogLength = 0;
        glGetProgramiv(pipeline.program, GL_INFO_LOG_LENGTH, &infoLogLength);
        std::string infoLog(std::max(1, infoLogLength), '\0');
        glGetProgramInfoLog(pipeline.program, infoLogLength, nullptr, infoLog.data());
        if (Log::logger)
            Log::logger->log(Log::ERR, "[hymission] gaussian blur shader link failed: " + infoLog);
        glDeleteProgram(pipeline.program);
        pipeline.program = 0;
        pipeline.failed = true;
        return false;
    }

    static constexpr std::array<float, 16> kQuadVertices = {
        -1.0F, -1.0F, 0.0F, 0.0F,
         1.0F, -1.0F, 1.0F, 0.0F,
        -1.0F,  1.0F, 0.0F, 1.0F,
         1.0F,  1.0F, 1.0F, 1.0F,
    };

    glGenVertexArrays(1, &pipeline.vao);
    glGenBuffers(1, &pipeline.vbo);
    glBindVertexArray(pipeline.vao);
    glBindBuffer(GL_ARRAY_BUFFER, pipeline.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVertices), kQuadVertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    pipeline.textureLocation = glGetUniformLocation(pipeline.program, "uTexture");
    pipeline.texelSizeLocation = glGetUniformLocation(pipeline.program, "uTexelSize");
    pipeline.directionLocation = glGetUniformLocation(pipeline.program, "uDirection");
    pipeline.radiusLocation = glGetUniformLocation(pipeline.program, "uRadius");
    pipeline.ready = pipeline.textureLocation >= 0 && pipeline.texelSizeLocation >= 0 && pipeline.directionLocation >= 0 && pipeline.radiusLocation >= 0;
    pipeline.failed = !pipeline.ready;
    return pipeline.ready;
}

void destroyGaussianBlurPipeline() {
    auto& pipeline = gaussianBlurPipeline();
    if (!pipeline.ready && !pipeline.failed && pipeline.program == 0 && pipeline.vao == 0 && pipeline.vbo == 0 && pipeline.vertexShader == 0 && pipeline.fragmentShader == 0)
        return;

    if (g_pHyprRenderer)
        g_pHyprOpenGL->makeEGLCurrent();

    if (pipeline.vbo)
        glDeleteBuffers(1, &pipeline.vbo);
    if (pipeline.vao)
        glDeleteVertexArrays(1, &pipeline.vao);
    if (pipeline.program)
        glDeleteProgram(pipeline.program);
    if (pipeline.vertexShader)
        glDeleteShader(pipeline.vertexShader);
    if (pipeline.fragmentShader)
        glDeleteShader(pipeline.fragmentShader);
    pipeline = {};
}

bool renderGaussianBlurPass(Render::IFramebuffer& sourceFramebuffer, Render::IFramebuffer& targetFramebuffer, const Vector2D& direction, float radius) {
    if (!ensureGaussianBlurPipeline() || !sourceFramebuffer.isAllocated() || !targetFramebuffer.isAllocated())
        return false;
    const auto targetFramebufferId = framebufferId(targetFramebuffer);
    if (!targetFramebufferId)
        return false;

    auto texture = sourceFramebuffer.getTexture();
    if (!texture)
        return false;

    setTextureLinearFiltering(texture);
    setFramebufferLinearFiltering(targetFramebuffer);

    GLint previousFramebuffer = 0;
    GLint previousProgram = 0;
    GLint previousVAO = 0;
    GLint previousArrayBuffer = 0;
    GLint previousActiveTexture = 0;
    GLint previousTexture0 = 0;
    GLint previousViewport[4] = {};
    const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
    glGetIntegerv(GL_VIEWPORT, previousViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, *targetFramebufferId);
    glViewport(0, 0, static_cast<GLsizei>(std::lround(targetFramebuffer.m_size.x)), static_cast<GLsizei>(std::lround(targetFramebuffer.m_size.y)));
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    const auto& pipeline = gaussianBlurPipeline();
    glUseProgram(pipeline.program);
    glBindVertexArray(pipeline.vao);
    texture->bind();
    glUniform1i(pipeline.textureLocation, 0);
    glUniform2f(pipeline.texelSizeLocation, 1.0F / static_cast<float>(std::max(1.0, sourceFramebuffer.m_size.x)),
                1.0F / static_cast<float>(std::max(1.0, sourceFramebuffer.m_size.y)));
    glUniform2f(pipeline.directionLocation, static_cast<float>(direction.x), static_cast<float>(direction.y));
    glUniform1f(pipeline.radiusLocation, radius);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    texture->unbind();

    glBindTexture(GL_TEXTURE_2D, previousTexture0);
    glActiveTexture(previousActiveTexture);
    glBindBuffer(GL_ARRAY_BUFFER, previousArrayBuffer);
    glBindVertexArray(previousVAO);
    glUseProgram(previousProgram);
    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    if (blendEnabled)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    if (scissorEnabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    return true;
}

bool buildBlurredProxyFramebuffers(const SP<Render::IFramebuffer>& sourceFramebuffer, const std::array<SP<Render::IFramebuffer>, 4>& blurredFramebuffers) {
    if (!g_pHyprRenderer || !sourceFramebuffer || !sourceFramebuffer->isAllocated())
        return false;

    g_pHyprOpenGL->makeEGLCurrent();

    const int framebufferWidth = static_cast<int>(std::lround(sourceFramebuffer->m_size.x));
    const int framebufferHeight = static_cast<int>(std::lround(sourceFramebuffer->m_size.y));
    auto      horizontalFramebuffer = createFramebuffer("hymission hidden strip horizontal blur");
    auto      verticalFramebuffer = createFramebuffer("hymission hidden strip vertical blur");
    if (!horizontalFramebuffer || !verticalFramebuffer || !horizontalFramebuffer->alloc(framebufferWidth, framebufferHeight) || !verticalFramebuffer->alloc(framebufferWidth, framebufferHeight))
        return false;
    setFramebufferLinearFiltering(*horizontalFramebuffer);
    setFramebufferLinearFiltering(*verticalFramebuffer);

    constexpr std::array<int, 4> kBlurIterations = {1, 3, 6, 10};
    constexpr float              kGaussianStepRadius = 1.35F;
    std::size_t                  blurLevelIndex = 0;
    int                          completedIterations = 0;
    Render::IFramebuffer*        currentSource = sourceFramebuffer.get();

    while (blurLevelIndex < blurredFramebuffers.size()) {
        if (!blurredFramebuffers[blurLevelIndex] || !blurredFramebuffers[blurLevelIndex]->isAllocated())
            return false;
        setFramebufferLinearFiltering(*blurredFramebuffers[blurLevelIndex]);

        if (!renderGaussianBlurPass(*currentSource, *horizontalFramebuffer, Vector2D{1.0, 0.0}, kGaussianStepRadius))
            return false;
        if (!renderGaussianBlurPass(*horizontalFramebuffer, *verticalFramebuffer, Vector2D{0.0, 1.0}, kGaussianStepRadius))
            return false;

        ++completedIterations;
        currentSource = verticalFramebuffer.get();

        if (completedIterations < kBlurIterations[blurLevelIndex])
            continue;

        if (!blitFramebufferRegion(*currentSource, *blurredFramebuffers[blurLevelIndex], makeRect(0.0, 0.0, currentSource->m_size.x, currentSource->m_size.y),
                                   makeRect(0.0, 0.0, blurredFramebuffers[blurLevelIndex]->m_size.x, blurredFramebuffers[blurLevelIndex]->m_size.y)))
            return false;

        ++blurLevelIndex;
    }

    return true;
}

Rect scaleRectFromAnchor(const Rect& rect, const Rect& contentRect, WorkspaceStripAnchor anchor, double scaleX, double scaleY) {
    double anchorX = contentRect.x + contentRect.width * 0.5;
    double anchorY = contentRect.y + contentRect.height * 0.5;

    switch (anchor) {
        case WorkspaceStripAnchor::Left:
            anchorX = contentRect.x;
            break;
        case WorkspaceStripAnchor::Right:
            anchorX = contentRect.x + contentRect.width;
            break;
        case WorkspaceStripAnchor::Top:
        default:
            anchorY = contentRect.y;
            break;
    }

    const double width = rect.width * scaleX;
    const double height = rect.height * scaleY;
    return makeRect(anchorX - (anchorX - rect.x) * scaleX, anchorY - (anchorY - rect.y) * scaleY, width, height);
}

double scaleLengthForRender(const PHLMONITOR& monitor, double logicalLength) {
    return logicalLength * renderScaleForMonitor(monitor);
}

int scaleFontSizeForRender(const PHLMONITOR& monitor, int logicalSize) {
    return std::max(1, static_cast<int>(std::lround(scaleLengthForRender(monitor, logicalSize))));
}

void expandRenderDamageToFullMonitor(const PHLMONITOR& monitor) {
    if (!monitor)
        return;

    CRegion fullMonitorDamage{0.0, 0.0, monitor->m_transformedSize.x, monitor->m_transformedSize.y};
    CRegion damage = g_pHyprRenderer->m_renderData.damage.copy();
    damage.add(fullMonitorDamage);
    CRegion finalDamage = g_pHyprRenderer->m_renderData.finalDamage.copy();
    finalDamage.add(fullMonitorDamage);
    g_pHyprRenderer->m_renderData.damage = damage;
    g_pHyprRenderer->m_renderData.finalDamage = finalDamage;
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

Rect surfaceRenderGlobalRectForWindow(const PHLWINDOW& window) {
    if (!window)
        return {};

    return renderGlobalRectForWindow(window);
}

Rect snapRectToRenderPixelGrid(const Rect& rect, const PHLMONITOR& monitor) {
    if (!monitor)
        return rect;

    const double renderScale = std::max(0.0001, renderScaleForMonitor(monitor));
    const double minLogicalStep = 1.0 / renderScale;
    const Rect   local = rectToMonitorLocal(rect, monitor);

    const auto snap = [renderScale](double value) {
        return std::round(value * renderScale) / renderScale;
    };

    const double snappedCenterX = snap(local.centerX());
    const double snappedCenterY = snap(local.centerY());
    const double snappedWidth = std::max(minLogicalStep, snap(local.width));
    const double snappedHeight = std::max(minLogicalStep, snap(local.height));

    return makeRect(monitor->m_position.x + snappedCenterX - snappedWidth * 0.5, monitor->m_position.y + snappedCenterY - snappedHeight * 0.5, snappedWidth,
                    snappedHeight);
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

Rect centeredSurfaceRectInLayoutBox(const CBox& layoutBox, const Rect& surfaceGlobal) {
    const double width = surfaceGlobal.width > 1.0 ? surfaceGlobal.width : layoutBox.width;
    const double height = surfaceGlobal.height > 1.0 ? surfaceGlobal.height : layoutBox.height;
    return makeRect(layoutBox.x + (layoutBox.width - width) * 0.5, layoutBox.y + (layoutBox.height - height) * 0.5, width, height);
}

template <typename TargetPtr>
CBox liveScrollingLayoutBoxForTarget(const TargetPtr& target, const CBox& snapshotBox) {
    if (!target)
        return snapshotBox;

    const CBox liveBox = target->position();
    if (liveBox.width > 1.0 && liveBox.height > 1.0)
        return liveBox;

    return snapshotBox;
}



bool focusFit0NativeEdgeCameraActive(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space)
        return false;

    auto* const scrolling = scrollingAlgorithmForWorkspace(workspace);
    if (!scrolling || !scrolling->m_scrollingData || !scrolling->m_scrollingData->controller)
        return false;

    auto* const controller = scrolling->m_scrollingData->controller.get();
    const CBox usable = scrolling->usableArea();
    const bool fullscreenOnOne = getConfigInt(nullptr, "scrolling:fullscreen_on_one_column", 1) != 0;
    const double viewportLength = controller->isPrimaryHorizontal() ? static_cast<double>(usable.w) : static_cast<double>(usable.h);
    const double maxExtent = controller->calculateMaxExtent(usable, fullscreenOnOne);
    const double maxNormalOffset = std::max(0.0, maxExtent - std::max(1.0, viewportLength));
    const double offset = controller->getOffset();

    return offset < -0.5 || offset > maxNormalOffset + 0.5;
}

PHLWINDOW centeredFocusFit0WindowForScrollingWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space)
        return {};

    auto* const scrolling = scrollingAlgorithmForWorkspace(workspace);
    if (!scrolling || !scrolling->m_scrollingData || !scrolling->m_scrollingData->controller)
        return {};

    const CBox usable = scrolling->usableArea();
    Rect viewport = makeRect(usable.x, usable.y, usable.width, usable.height);
    if (viewport.width <= 1.0 || viewport.height <= 1.0) {
        const CBox workAreaBox = workspace->m_space->workArea();
        viewport = makeRect(workAreaBox.x, workAreaBox.y, workAreaBox.width, workAreaBox.height);
    }
    if (viewport.width <= 1.0 || viewport.height <= 1.0)
        return {};

    auto* const controller = scrolling->m_scrollingData->controller.get();
    const bool horizontal = controller->isPrimaryHorizontal();
    const double nativeOffset = controller->getOffset();

    const auto rectPrimaryStart = [&](const Rect& rect) { return horizontal ? rect.x : rect.y; };
    const auto rectPrimarySize = [&](const Rect& rect) { return horizontal ? rect.width : rect.height; };
    const auto rectPrimaryCenter = [&](const Rect& rect) { return rectPrimaryStart(rect) + rectPrimarySize(rect) * 0.5; };
    const auto rectSecondaryCenter = [&](const Rect& rect) { return horizontal ? rect.centerY() : rect.centerX(); };
    const auto shiftPrimary = [&](Rect rect, double delta) {
        if (horizontal)
            rect.x += delta;
        else
            rect.y += delta;
        return rect;
    };

    const double viewportStart = rectPrimaryStart(viewport);
    const double viewportEnd = viewportStart + rectPrimarySize(viewport);
    const double viewportCenter = (viewportStart + viewportEnd) * 0.5;
    const double secondaryViewportCenter = rectSecondaryCenter(viewport);

    const auto primaryOverlap = [&](const Rect& rect) {
        const double start = rectPrimaryStart(rect);
        const double end = start + rectPrimarySize(rect);
        return std::max(0.0, std::min(end, viewportEnd) - std::max(start, viewportStart));
    };

    const auto mergeBounds = [](std::optional<Rect>& bounds, const Rect& rect) {
        if (rect.width <= 1.0 || rect.height <= 1.0)
            return;
        if (!bounds) {
            bounds = rect;
            return;
        }

        const double minX = std::min(bounds->x, rect.x);
        const double minY = std::min(bounds->y, rect.y);
        const double maxX = std::max(bounds->x + bounds->width, rect.x + rect.width);
        const double maxY = std::max(bounds->y + bounds->height, rect.y + rect.height);
        bounds = makeRect(minX, minY, maxX - minX, maxY - minY);
    };

    struct Candidate {
        PHLWINDOW window;
        Rect      rect;
        double    score = std::numeric_limits<double>::infinity();
        bool      centerInside = false;
        bool      centerAligned = false;
        double    overlap = 0.0;
        double    centerDistance = std::numeric_limits<double>::infinity();
        std::size_t columnIndex = 0;
    };

    Candidate best;

    for (std::size_t columnIndex = 0; columnIndex < scrolling->m_scrollingData->columns.size(); ++columnIndex) {
        const auto& column = scrolling->m_scrollingData->columns[columnIndex];
        if (!column || column->targetDatas.empty())
            continue;

        PHLWINDOW columnFocusWindow;
        if (const auto lastFocusedTarget = column->lastFocusedTarget.lock()) {
            const auto target = lastFocusedTarget->target.lock();
            const auto window = target ? target->window() : PHLWINDOW{};
            if (window && window->m_isMapped && !window->m_fadingOut && !window->m_pinned && !window->onSpecialWorkspace() &&
                window->m_workspace == workspace && target && !target->floating())
                columnFocusWindow = window;
        }

        std::optional<Rect> columnBounds;
        PHLWINDOW fallbackWindow;
        double fallbackSecondaryDistance = std::numeric_limits<double>::infinity();

        for (const auto& targetData : column->targetDatas) {
            if (!targetData || !targetData->target)
                continue;

            const auto target = targetData->target.lock();
            const auto window = target ? target->window() : PHLWINDOW{};
            if (!window || !window->m_isMapped || window->m_fadingOut || window->m_pinned || window->onSpecialWorkspace() || window->m_workspace != workspace ||
                !target || target->floating())
                continue;

            if (targetData->layoutBox.width <= 1.0 || targetData->layoutBox.height <= 1.0)
                continue;

            // Use the scrolling model's stable layout boxes, translated through
            // the current native camera offset.  Live target/window positions can
            // still belong to the previous focus/camera state while the overview
            // is building the target workspace transition; using them here is what
            // made focus_fit_method=0 sometimes pick the neighboring 0.5 column
            // and then snap the strip when the sync path recentered that wrong
            // column.
            const Rect layoutRect = makeRect(targetData->layoutBox.x, targetData->layoutBox.y, targetData->layoutBox.width, targetData->layoutBox.height);
            const Rect cameraRect = shiftPrimary(layoutRect, -nativeOffset);
            mergeBounds(columnBounds, cameraRect);

            const double secondaryDistance = std::abs(rectSecondaryCenter(cameraRect) - secondaryViewportCenter);
            if (secondaryDistance < fallbackSecondaryDistance) {
                fallbackSecondaryDistance = secondaryDistance;
                fallbackWindow = window;
            }
        }

        const PHLWINDOW focusWindow = columnFocusWindow ? columnFocusWindow : fallbackWindow;
        if (!focusWindow || !columnBounds || columnBounds->width <= 1.0 || columnBounds->height <= 1.0)
            continue;

        const double overlap = primaryOverlap(*columnBounds);
        if (overlap <= 0.5)
            continue;

        const double start = rectPrimaryStart(*columnBounds);
        const double end = start + rectPrimarySize(*columnBounds);
        const bool centerInside = viewportCenter >= start - 0.5 && viewportCenter <= end + 0.5;
        const double centerDistance = std::abs(rectPrimaryCenter(*columnBounds) - viewportCenter);
        const double secondaryDistance = std::abs(rectSecondaryCenter(*columnBounds) - secondaryViewportCenter);
        const double centerAlignmentTolerance = std::clamp(rectPrimarySize(*columnBounds) * 0.08, 16.0, 96.0);
        const bool centerAligned = centerInside && centerDistance <= centerAlignmentTolerance;

        // First decide by the native camera geometry, not by MRU/last focus.
        // A centered partial column should always win over a merely visible
        // neighboring partial.  Last-focused target is used only after the column
        // has been selected, to choose a tile inside that selected column.
        const double outsideCenterPenalty = centerInside ? 0.0 : 100000.0;
        const double misalignedCenterPenalty = centerAligned ? 0.0 : 50000.0;
        const double score = outsideCenterPenalty + misalignedCenterPenalty + centerDistance + secondaryDistance * 0.01 - std::min(overlap, 1024.0) * 0.001;
        if (!best.window || score < best.score) {
            best = Candidate{
                .window = focusWindow,
                .rect = *columnBounds,
                .score = score,
                .centerInside = centerInside,
                .centerAligned = centerAligned,
                .overlap = overlap,
                .centerDistance = centerDistance,
                .columnIndex = columnIndex,
            };
        }
    }

    // The distinction that matters for focus_fit_method=0 is whether the native
    // viewport center is inside a real tiled column.  A centered 0.5 column can
    // have a small alignment mismatch while the target workspace is being built,
    // but the center is still inside that column and it should receive focus.
    // A scroll-past edge-camera state has no column under the viewport center;
    // preserve that focusless camera instead of falling back to a visible edge
    // leaf and snapping the strip back.
    return best.centerInside ? best.window : PHLWINDOW{};
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

struct ScrollingOverviewGeometry {
    Rect        sourceGlobal;
    Rect        anchorGlobal;
    Rect        baseGlobal;
    GestureAxis primaryAxis = GestureAxis::Horizontal;
};

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
            if (!candidate || !candidate->target || candidate->layoutBox.width <= 1.0 || candidate->layoutBox.height <= 1.0)
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
            if (!candidate)
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

    if (!fitFocusMethod) {
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
        if (!candidate || !candidate->target || candidate->layoutBox.width <= 1.0 || candidate->layoutBox.height <= 1.0)
            continue;

        if (candidate != targetData)
            continue;

        const CBox candidateLayoutBox = liveScrollingLayoutBoxForTarget(candidate->target, candidate->layoutBox);
        Rect virtualCandidateLayoutBox = makeRect(candidateLayoutBox.x, candidateLayoutBox.y, candidateLayoutBox.width, candidateLayoutBox.height);
        const double candidateOffset = primaryStart(virtualCandidateLayoutBox) - primaryStart(targetColumn.bounds);
        setPrimaryStart(virtualCandidateLayoutBox, primaryStart(targetColumn.virtualBounds) + candidateOffset);

        CBox virtualBox{virtualCandidateLayoutBox.x, virtualCandidateLayoutBox.y, virtualCandidateLayoutBox.width, virtualCandidateLayoutBox.height};
        targetSource = centeredSurfaceRectInLayoutBox(virtualBox, fallbackGlobal);

        const double anchorX = targetColumn.virtualBounds.centerX();
        const double anchorY = targetColumn.virtualBounds.centerY();
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


std::string vectorToString(const Vector2D& value) {
    std::ostringstream out;
    out << value.x << ',' << value.y;
    return out.str();
}

std::string boxToString(const CBox& box) {
    std::ostringstream out;
    out << box.x << ',' << box.y << ' ' << box.width << 'x' << box.height;
    return out.str();
}

std::string rectToString(const Rect& rect) {
    std::ostringstream out;
    out << rect.x << ',' << rect.y << ' ' << rect.width << 'x' << rect.height;
    return out.str();
}

std::string trimCopy(std::string value) {
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) { return !isSpace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) { return !isSpace(ch); }).base(), value.end());
    return value;
}

std::vector<std::string> splitCommaTokens(const std::string& value) {
    std::vector<std::string> tokens;
    std::string              current;
    std::istringstream       stream(value);
    while (std::getline(stream, current, ','))
        tokens.push_back(trimCopy(current));
    return tokens;
}

std::string joinTokens(const std::vector<std::string>& tokens, std::size_t beginIndex) {
    std::ostringstream out;
    for (std::size_t i = beginIndex; i < tokens.size(); ++i) {
        if (i != beginIndex)
            out << ',';
        out << tokens[i];
    }
    return out.str();
}

double normalizedGestureDelta(const IPointer::SSwipeUpdateEvent& event, eTrackpadGestureDirection direction, float deltaScale, bool invertVertical) {
    const double baseDelta = direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? -static_cast<double>(event.delta.x) : -static_cast<double>(event.delta.y);
    const double scaled = baseDelta * static_cast<double>(deltaScale);
    return invertVertical ? -scaled : scaled;
}

class CHymissionTrackpadGesture final : public ITrackpadGesture {
  public:
    CHymissionTrackpadGesture(GestureDispatcherKind dispatcher, OverviewController::ScopeOverride scope, bool recommand, eTrackpadGestureDirection direction,
                              float deltaScale)
        : m_dispatcher(dispatcher), m_scope(scope), m_recommand(recommand), m_direction(direction), m_deltaScale(deltaScale) {
    }

    void begin(const STrackpadGestureBegin& e) override {
        m_tracking = false;
        if (!g_controller || !e.swipe ||
            !g_controller->beginTrackpadGesture(m_dispatcher == GestureDispatcherKind::Open, m_scope, m_recommand, m_direction, *e.swipe, m_deltaScale))
            return;

        m_tracking = true;
    }

    void update(const STrackpadGestureUpdate& e) override {
        if (!m_tracking || !g_controller || !e.swipe)
            return;

        g_controller->updateTrackpadGesture(*e.swipe);
    }

    void end(const STrackpadGestureEnd& e) override {
        if (!m_tracking || !g_controller)
            return;

        m_tracking = false;
        g_controller->endTrackpadGesture(e.swipe ? e.swipe->cancelled : true);
    }

  private:
    GestureDispatcherKind             m_dispatcher;
    OverviewController::ScopeOverride m_scope = OverviewController::ScopeOverride::Default;
    bool                              m_recommand = false;
    eTrackpadGestureDirection         m_direction = TRACKPAD_GESTURE_DIR_VERTICAL;
    float                             m_deltaScale = 1.0F;
    bool                              m_tracking = false;
};

class CHymissionWorkspaceTrackpadGesture final : public ITrackpadGesture {
  public:
    CHymissionWorkspaceTrackpadGesture(eTrackpadGestureDirection direction, float)
        : m_direction(direction) {
    }

    void begin(const STrackpadGestureBegin& e) override {
        m_mode = Mode::Native;

        if (!g_controller || !e.swipe) {
            m_nativeGesture.begin(e);
            return;
        }

        if (g_controller->blocksWorkspaceSwitchInOverviewForGestures()) {
            m_mode = Mode::Blocked;
            return;
        }

        if (g_controller->allowsWorkspaceSwitchInOverviewForGestures()) {
            const auto configuredDirection = e.direction != TRACKPAD_GESTURE_DIR_NONE ? e.direction : m_direction;
            if (!g_controller->beginOverviewWorkspaceSwipeGesture(configuredDirection))
                return;

            ITrackpadGesture::begin(e);
            m_mode = Mode::Overview;
            g_controller->updateOverviewWorkspaceSwipeGesture(distance(e));
            return;
        }

        m_nativeGesture.begin(e);
    }

    void update(const STrackpadGestureUpdate& e) override {
        if (m_mode == Mode::Blocked || !e.swipe)
            return;

        if (m_mode == Mode::Overview) {
            if (g_controller)
                g_controller->updateOverviewWorkspaceSwipeGesture(distance(e));
            return;
        }

        m_nativeGesture.update(e);
    }

    void end(const STrackpadGestureEnd& e) override {
        if (m_mode == Mode::Blocked)
            return;

        if (m_mode == Mode::Overview) {
            if (g_controller)
                g_controller->endOverviewWorkspaceSwipeGesture(e.swipe ? e.swipe->cancelled : true);
            return;
        }

        m_nativeGesture.end(e);
    }

    bool isDirectionSensitive() override {
        return true;
    }

  private:
    enum class Mode {
        Native,
        Overview,
        Blocked,
    };

    CWorkspaceSwipeGesture   m_nativeGesture;
    eTrackpadGestureDirection m_direction = TRACKPAD_GESTURE_DIR_HORIZONTAL;
    Mode                      m_mode = Mode::Native;
};

class CHymissionScrollTrackpadGesture final : public ITrackpadGesture {
  public:
    CHymissionScrollTrackpadGesture(HymissionScrollMode mode, eTrackpadGestureDirection direction, float)
        : m_mode(mode), m_direction(direction) {
    }

    void begin(const STrackpadGestureBegin& e) override {
        m_tracking = false;

        const auto gestureDirection = e.direction != TRACKPAD_GESTURE_DIR_NONE ? e.direction : m_direction;
        if (g_controller && e.swipe && g_controller->beginScrollGesture(m_mode, gestureDirection, *e.swipe, e.scale)) {
            m_tracking = true;
            return;
        }
    }

    void update(const STrackpadGestureUpdate& e) override {
        if (m_tracking) {
            if (g_controller && e.swipe)
                g_controller->updateScrollGesture(*e.swipe);
            return;
        }
    }

    void end(const STrackpadGestureEnd& e) override {
        if (m_tracking) {
            m_tracking = false;
            if (g_controller)
                g_controller->endScrollGesture(e.swipe ? e.swipe->cancelled : true);
            return;
        }
    }

    bool isDirectionSensitive() override {
        return true;
    }

  private:
    HymissionScrollMode      m_mode = HymissionScrollMode::Layout;
    eTrackpadGestureDirection m_direction = TRACKPAD_GESTURE_DIR_HORIZONTAL;
    bool                     m_tracking = false;
};

template <typename T>
bool containsHandle(const std::vector<T>& values, const T& value) {
    return std::ranges::find(values, value) != values.end();
}

bool rectApproxEqual(const Rect& lhs, const Rect& rhs, double epsilon) {
    return std::abs(lhs.x - rhs.x) <= epsilon && std::abs(lhs.y - rhs.y) <= epsilon && std::abs(lhs.width - rhs.width) <= epsilon &&
        std::abs(lhs.height - rhs.height) <= epsilon;
}

bool rectContainsPoint(const Rect& rect, double x, double y) {
    return x >= rect.x && y >= rect.y && x <= rect.x + rect.width && y <= rect.y + rect.height;
}

double rectCenterDistanceSquared(const Rect& rect, double x, double y) {
    const double dx = rect.centerX() - x;
    const double dy = rect.centerY() - y;
    return dx * dx + dy * dy;
}


bool usableOverviewRect(const Rect& rect) {
    return rect.width > 1.0 && rect.height > 1.0;
}

Rect unionOverviewRect(const Rect& lhs, const Rect& rhs) {
    if (!usableOverviewRect(lhs))
        return rhs;
    const double x1 = std::min(lhs.x, rhs.x);
    const double y1 = std::min(lhs.y, rhs.y);
    const double x2 = std::max(lhs.x + lhs.width, rhs.x + rhs.width);
    const double y2 = std::max(lhs.y + lhs.height, rhs.y + rhs.height);
    return makeRect(x1, y1, x2 - x1, y2 - y1);
}

template <typename WorkspaceStripEntryLike>
std::optional<Rect> stripWindowPreviewRectForHitTest(const WorkspaceStripEntryLike& entry, const Rect& stripRect, const PHLWINDOW& window) {
    if (!window || !usableOverviewRect(stripRect))
        return std::nullopt;

    const auto targetPreview = std::find_if(entry.windows.begin(), entry.windows.end(), [&](const auto& preview) {
        return preview.window == window && usableOverviewRect(preview.naturalGlobal);
    });
    if (targetPreview == entry.windows.end())
        return std::nullopt;

    Rect sourceBounds{};
    bool hasSourceBounds = false;
    for (const auto& preview : entry.windows) {
        if (!preview.window || preview.window->m_pinned || !usableOverviewRect(preview.naturalGlobal))
            continue;

        sourceBounds = hasSourceBounds ? unionOverviewRect(sourceBounds, preview.naturalGlobal) : preview.naturalGlobal;
        hasSourceBounds = true;
    }

    if (!hasSourceBounds)
        sourceBounds = targetPreview->naturalGlobal;
    if (!usableOverviewRect(sourceBounds))
        return std::nullopt;

    const double padding = std::clamp(std::min(stripRect.width, stripRect.height) * 0.045, 2.0, 10.0);
    const Rect inner = makeRect(stripRect.x + padding, stripRect.y + padding,
                                std::max(1.0, stripRect.width - padding * 2.0),
                                std::max(1.0, stripRect.height - padding * 2.0));
    const double scale = std::min(inner.width / std::max(1.0, sourceBounds.width), inner.height / std::max(1.0, sourceBounds.height));
    if (scale <= 0.0)
        return std::nullopt;

    const Rect& natural = targetPreview->naturalGlobal;
    const double centerX = inner.centerX() + (natural.centerX() - sourceBounds.centerX()) * scale;
    const double centerY = inner.centerY() + (natural.centerY() - sourceBounds.centerY()) * scale;
    return makeRect(centerX - natural.width * scale * 0.5,
                    centerY - natural.height * scale * 0.5,
                    std::max(1.0, natural.width * scale),
                    std::max(1.0, natural.height * scale));
}

std::optional<Vector2D> visiblePointForRectOnMonitor(const Rect& windowRect, const PHLMONITOR& monitor) {
    if (!monitor)
        return std::nullopt;

    const Rect monitorRect = makeRect(monitor->m_position.x, monitor->m_position.y, monitor->m_size.x, monitor->m_size.y);
    const double left = std::max(windowRect.x, monitorRect.x);
    const double top = std::max(windowRect.y, monitorRect.y);
    const double right = std::min(windowRect.x + windowRect.width, monitorRect.x + monitorRect.width);
    const double bottom = std::min(windowRect.y + windowRect.height, monitorRect.y + monitorRect.height);
    if (right <= left || bottom <= top)
        return std::nullopt;

    return Vector2D((left + right) * 0.5, (top + bottom) * 0.5);
}

bool rectHasVisibleOverlap(const Rect& lhs, const Rect& rhs, double minimumOverlap = 1.0) {
    const double overlapWidth = std::min(lhs.x + lhs.width, rhs.x + rhs.width) - std::max(lhs.x, rhs.x);
    const double overlapHeight = std::min(lhs.y + lhs.height, rhs.y + rhs.height) - std::max(lhs.y, rhs.y);
    return overlapWidth > minimumOverlap && overlapHeight > minimumOverlap;
}

bool scrollingWindowIntersectsNativeViewport(const PHLWINDOW& window) {
    if (!window || !window->m_workspace || !window->m_workspace->m_space)
        return false;

    const auto target = window->layoutTarget();
    if (!target || target->floating())
        return false;

    auto* const scrolling = scrollingAlgorithmForWorkspace(window->m_workspace);
    if (!scrolling || !scrolling->m_scrollingData || !scrolling->m_scrollingData->controller)
        return false;

    CBox targetBox = target->position();
    if (targetBox.width <= 1.0 || targetBox.height <= 1.0) {
        if (const auto targetData = scrolling->dataFor(target); targetData && targetData->layoutBox.width > 1.0 && targetData->layoutBox.height > 1.0)
            targetBox = liveScrollingLayoutBoxForTarget(targetData->target, targetData->layoutBox);
    }
    if (targetBox.width <= 1.0 || targetBox.height <= 1.0)
        return false;

    CBox viewportBox = window->m_workspace->m_space->workArea();
    if (viewportBox.width <= 1.0 || viewportBox.height <= 1.0)
        viewportBox = scrolling->usableArea();
    if (viewportBox.width <= 1.0 || viewportBox.height <= 1.0)
        return false;

    return rectHasVisibleOverlap(makeRect(targetBox.x, targetBox.y, targetBox.width, targetBox.height),
                                 makeRect(viewportBox.x, viewportBox.y, viewportBox.width, viewportBox.height));
}

std::size_t scrollingWorkspaceColumnCount(const PHLWORKSPACE& workspace) {
    if (!workspace)
        return 0;

    auto* const scrolling = scrollingAlgorithmForWorkspace(workspace);
    return scrolling && scrolling->m_scrollingData ? scrolling->m_scrollingData->columns.size() : 0;
}

bool scrollingWorkspaceHasSingleColumn(const PHLWORKSPACE& workspace) {
    return scrollingWorkspaceColumnCount(workspace) == 1;
}

std::optional<Vector2D> expectedSurfaceSizeForUV(const PHLWINDOW& window, const SP<CWLSurfaceResource>& surface, const PHLMONITOR& monitor, bool main) {
    if (!surface || !monitor)
        return std::nullopt;

    const bool canUseWindow = window && main;
    const bool windowSizeMisalign = canUseWindow && window->getReportedSize() != window->wlSurface()->resource()->m_current.size;

    if (surface->m_current.viewport.hasDestination)
        return (surface->m_current.viewport.destination * monitor->m_scale).round();

    if (surface->m_current.viewport.hasSource)
        return (surface->m_current.viewport.source.size() * monitor->m_scale).round();

    if (!canUseWindow)
        return (surface->m_current.size * monitor->m_scale).round();

    if (windowSizeMisalign)
        return (surface->m_current.size * monitor->m_scale).round();

    if (canUseWindow)
        return (window->getReportedSize() * monitor->m_scale).round();

    return std::nullopt;
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

void clearWindowFocusCompat(const PHLMONITOR& monitor) {
    Desktop::focusState()->resetWindowFocus();
    if (monitor)
        Desktop::focusState()->rawMonitorFocus(monitor);
    if (g_pSeatManager)
        g_pSeatManager->setKeyboardFocus(nullptr);
}

CSurfacePassElement::SRenderData* surfaceRenderDataMutable(void* surfacePassThisptr) {
    if (!surfacePassThisptr)
        return nullptr;

    auto* passElement = reinterpret_cast<IPassElement*>(surfacePassThisptr);
    if (passElement->type() != EK_SURFACE)
        return nullptr;

    return &reinterpret_cast<CSurfacePassElement*>(surfacePassThisptr)->m_data;
}

CBox hkSurfaceTexBox(void* surfacePassThisptr) {
    if (!g_controller)
        return {};

    return g_controller->surfaceTexBoxHook(surfacePassThisptr);
}

std::optional<CBox> hkSurfaceBoundingBox(void* surfacePassThisptr) {
    if (!g_controller)
        return {};

    return g_controller->surfaceBoundingBoxHook(surfacePassThisptr);
}

CRegion hkSurfaceOpaqueRegion(void* surfacePassThisptr) {
    if (!g_controller)
        return {};

    return g_controller->surfaceOpaqueRegionHook(surfacePassThisptr);
}

CRegion hkSurfaceVisibleRegion(void* surfacePassThisptr, bool& cancel) {
    if (!g_controller)
        return {};

    return g_controller->surfaceVisibleRegionHook(surfacePassThisptr, cancel);
}

void hkBorderDraw(void* borderDecorationThisptr, PHLMONITOR monitor, const float& alpha) {
    if (!g_controller)
        return;

    g_controller->borderDrawHook(borderDecorationThisptr, monitor, alpha);
}

void hkShadowDraw(void* shadowDecorationThisptr, PHLMONITOR monitor, const float& alpha) {
    if (!g_controller)
        return;

    g_controller->shadowDrawHook(shadowDecorationThisptr, monitor, alpha);
}

void hkCalculateUVForSurface(void* rendererThisptr, PHLWINDOW window, SP<CWLSurfaceResource> surface, PHLMONITOR monitor, bool main, const Vector2D& projSize,
                             const Vector2D& projSizeUnscaled, bool fixMisalignedFSV1) {
    if (!g_controller)
        return;

    (void)rendererThisptr;
    g_controller->calculateUVForSurfaceHook(window, std::move(surface), monitor, main, projSize, projSizeUnscaled, fixMisalignedFSV1);
}

std::vector<UP<IPassElement>> hkSurfaceDraw(void* surfacePassThisptr) {
    if (!g_controller)
        return {};

    return g_controller->surfaceDrawHook(surfacePassThisptr);
}

bool hkSurfaceNeedsLiveBlur(void* surfacePassThisptr) {
    if (!g_controller)
        return false;

    return g_controller->surfaceNeedsLiveBlurHook(surfacePassThisptr);
}

bool hkSurfaceNeedsPrecomputeBlur(void* surfacePassThisptr) {
    if (!g_controller)
        return false;

    return g_controller->surfaceNeedsPrecomputeBlurHook(surfacePassThisptr);
}

bool hkShouldRenderWindow(void*, PHLWINDOW window, PHLMONITOR monitor) {
    if (!g_controller)
        return false;

    return g_controller->shouldRenderWindowHook(window, monitor);
}

void hkRenderLayer(void* rendererThisptr, PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& now, bool popups, bool lockscreen) {
    if (!g_controller)
        return;

    g_controller->renderLayerHook(rendererThisptr, layer, monitor, now, popups, lockscreen);
}

void hkWorkspaceSwipeBegin(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureBegin& e) {
    if (!g_controller)
        return;

    g_controller->workspaceSwipeBeginHook(gestureThisptr, e);
}

void hkWorkspaceSwipeUpdate(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (!g_controller)
        return;

    g_controller->workspaceSwipeUpdateHook(gestureThisptr, e);
}

void hkWorkspaceSwipeEnd(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (!g_controller)
        return;

    g_controller->workspaceSwipeEndHook(gestureThisptr, e);
}

Config::Actions::ActionResult hkMoveToWorkspaceAction(PHLWORKSPACE workspace, bool silent, std::optional<PHLWINDOW> window) {
    if (!g_controller)
        return {};

    return g_controller->moveToWorkspaceActionHook(std::move(workspace), silent, std::move(window));
}

void hkUnifiedWorkspaceSwipeBegin(void* gestureThisptr) {
    if (!g_controller)
        return;

    g_controller->unifiedWorkspaceSwipeBeginHook(gestureThisptr);
}

void hkUnifiedWorkspaceSwipeUpdate(void* gestureThisptr, double delta) {
    if (!g_controller)
        return;

    g_controller->unifiedWorkspaceSwipeUpdateHook(gestureThisptr, delta);
}

void hkUnifiedWorkspaceSwipeEnd(void* gestureThisptr) {
    if (!g_controller)
        return;

    g_controller->unifiedWorkspaceSwipeEndHook(gestureThisptr);
}

void hkScrollMoveGestureBegin(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureBegin& e) {
    if (!g_controller)
        return;

    g_controller->scrollMoveGestureBeginHook(gestureThisptr, e);
}

void hkScrollMoveGestureUpdate(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (!g_controller)
        return;

    g_controller->scrollMoveGestureUpdateHook(gestureThisptr, e);
}

void hkScrollMoveGestureEnd(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (!g_controller)
        return;

    g_controller->scrollMoveGestureEndHook(gestureThisptr, e);
}

std::optional<std::string> hkHandleGesture(void*, const std::string& keyword, const std::string& value) {
    if (!g_controller)
        return {};

    return g_controller->handleGestureConfigHook(keyword, value);
}

} // namespace

bool niri_scrolling_detail::isActiveController(const OverviewController* controller) {
    return g_controller == controller;
}

OverviewController::OverviewController(HANDLE handle) : m_handle(handle) {
    g_controller = this;
    g_openOverviewLayoutConfigSignatures[this] = layoutAffectingConfigSignature(m_handle);
}

OverviewController::~OverviewController() {
    g_openOverviewLayoutConfigSignatures.erase(this);
    setDamageTrackingOverride(false);
    destroyGaussianBlurPipeline();
    clearToggleSwitchReleasePollTimer();
    clearPostCloseCursorShapeResetTimer();
    clearPendingDeferredOpen();
    clearThemeSurfaceFeedbackTimer();
    clearThemeWorkspaceActivationRefresh();
    clearWorkspaceStripSnapshotRefreshTimer();
    clearNiriWallpaperLayoutLayerRefresh();
    clearRegisteredTrackpadGestures();
    clearPostCloseForcedFocus();
    clearPostCloseDispatcher();
    restoreWorkspaceNameOverrides();
    g_pHyprRenderer->m_directScanoutBlocked = false;
    setFullscreenRenderOverride(false);
    restoreOverviewRenderState();
    restoreWorkspaceTransitionRenderState();
    setInputFollowMouseOverride(false);
    setScrollingFollowFocusOverride(false);
    setAnimationsEnabledOverride(false);
    restoreWrappedDispatchers();
    deactivateHooks();
    if (m_workspaceSwipeBeginFunctionHook)
        m_workspaceSwipeBeginFunctionHook->unhook();
    if (m_workspaceSwipeUpdateFunctionHook)
        m_workspaceSwipeUpdateFunctionHook->unhook();
    if (m_workspaceSwipeEndFunctionHook)
        m_workspaceSwipeEndFunctionHook->unhook();
    if (m_unifiedWorkspaceSwipeBeginFunctionHook)
        m_unifiedWorkspaceSwipeBeginFunctionHook->unhook();
    if (m_unifiedWorkspaceSwipeUpdateFunctionHook)
        m_unifiedWorkspaceSwipeUpdateFunctionHook->unhook();
    if (m_unifiedWorkspaceSwipeEndFunctionHook)
        m_unifiedWorkspaceSwipeEndFunctionHook->unhook();
    if (m_scrollMoveGestureBeginFunctionHook)
        m_scrollMoveGestureBeginFunctionHook->unhook();
    if (m_scrollMoveGestureUpdateFunctionHook)
        m_scrollMoveGestureUpdateFunctionHook->unhook();
    if (m_scrollMoveGestureEndFunctionHook)
        m_scrollMoveGestureEndFunctionHook->unhook();
    if (m_moveToWorkspaceActionFunctionHook)
        m_moveToWorkspaceActionFunctionHook->unhook();
    if (g_layoutMessageActionHook)
        g_layoutMessageActionHook->unhook();
    if (g_moveFocusActionHook)
        g_moveFocusActionHook->unhook();
    if (g_moveInDirectionActionHook)
        g_moveInDirectionActionHook->unhook();
    if (g_swapInDirectionActionHook)
        g_swapInDirectionActionHook->unhook();
    if (g_resizeActionHook)
        g_resizeActionHook->unhook();

    if (m_surfaceTexBoxHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceTexBoxHook);
    if (m_surfaceBoundingBoxHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceBoundingBoxHook);
    if (m_surfaceOpaqueRegionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceOpaqueRegionHook);
    if (m_surfaceVisibleRegionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceVisibleRegionHook);
    if (m_surfaceDrawHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceDrawHook);
    if (m_surfaceNeedsLiveBlurHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceNeedsLiveBlurHook);
    if (m_surfaceNeedsPrecomputeBlurHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceNeedsPrecomputeBlurHook);
    if (m_shouldRenderWindowHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_shouldRenderWindowHook);
    if (m_borderDrawHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_borderDrawHook);
    if (m_shadowDrawHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_shadowDrawHook);
    if (m_calculateUVForSurfaceHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_calculateUVForSurfaceHook);
    if (m_workspaceSwipeBeginFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_workspaceSwipeBeginFunctionHook);
    if (m_workspaceSwipeUpdateFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_workspaceSwipeUpdateFunctionHook);
    if (m_workspaceSwipeEndFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_workspaceSwipeEndFunctionHook);
    if (m_unifiedWorkspaceSwipeBeginFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_unifiedWorkspaceSwipeBeginFunctionHook);
    if (m_unifiedWorkspaceSwipeUpdateFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_unifiedWorkspaceSwipeUpdateFunctionHook);
    if (m_unifiedWorkspaceSwipeEndFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_unifiedWorkspaceSwipeEndFunctionHook);
    if (m_scrollMoveGestureBeginFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_scrollMoveGestureBeginFunctionHook);
    if (m_scrollMoveGestureUpdateFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_scrollMoveGestureUpdateFunctionHook);
    if (m_scrollMoveGestureEndFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_scrollMoveGestureEndFunctionHook);
    if (m_moveToWorkspaceActionFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_moveToWorkspaceActionFunctionHook);
    if (g_layoutMessageActionHook) {
        HyprlandAPI::removeFunctionHook(m_handle, g_layoutMessageActionHook);
        g_layoutMessageActionHook = nullptr;
        g_layoutMessageActionOriginal = nullptr;
    }
    if (g_moveFocusActionHook) {
        HyprlandAPI::removeFunctionHook(m_handle, g_moveFocusActionHook);
        g_moveFocusActionHook = nullptr;
        g_moveFocusActionOriginal = nullptr;
    }
    if (g_moveInDirectionActionHook) {
        HyprlandAPI::removeFunctionHook(m_handle, g_moveInDirectionActionHook);
        g_moveInDirectionActionHook = nullptr;
        g_moveInDirectionActionOriginal = nullptr;
    }
    if (g_swapInDirectionActionHook) {
        HyprlandAPI::removeFunctionHook(m_handle, g_swapInDirectionActionHook);
        g_swapInDirectionActionHook = nullptr;
        g_swapInDirectionActionOriginal = nullptr;
    }
    if (g_resizeActionHook) {
        HyprlandAPI::removeFunctionHook(m_handle, g_resizeActionHook);
        g_resizeActionHook = nullptr;
        g_resizeActionOriginal = nullptr;
    }
    if (m_handleGestureHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_handleGestureHook);

    niri_scrolling_detail::overviewOpenInputBlockUntil = {};
    niri_scrolling_detail::overviewHeavyEditInputBlockUntil = {};
    g_controller = nullptr;
}

bool OverviewController::initialize() {
    if (!installHooks())
        return false;

    auto& events = Event::bus()->m_events;

    m_renderStageListener = events.render.stage.listen([this](eRenderStage stage) { renderStage(stage); });
    m_mouseMoveListener = events.input.mouse.move.listen([this](const Vector2D&, Event::SCallbackInfo&) {
        handleMouseMove();
    });
    m_mouseButtonListener = events.input.mouse.button.listen([this](const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
        // Copy the signal payload immediately; forwarding the raw listener arg
        // has produced corrupted button/state values on current Hyprland builds.
        const auto copiedEvent = event;
        if (handleMouseButton(copiedEvent))
            info.cancelled = true;
    });
    m_touchDownListener = events.input.touch.down.listen([this](const ITouch::SDownEvent& event, Event::SCallbackInfo& info) {
        if (handleTouchDown(event))
            info.cancelled = true;
    });
    m_touchMotionListener = events.input.touch.motion.listen([this](const ITouch::SMotionEvent& event, Event::SCallbackInfo& info) {
        if (handleTouchMotion(event))
            info.cancelled = true;
    });
    m_touchUpListener = events.input.touch.up.listen([this](const ITouch::SUpEvent& event, Event::SCallbackInfo& info) {
        if (handleTouchUp(event))
            info.cancelled = true;
    });
    m_touchCancelListener = events.input.touch.cancel.listen([this](const ITouch::SCancelEvent& event, Event::SCallbackInfo& info) {
        if (handleTouchUp(ITouch::SUpEvent{.timeMs = event.timeMs, .touchID = event.touchID}, true))
            info.cancelled = true;
    });
    m_keyboardListener = events.input.keyboard.key.listen([this](const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) { handleKeyboard(event, info); });
    m_windowOpenListener = events.window.open.listen([this](PHLWINDOW window) { handleWindowSetChange(window, WindowSetChangeKind::Open); });
    m_windowDestroyListener = events.window.destroy.listen([this](PHLWINDOW window) {
        pruneWindowActivationHistory(window);
        handleWindowSetChange(window, WindowSetChangeKind::General, true);
    });
    m_windowCloseListener = events.window.close.listen([this](PHLWINDOW window) {
        pruneWindowActivationHistory(window);
        handleWindowSetChange(window, WindowSetChangeKind::General, true);
    });
    m_windowActiveListener = events.window.active.listen([this](PHLWINDOW window, Desktop::eFocusReason) {
        const auto previousActiveMonitor = m_lastActiveWindowMonitor;
        m_lastActiveWindowMonitor = focusMonitorForWindow(window);
        if (!m_lastActiveWindowMonitor)
            m_lastActiveWindowMonitor = Desktop::focusState()->monitor();
        recordWindowActivation(window);
        if (m_applyingWorkspaceTransitionCommit)
            return;
        if (m_overviewEditingDispatcherInProgress && activeDirectNiriSingleWorkspaceOverview()) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] defer window.active refresh to overview dispatcher"
                    << " active=" << debugWindowLabel(window);
                debugLog(out.str());
            }
            return;
        }
        if (timedNiriSingleWorkspaceTransitionActive() && window && window != m_workspaceTransition.targetState.focusDuringOverview && window->m_workspace &&
            window->m_workspace->m_id == m_workspaceTransition.targetWorkspaceId) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] retarget workspace transition for changed active window"
                    << " expected=" << debugWindowLabel(m_workspaceTransition.targetState.focusDuringOverview)
                    << " active=" << debugWindowLabel(window)
                    << " workspace=" << debugWorkspaceLabel(window->m_workspace);
                debugLog(out.str());
            }
            commitActiveNiriWorkspaceTransitionForRetarget();
        }
        if (!m_beginCloseInProgress && window && hasManagedWindow(window) && windowMatchesOverviewScope(window, m_state, false)) {
            const bool sameOverviewFocus = isVisible() && m_state.phase == Phase::Active && m_state.focusDuringOverview == window;
            const bool syncScrollingSpot = !shouldSuppressNiriFocusScrollForMonitorReturn(window, previousActiveMonitor);
            refreshNiriScrollingOverviewAfterFocusDispatcher(sameOverviewFocus ? "window-active-same" : "window-active", {}, syncScrollingSpot);
        }
    });
    m_windowMoveWorkspaceListener =
        events.window.moveToWorkspace.listen([this](PHLWINDOW window, PHLWORKSPACE) { handleWindowSetChange(window, WindowSetChangeKind::MoveToWorkspace); });
    m_workspaceActiveListener = events.workspace.active.listen([this](PHLWORKSPACE workspace) { handleWorkspaceChange(workspace); });
    m_monitorRemovedListener = events.monitor.removed.listen([this](PHLMONITOR monitor) { handleMonitorChange(monitor); });
    m_monitorFocusedListener = events.monitor.focused.listen([this](PHLMONITOR) {
        if (isVisible() && shouldHandleInput())
            updateHoveredFromPointer(false, false, false, false, "monitor-focused");
    });
    m_configReloadedListener = events.config.reloaded.listen([this] { handleConfigReloaded(); });

    replaceNativeWorkspaceGestures("initialize");

    return true;
}

void OverviewController::pruneWindowActivationHistory(const PHLWINDOW& removedWindow) {
    if (removedWindow)
        m_windowMruSerials.erase(removedWindow);

    std::erase_if(m_windowMruSerials, [](const auto& entry) { return !entry.first || !entry.first->m_isMapped; });
}

void OverviewController::recordWindowActivation(const PHLWINDOW& window, bool allowWhileVisible) {
    if (!window || !window->m_isMapped || window->isHidden())
        return;

    if (!allowWhileVisible && isVisible())
        return;

    pruneWindowActivationHistory();
    m_windowMruSerials[window] = m_nextWindowMruSerial++;
}

bool OverviewController::shouldUseRecentWindowOrdering(const State& state) const {
    return !state.collectionPolicy.onlyActiveWorkspace && multiWorkspaceSortRecentFirstEnabled();
}

SP<IKeyboard> OverviewController::inputKeyboardWithState() const {
    for (const auto& candidate : g_pInputManager->m_keyboards) {
        if (candidate && candidate->m_xkbState)
            return candidate;
    }

    return {};
}

bool OverviewController::switchReleaseKeyHeld() const {
    if (!toggleSwitchModeEnabled())
        return false;

    const auto releaseKey = switchReleaseKeyConfig();
    if (releaseKey.empty())
        return false;

    if (const auto modifierMask = switchReleaseModifierMask(releaseKey))
        return anyKeyboardWithState([modifierMask](const auto& keyboard) { return (keyboard->getModifiers() & *modifierMask) != 0; });

    if (const auto configuredKeycode = parseSwitchReleaseKeycode(releaseKey))
        return anyKeyboardWithState([configuredKeycode](const auto& keyboard) { return keyboard->getPressed(*configuredKeycode); });

    const xkb_keysym_t configuredKeysym = keysymFromConfiguredSwitchReleaseKey(releaseKey);
    return anyKeyboardWithState([configuredKeysym](const auto& keyboard) { return keyboardHasPressedKeysym(keyboard, configuredKeysym); });
}

bool OverviewController::isSwitchReleaseEvent(const IKeyboard::SKeyEvent& event, const SP<IKeyboard>& keyboard) const {
    const auto releaseKey = switchReleaseKeyConfig();
    if (!keyboard || !keyboard->m_xkbState || releaseKey.empty())
        return false;

    if (const auto modifierMask = switchReleaseModifierMask(releaseKey)) {
        return !switchReleaseKeyHeld();
    }

    if (const auto configuredKeycode = parseSwitchReleaseKeycode(releaseKey))
        return event.keycode + 8 == *configuredKeycode;

    const xkb_keysym_t configuredKeysym = keysymFromConfiguredSwitchReleaseKey(releaseKey);
    if (configuredKeysym == XKB_KEY_NoSymbol)
        return false;

    const xkb_keysym_t eventKeysym = xkb_state_key_get_one_sym(keyboard->m_xkbState, event.keycode + 8);
    if (eventKeysym == configuredKeysym)
        return true;

    xkb_keymap* const keymap = xkb_state_get_keymap(keyboard->m_xkbState);
    if (!keymap)
        return false;

    const xkb_keycode_t eventKeycode = event.keycode + 8;
    for (int level = 0; level < 8; ++level) {
        const int count = xkb_keymap_key_get_syms_by_level(keymap, eventKeycode, 0, level, nullptr);
        if (count <= 0)
            break;

        const xkb_keysym_t* levelSyms = nullptr;
        const int            resolvedCount = xkb_keymap_key_get_syms_by_level(keymap, eventKeycode, 0, level, &levelSyms);
        for (int index = 0; index < resolvedCount; ++index) {
            if (levelSyms[index] == configuredKeysym)
                return true;
        }
    }

    return false;
}

void OverviewController::updateToggleSwitchSessionReleaseTracking(const char* source) {
    if (!m_toggleSwitchSessionActive || m_beginCloseInProgress || m_state.phase == Phase::Inactive || m_state.phase == Phase::Closing ||
        m_state.phase == Phase::ClosingSettle)
        return;

    const bool releaseKeyHeld = switchReleaseKeyHeld();
    if (!m_toggleSwitchReleaseArmed) {
        if (!releaseKeyHeld)
            return;

        m_toggleSwitchReleaseArmed = true;
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] toggle switch release armed source=" << (source ? source : "?") << " key=" << switchReleaseKeyConfig();
            debugLog(out.str());
        }
        return;
    }

    if (releaseKeyHeld)
        return;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] toggle switch release poll close source=" << (source ? source : "?") << " key=" << switchReleaseKeyConfig();
        debugLog(out.str());
    }
    beginClose(CloseMode::ActivateSelection);
}

void OverviewController::scheduleToggleSwitchReleasePoll() {
    if (!g_pEventLoopManager)
        return;

    if (!m_toggleSwitchReleasePollTimer) {
        m_toggleSwitchReleasePollTimer = makeShared<CEventLoopTimer>(
            TOGGLE_SWITCH_RELEASE_POLL_INTERVAL,
            [this](SP<CEventLoopTimer> self, void*) {
                updateToggleSwitchSessionReleaseTracking("poll");

                if (!m_toggleSwitchSessionActive || m_beginCloseInProgress || m_state.phase == Phase::Inactive || m_state.phase == Phase::Closing ||
                    m_state.phase == Phase::ClosingSettle) {
                    self->updateTimeout(std::nullopt);
                    return;
                }

                self->updateTimeout(TOGGLE_SWITCH_RELEASE_POLL_INTERVAL);
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_toggleSwitchReleasePollTimer);
        return;
    }

    m_toggleSwitchReleasePollTimer->updateTimeout(TOGGLE_SWITCH_RELEASE_POLL_INTERVAL);
}

void OverviewController::clearToggleSwitchReleasePollTimer() {
    if (!m_toggleSwitchReleasePollTimer)
        return;

    m_toggleSwitchReleasePollTimer->cancel();
    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_toggleSwitchReleasePollTimer);
    m_toggleSwitchReleasePollTimer.reset();
}

void OverviewController::clearToggleSwitchSession() {
    m_toggleSwitchSessionActive = false;
    m_toggleSwitchReleaseArmed = false;
    clearToggleSwitchReleasePollTimer();
}

void OverviewController::schedulePostCloseCursorShapeReset() {
    if (!g_pEventLoopManager) {
        refreshPostCloseCursorShape();
        return;
    }

    m_postCloseCursorShapeResetTicks = POST_CLOSE_CURSOR_SHAPE_RESET_TICKS;

    if (!m_postCloseCursorShapeResetTimer) {
        m_postCloseCursorShapeResetTimer = makeShared<CEventLoopTimer>(
            POST_CLOSE_CURSOR_SHAPE_RESET_INTERVAL,
            [this](SP<CEventLoopTimer> self, void*) {
                if (g_controller != this || m_state.phase != Phase::Inactive || m_postCloseCursorShapeResetTicks == 0) {
                    self->updateTimeout(std::nullopt);
                    return;
                }

                refreshPostCloseCursorShape();
                --m_postCloseCursorShapeResetTicks;

                self->updateTimeout(m_postCloseCursorShapeResetTicks > 0 ? std::optional{POST_CLOSE_CURSOR_SHAPE_RESET_INTERVAL} : std::nullopt);
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_postCloseCursorShapeResetTimer);
        return;
    }

    m_postCloseCursorShapeResetTimer->updateTimeout(POST_CLOSE_CURSOR_SHAPE_RESET_INTERVAL);
}

void OverviewController::clearPostCloseCursorShapeResetTimer() {
    m_postCloseCursorShapeResetTicks = 0;
    if (!m_postCloseCursorShapeResetTimer)
        return;

    m_postCloseCursorShapeResetTimer->cancel();
    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_postCloseCursorShapeResetTimer);
    m_postCloseCursorShapeResetTimer.reset();
}

void OverviewController::scheduleDeferredOpen(ScopeOverride requestedScope) {
    m_pendingDeferredOpenScope = requestedScope;

    if (!g_pEventLoopManager)
        return;

    if (!m_deferredOpenTimer) {
        m_deferredOpenTimer = makeShared<CEventLoopTimer>(
            DEFERRED_OPEN_POLL_INTERVAL,
            [this](SP<CEventLoopTimer> self, void*) {
                if (g_controller != this || !m_pendingDeferredOpenScope || isVisible()) {
                    self->updateTimeout(std::nullopt);
                    return;
                }

                if (g_pInputManager && g_pInputManager->hasHeldButtons()) {
                    self->updateTimeout(DEFERRED_OPEN_POLL_INTERVAL);
                    return;
                }

                const auto requestedScope = *m_pendingDeferredOpenScope;
                m_pendingDeferredOpenScope.reset();
                self->updateTimeout(std::nullopt);

                const auto monitor = g_pCompositor->getMonitorFromCursor();
                if (!monitor)
                    return;

                const ScopedFlag dispatching(m_deferredOpenTimerDispatching);
                beginOpen(monitor, requestedScope);
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_deferredOpenTimer);
        return;
    }

    m_deferredOpenTimer->updateTimeout(DEFERRED_OPEN_POLL_INTERVAL);
}

void OverviewController::clearPendingDeferredOpen() {
    m_pendingDeferredOpenScope.reset();
    if (m_deferredOpenTimerDispatching)
        return;

    if (!m_deferredOpenTimer)
        return;

    m_deferredOpenTimer->cancel();
    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_deferredOpenTimer);
    m_deferredOpenTimer.reset();
}

void OverviewController::armThemeSurfaceFeedback(std::size_t frames) {
    if (frames == 0)
        return;

    m_themeSurfaceFeedbackFrames = std::max(m_themeSurfaceFeedbackFrames, frames);
    m_themeWorkspaceFeedbackFrames = std::max(m_themeWorkspaceFeedbackFrames, std::min(frames, THEME_WORKSPACE_FEEDBACK_FRAMES));
    pumpThemeSurfaceFeedbackFrames();

    if (m_themeSurfaceFeedbackFrames == 0 || !g_pEventLoopManager)
        return;

    if (!m_themeSurfaceFeedbackTimer) {
        m_themeSurfaceFeedbackTimer = makeShared<CEventLoopTimer>(
            THEME_SURFACE_FEEDBACK_INTERVAL,
            [this](SP<CEventLoopTimer> self, void*) {
                if (g_controller != this || m_themeSurfaceFeedbackFrames == 0) {
                    self->updateTimeout(std::nullopt);
                    return;
                }

                pumpThemeSurfaceFeedbackFrames();
                self->updateTimeout(m_themeSurfaceFeedbackFrames > 0 ? std::optional{THEME_SURFACE_FEEDBACK_INTERVAL} : std::nullopt);
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_themeSurfaceFeedbackTimer);
        return;
    }

    m_themeSurfaceFeedbackTimer->updateTimeout(THEME_SURFACE_FEEDBACK_INTERVAL);
}

void OverviewController::pumpThemeSurfaceFeedbackFrames() {
    if (m_themeSurfaceFeedbackFrames == 0 || !g_pHyprRenderer || !g_pCompositor)
        return;

    using SendFrameEventsToWorkspaceFn = void (*)(Render::IHyprRenderer*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
    static SendFrameEventsToWorkspaceFn sendFrameEventsToWorkspaceFn = nullptr;
    static bool                         sendFrameEventsToWorkspaceResolved = false;
    if (!sendFrameEventsToWorkspaceResolved) {
        sendFrameEventsToWorkspaceResolved = true;
        sendFrameEventsToWorkspaceFn =
            reinterpret_cast<SendFrameEventsToWorkspaceFn>(findFunction("sendFrameEventsToWorkspace", "IHyprRenderer::sendFrameEventsToWorkspace"));
        if (!sendFrameEventsToWorkspaceFn)
            debugLog("[hymission] failed to resolve IHyprRenderer::sendFrameEventsToWorkspace for theme refresh");
    }

    if (!sendFrameEventsToWorkspaceFn) {
        m_themeSurfaceFeedbackFrames = 0;
        return;
    }

    const auto now = Time::steadyNow();
    for (const auto& workspace : g_pCompositor->getWorkspacesCopy()) {
        if (!workspace || workspace->m_isSpecialWorkspace)
            continue;

        const auto monitor = workspace->m_monitor.lock();
        if (!monitor)
            continue;

        sendFrameEventsToWorkspaceFn(g_pHyprRenderer.get(), monitor, workspace, now);
    }

    if (m_themeWorkspaceFeedbackFrames > 0 && renderThemeWorkspaceFeedbackFrame())
        --m_themeWorkspaceFeedbackFrames;

    --m_themeSurfaceFeedbackFrames;
}

void OverviewController::armThemeWorkspaceActivationRefresh() {
    if (!stripThemeWorkspaceActivationRefreshEnabled() || !g_pEventLoopManager || !isVisible() || !workspaceStripEnabled(m_state) || m_state.stripEntries.empty() ||
        !m_focusWorkspaceOnCurrentMonitorOriginal)
        return;

    if (niriModeAppliesToState(m_state) && m_state.collectionPolicy.onlyActiveWorkspace && m_state.windows.empty())
        return;

    clearThemeWorkspaceActivationRefresh();

    std::vector<std::size_t> targetIndexes;
    for (std::size_t index = 0; index < m_state.stripEntries.size(); ++index) {
        const auto& entry = m_state.stripEntries[index];
        if (!entry.active)
            continue;

        for (std::size_t previous = index; previous > 0; --previous) {
            if (m_state.stripEntries[previous - 1].monitor != entry.monitor)
                break;

            targetIndexes.push_back(previous - 1);
            break;
        }

        for (std::size_t next = index + 1; next < m_state.stripEntries.size(); ++next) {
            if (m_state.stripEntries[next].monitor != entry.monitor)
                break;

            targetIndexes.push_back(next);
            break;
        }
    }

    for (const auto index : targetIndexes) {
        if (index >= m_state.stripEntries.size())
            continue;

        const auto& entry = m_state.stripEntries[index];
        if (!entry.monitor || !entry.workspace || entry.workspace->m_isSpecialWorkspace || entry.workspaceId == WORKSPACE_INVALID)
            continue;
        if (entry.monitor->m_activeWorkspace == entry.workspace)
            continue;
        if (!workspaceNeedsThemeActivationRefresh(entry.workspace))
            continue;
        if (std::ranges::any_of(m_themeWorkspaceActivationRefreshTargets,
                                [&](const ThemeWorkspaceActivationTarget& target) { return target.workspace == entry.workspace; }))
            continue;

        m_themeWorkspaceActivationRefreshTargets.push_back({
            .monitor = entry.monitor,
            .workspace = entry.workspace,
            .workspaceId = entry.workspaceId,
        });
    }

    if (m_themeWorkspaceActivationRefreshTargets.empty())
        return;

    const auto focusState = Desktop::focusState();
    m_themeWorkspaceActivationRefreshOriginalMonitor = focusState ? focusState->monitor() : PHLMONITOR{};
    m_themeWorkspaceActivationRefreshOriginalWorkspace =
        m_themeWorkspaceActivationRefreshOriginalMonitor ? m_themeWorkspaceActivationRefreshOriginalMonitor->m_activeWorkspace : PHLWORKSPACE{};
    m_themeWorkspaceActivationRefreshOriginalFocus = focusState ? focusState->window() : PHLWINDOW{};
    m_themeWorkspaceActivationRefreshActive = true;
    m_themeWorkspaceActivationRefreshRestoring = false;
    m_themeWorkspaceActivationRefreshIndex = 0;
    const auto generation = ++m_themeWorkspaceActivationRefreshGeneration;

    m_themeWorkspaceActivationRefreshTimer = makeShared<CEventLoopTimer>(
        THEME_SURFACE_FEEDBACK_INTERVAL,
        [this, generation](SP<CEventLoopTimer> self, void*) {
            if (g_controller != this || generation != m_themeWorkspaceActivationRefreshGeneration || !m_themeWorkspaceActivationRefreshActive) {
                self->updateTimeout(std::nullopt);
                return;
            }

            stepThemeWorkspaceActivationRefresh();
            self->updateTimeout(m_themeWorkspaceActivationRefreshActive ? std::optional{THEME_SURFACE_FEEDBACK_INTERVAL} : std::nullopt);
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_themeWorkspaceActivationRefreshTimer);

    stepThemeWorkspaceActivationRefresh();
}

void OverviewController::stepThemeWorkspaceActivationRefresh() {
    if (!m_themeWorkspaceActivationRefreshActive)
        return;

    if (m_themeWorkspaceActivationRefreshIndex < m_themeWorkspaceActivationRefreshTargets.size()) {
        const auto target = m_themeWorkspaceActivationRefreshTargets[m_themeWorkspaceActivationRefreshIndex++];
        if (!target.monitor || !target.workspace || target.workspace->m_isSpecialWorkspace || target.workspaceId == WORKSPACE_INVALID)
            return;

        m_pendingStripWorkspaceChangeTarget = target.workspace;
        Desktop::focusState()->rawMonitorFocus(target.monitor);
        m_focusWorkspaceOnCurrentMonitorOriginal(std::to_string(target.workspaceId));
        m_stripSnapshotsDirty = true;
        scheduleWorkspaceStripSnapshotRefresh();
        damageOwnedMonitors();
        return;
    }

    if (!m_themeWorkspaceActivationRefreshRestoring) {
        m_themeWorkspaceActivationRefreshRestoring = true;
        const auto originalMonitor = m_themeWorkspaceActivationRefreshOriginalMonitor.lock();
        const auto originalWorkspace = m_themeWorkspaceActivationRefreshOriginalWorkspace.lock();
        if (originalMonitor && originalWorkspace && originalWorkspace->m_monitor.lock() == originalMonitor) {
            m_pendingStripWorkspaceChangeTarget = originalWorkspace;
            Desktop::focusState()->rawMonitorFocus(originalMonitor);
            m_focusWorkspaceOnCurrentMonitorOriginal(std::to_string(originalWorkspace->m_id));
        }
        return;
    }

    if (const auto originalFocus = m_themeWorkspaceActivationRefreshOriginalFocus.lock(); originalFocus && originalFocus->m_isMapped && !originalFocus->isHidden())
        focusWindowCompat(originalFocus);
    else if (const auto originalMonitor = m_themeWorkspaceActivationRefreshOriginalMonitor.lock())
        Desktop::focusState()->rawMonitorFocus(originalMonitor);

    m_stripSnapshotsDirty = true;
    m_stripSnapshotSurfaceFeedbackFrames = std::max(m_stripSnapshotSurfaceFeedbackFrames, static_cast<std::size_t>(std::max(1, stripThemeSurfaceFeedbackFrames())));
    scheduleWorkspaceStripSnapshotRefresh();
    damageOwnedMonitors();
    clearThemeWorkspaceActivationRefresh();
}

void OverviewController::clearThemeWorkspaceActivationRefresh() {
    ++m_themeWorkspaceActivationRefreshGeneration;
    m_themeWorkspaceActivationRefreshActive = false;
    m_themeWorkspaceActivationRefreshRestoring = false;
    m_themeWorkspaceActivationRefreshTargets.clear();
    m_themeWorkspaceActivationRefreshIndex = 0;
    m_themeWorkspaceActivationRefreshOriginalMonitor.reset();
    m_themeWorkspaceActivationRefreshOriginalWorkspace.reset();
    m_themeWorkspaceActivationRefreshOriginalFocus.reset();

    if (!m_themeWorkspaceActivationRefreshTimer)
        return;

    m_themeWorkspaceActivationRefreshTimer->cancel();
    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_themeWorkspaceActivationRefreshTimer);
    m_themeWorkspaceActivationRefreshTimer.reset();
}

bool OverviewController::renderThemeWorkspaceFeedbackFrame() {
    if (!g_pHyprRenderer || !g_pHyprOpenGL || !g_pCompositor)
        return false;

    if (m_stripSnapshotRenderDepth > 0 || g_pHyprRenderer->m_renderData.pMonitor)
        return false;

    using RenderWindowFn = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
    static RenderWindowFn renderWindowFn = nullptr;
    static bool           renderWindowResolved = false;
    if (!renderWindowResolved) {
        renderWindowResolved = true;
        renderWindowFn = reinterpret_cast<RenderWindowFn>(findFunction("renderWindow", "IHyprRenderer::renderWindow"));
        if (!renderWindowFn)
            debugLog("[hymission] failed to resolve IHyprRenderer::renderWindow for theme feedback");
    }
    if (!renderWindowFn)
        return false;

    bool rendered = false;
    for (const auto& workspace : g_pCompositor->getWorkspacesCopy()) {
        if (!workspace || workspace->m_isSpecialWorkspace)
            continue;

        const auto monitor = workspace->m_monitor.lock();
        if (!monitor)
            continue;

        std::vector<PHLWINDOW> workspaceWindows;
        for (const auto& window : g_pCompositor->m_windows) {
            if (!window || !window->m_isMapped || window->isHidden() || window->m_workspace != workspace)
                continue;
            workspaceWindows.push_back(window);
        }
        if (workspaceWindows.empty())
            continue;

        const int fbWidth = std::max(1, static_cast<int>(std::ceil(static_cast<double>(monitor->m_size.x) * renderScaleForMonitor(monitor))));
        const int fbHeight = std::max(1, static_cast<int>(std::ceil(static_cast<double>(monitor->m_size.y) * renderScaleForMonitor(monitor))));
        auto      framebuffer = createFramebuffer("hymission theme workspace feedback");
        if (!framebuffer || !framebuffer->alloc(fbWidth, fbHeight))
            continue;
        framebuffer->setImageDescription(monitor->workBufferImageDescription());

        const auto previousWorkspace = monitor->m_activeWorkspace;
        const auto previousSpecialWorkspace = monitor->m_activeSpecialWorkspace;
        const bool previousVisible = workspace->m_visible;
        const auto previousRenderOffsetValue = workspace->m_renderOffset->value();
        const auto previousRenderOffsetGoal = workspace->m_renderOffset->goal();
        const float previousAlphaValue = workspace->m_alpha->value();
        const float previousAlphaGoal = workspace->m_alpha->goal();
        const bool previousBlockSurfaceFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
        const bool previousBlockScreenShader = g_pHyprRenderer->m_renderData.blockScreenShader;
        const bool previousRenderingSnapshot = g_pHyprRenderer->m_bRenderingSnapshot;

        monitor->m_activeWorkspace = workspace;
        monitor->m_activeSpecialWorkspace.reset();
        workspace->m_visible = true;
        workspace->m_renderOffset->setValueAndWarp(Vector2D{});
        workspace->m_alpha->setValueAndWarp(1.F);
        g_pHyprRenderer->m_bBlockSurfaceFeedback = false;
        g_pHyprRenderer->m_bRenderingSnapshot = false;
        g_pHyprOpenGL->makeEGLCurrent();

        CRegion fakeDamage{0, 0, fbWidth, fbHeight};
        if (g_pHyprRenderer->beginFullFakeRender(monitor, fakeDamage, framebuffer)) {
            g_pHyprRenderer->draw(CClearPassElement::SClearData{.color = CHyprColor{0.0, 0.0, 0.0, 1.0}}, fakeDamage);
            const auto now = Time::steadyNow();
            for (const auto& window : workspaceWindows)
                renderWindowFn(g_pHyprRenderer.get(), window, monitor, now, false, Render::RENDER_PASS_ALL, false, true);
            g_pHyprRenderer->m_renderData.blockScreenShader = true;
            g_pHyprRenderer->endRender();
            rendered = true;
        }

        g_pHyprRenderer->m_bRenderingSnapshot = previousRenderingSnapshot;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockSurfaceFeedback;
        g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockScreenShader;
        workspace->m_visible = previousVisible;
        workspace->m_renderOffset->setValueAndWarp(previousRenderOffsetValue);
        if (previousRenderOffsetGoal != previousRenderOffsetValue)
            *workspace->m_renderOffset = previousRenderOffsetGoal;
        workspace->m_alpha->setValueAndWarp(previousAlphaValue);
        if (std::abs(previousAlphaGoal - previousAlphaValue) > 0.0001F)
            *workspace->m_alpha = previousAlphaGoal;
        monitor->m_activeSpecialWorkspace = previousSpecialWorkspace;
        monitor->m_activeWorkspace = previousWorkspace;
    }

    return rendered;
}

void OverviewController::clearThemeSurfaceFeedbackTimer() {
    m_themeSurfaceFeedbackFrames = 0;
    m_themeWorkspaceFeedbackFrames = 0;
    if (!m_themeSurfaceFeedbackTimer)
        return;

    m_themeSurfaceFeedbackTimer->cancel();
    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_themeSurfaceFeedbackTimer);
    m_themeSurfaceFeedbackTimer.reset();
}

void OverviewController::clearWorkspaceStripSnapshotRefreshTimer() {
    if (!m_stripSnapshotRefreshTimer)
        return;

    m_stripSnapshotRefreshTimer->cancel();
    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_stripSnapshotRefreshTimer);
    m_stripSnapshotRefreshTimer.reset();
}

void OverviewController::armPostCloseOpenDebounce(ScopeOverride closedScope) {
    const auto debounce = postCloseCrossScopeDebounce();
    if (debounce.count() <= 0) {
        m_postCloseOpenDebounceScope.reset();
        m_postCloseOpenDebounceUntil = {};
        return;
    }

    m_postCloseOpenDebounceScope = closedScope;
    m_postCloseOpenDebounceUntil = std::chrono::steady_clock::now() + debounce;
}

bool OverviewController::shouldSuppressPostCloseOpen(ScopeOverride requestedScope) const {
    if (!m_postCloseOpenDebounceScope || *m_postCloseOpenDebounceScope == requestedScope)
        return false;

    return std::chrono::steady_clock::now() < m_postCloseOpenDebounceUntil;
}

SDispatchResult OverviewController::open(const std::string& args) {
    std::string error;
    const auto requestedScope = parseScopeOverride(args, error);
    if (!requestedScope)
        return {.success = false, .error = error};

    if (!isVisible() && shouldSuppressPostCloseOpen(*requestedScope)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] suppress post-close cross-scope open requested=" << static_cast<int>(*requestedScope)
                << " closed=" << static_cast<int>(*m_postCloseOpenDebounceScope);
            debugLog(out.str());
        }
        return {};
    }

    if (!isVisible() && g_pEventLoopManager) {
        scheduleDeferredOpen(*requestedScope);
        return {};
    }

    const auto monitor = g_pCompositor->getMonitorFromCursor();
    if (!monitor) {
        return {.success = false, .error = "no monitor under cursor"};
    }

    if ((m_state.phase == Phase::Opening || m_state.phase == Phase::Active) && *requestedScope == m_state.collectionPolicy.requestedScope)
        return {};

    beginOpen(monitor, *requestedScope);
    return {};
}

bool OverviewController::allowsWorkspaceSwitchInOverviewForGestures() const {
    return allowsWorkspaceSwitchInOverview();
}

bool OverviewController::blocksWorkspaceSwitchInOverviewForGestures() const {
    return shouldBlockWorkspaceSwitchInOverview();
}

SDispatchResult OverviewController::close() {
    if (m_state.phase == Phase::Inactive || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return {};

    beginClose();
    return {};
}

SDispatchResult OverviewController::toggle(const std::string& args) {
    if (m_state.phase == Phase::Inactive || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle) {
        const bool activateSwitchSession = toggleSwitchModeEnabled() && switchReleaseKeyHeld();
        const auto result = open(args);
        if (!result.success)
            return result;

        if (activateSwitchSession && isVisible()) {
            m_toggleSwitchSessionActive = true;
            m_toggleSwitchReleaseArmed = true;
            scheduleToggleSwitchReleasePoll();
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] toggle switch arm autoNext=" << (switchToggleAutoNextEnabled() ? 1 : 0) << " releaseKey=" << switchReleaseKeyConfig()
                    << " releaseArmed=" << (m_toggleSwitchReleaseArmed ? 1 : 0);
                debugLog(out.str());
            }
            if (switchToggleAutoNextEnabled())
                (void)moveSelectionCircular(1, "toggle-switch-open");
        }

        return result;
    }

    if (m_toggleSwitchSessionActive) {
        scheduleToggleSwitchReleasePoll();
        if (debugLogsEnabled())
            debugLog("[hymission] toggle switch cycle");
        (void)moveSelectionCircular(1, "toggle-switch-cycle");
        return {};
    }

    return close();
}

SDispatchResult OverviewController::debugCurrentLayout() const {
    const auto monitor = g_pCompositor->getMonitorFromCursor();
    if (!monitor) {
        return {.success = false, .error = "no monitor under cursor"};
    }

    const State preview = buildState(monitor, ScopeOverride::Default);
    if (preview.windows.empty()) {
        notify(collectionSummary(monitor), CHyprColor(1.0, 0.7, 0.2, 1.0), 5000);
        return {};
    }

    std::ostringstream summary;
    summary << "[hymission] " << preview.windows.size() << " previews";

    const auto limit = std::min<std::size_t>(preview.windows.size(), 3);
    for (std::size_t index = 0; index < limit; ++index) {
        const auto& rect = preview.windows[index].slot.target;
        summary << " | #" << index << ' ' << static_cast<int>(rect.x) << ',' << static_cast<int>(rect.y) << ' ' << static_cast<int>(rect.width) << 'x'
                << static_cast<int>(rect.height);
    }

    notify(summary.str(), CHyprColor(0.3, 0.9, 1.0, 1.0), 4000);
    return {};
}

void OverviewController::renderStage(eRenderStage stage) {
    if (stage == RENDER_POST && m_surfaceFeedbackOverrideActive) {
        g_pHyprRenderer->m_bBlockSurfaceFeedback = m_surfaceFeedbackOverrideBackup;
        m_surfaceFeedbackOverrideActive = false;
    }

    if (m_stripSnapshotRenderDepth > 0)
        return;

    if (!isVisible())
        return;

    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!monitor || !ownsMonitor(monitor))
        return;

    if (!m_workspaceTransition.active && (m_state.phase == Phase::Opening || m_state.phase == Phase::Active) &&
        std::any_of(m_state.windows.begin(), m_state.windows.end(), [](const ManagedWindow& managed) { return managed.window && managed.window->m_fadingOut; })) {
        scheduleVisibleStateRebuild();
    }

    if (stage == RENDER_PRE_WINDOWS && !m_workspaceTransition.active && m_state.phase == Phase::Active && workspaceStripEnabled(m_state) && !m_state.windows.empty()) {
        const bool niriOverview = niriModeAppliesToState(m_state);
        const bool directNiriOverview = usesDirectNiriScrollingOverview(m_state);
        const auto currentOverviewSourceFor = [&](const ManagedWindow& managed) {
            const auto window = managed.window;
            const bool useGoalGeometry = shouldUseGoalGeometryForStateSnapshot(window);
            const Rect stateGlobal = stateSnapshotGlobalRectForWindow(window, useGoalGeometry);
            if (managed.isNiriFloatingOverlay || isFloatingOverviewWindow(window) || (window && window->m_pinned))
                return floatingOverviewSourceGlobalRectForWindow(window, renderGlobalRectForWindow(window, useGoalGeometry));
            if (directNiriOverview) {
                const Rect scrollingSource = scrollingOverviewSourceGlobalRectForWindow(window, stateGlobal);
                PHLWINDOW layoutAnchorWindow;
                if (window && m_state.focusDuringOverview && !m_state.focusDuringOverview->m_pinned && m_state.focusDuringOverview->m_workspace == window->m_workspace)
                    layoutAnchorWindow = m_state.focusDuringOverview;
                else
                    layoutAnchorWindow = focusCandidateForWorkspace(window ? window->m_workspace : PHLWORKSPACE{});
                if (const auto rowGeometry = scrollingOverviewTapeRowGeometryForWindow(window, scrollingSource, layoutAnchorWindow))
                    return rowGeometry->sourceGlobal;
                return scrollingSource;
            }
            return layoutAnchorGlobalRectForWindow(window, useGoalGeometry);
        };

        const auto focusedFloatingPinnedAnchor = [&]() -> PHLWINDOW {
            if (!directNiriOverview)
                return {};

            PHLWINDOW floatingFocus = selectedWindow();
            if ((!floatingFocus || (!floatingFocus->m_pinned && !isFloatingOverviewWindow(floatingFocus))) && m_state.focusDuringOverview)
                floatingFocus = m_state.focusDuringOverview;
            if (!floatingFocus || (!floatingFocus->m_pinned && !isFloatingOverviewWindow(floatingFocus)))
                return {};

            const auto workspace = floatingFocus->m_pinned ? activeLayoutWorkspace() : floatingFocus->m_workspace;
            if (!workspace || !isScrollingWorkspace(workspace))
                return {};

            const auto* sourceManaged = managedWindowFor(m_state, floatingFocus, true);
            const Rect sourceRect = sourceManaged ? currentPreviewRect(*sourceManaged) : liveGlobalRectForWindow(floatingFocus);
            PHLWINDOW best;
            double    bestDistance = std::numeric_limits<double>::infinity();
            for (const auto& managed : m_state.windows) {
                const auto candidate = managed.window;
                if (!candidate || candidate == floatingFocus || !candidate->m_isMapped || candidate->m_workspace != workspace || candidate->m_pinned)
                    continue;

                const auto target = candidate->layoutTarget();
                if (!target || target->floating() || isFloatingOverviewWindow(candidate))
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

            return best;
        };

        if (!directNiriOverview && g_pEventLoopManager && !m_visibleStateRebuildScheduled) {
            if (const auto refocusAnchor = focusedFloatingPinnedAnchor(); refocusAnchor && refocusAnchor != selectedWindow()) {
                m_visibleStateRebuildScheduled = true;
                const auto generation = ++m_visibleStateRebuildGeneration;
                g_pEventLoopManager->doLater([this, generation, refocusAnchor] {
                    if (g_controller != this || generation != m_visibleStateRebuildGeneration)
                        return;

                    m_visibleStateRebuildScheduled = false;
                    if (!isVisible() || m_state.phase != Phase::Active || m_workspaceTransition.active || m_beginCloseInProgress)
                        return;
                    if (!refocusAnchor || !refocusAnchor->m_isMapped || !hasManagedWindow(refocusAnchor))
                        return;

                    selectWindowInState(m_state, refocusAnchor);
                    m_state.focusDuringOverview = refocusAnchor;
                    m_queuedOverviewSelectionTarget.reset();
                    m_queuedOverviewLiveFocusTarget.reset();
                    if (refocusAnchor->m_workspace) {
                        m_pendingLiveFocusWorkspaceChangeTarget = refocusAnchor;
                        (void)activateWindowWorkspaceForFocus(refocusAnchor);
                    }
                    focusWindowCompat(refocusAnchor, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
                    if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == refocusAnchor)
                        m_pendingLiveFocusWorkspaceChangeTarget.reset();
                    (void)syncScrollingWorkspaceSpotOnWindow(refocusAnchor);
                    if (refocusAnchor->m_workspace)
                        refreshWorkspaceLayoutSnapshot(refocusAnchor->m_workspace);
                    refreshNiriScrollingOverviewAfterLayoutScroll("floating-selected-render-refocus");
                    rebuildVisibleState(refocusAnchor, true);
                    m_stripSnapshotsDirty = true;
                    scheduleWorkspaceStripSnapshotRefresh();
                    damageOwnedMonitors();
                });
            }
        }

        bool needsForcedRelayout = false;
        PHLWINDOW relayoutTriggerWindow;
        for (const auto& managed : m_state.windows) {
            const auto window = managed.window;
            if (!window || !window->m_isMapped || window->m_fadingOut || window->isHidden()) {
                needsForcedRelayout = true;
                relayoutTriggerWindow = window;
                break;
            }

            const auto nextMonitor = preferredMonitorForWindow(window, m_state);
            const bool expectedNiriFloatingOverlay = niriOverview && (isFloatingOverviewWindow(window) || window->m_pinned);
            if (nextMonitor != managed.targetMonitor || managed.isFloating != window->m_isFloating || managed.isPinned != window->m_pinned ||
                managed.isNiriFloatingOverlay != expectedNiriFloatingOverlay) {
                needsForcedRelayout = true;
                relayoutTriggerWindow = window;
                break;
            }

            const Rect currentSource = currentOverviewSourceFor(managed);
            if (!directNiriOverview && !rectApproxEqual(currentSource, managed.naturalGlobal, 0.5)) {
                needsForcedRelayout = true;
                relayoutTriggerWindow = window;
                break;
            }
        }

        if (directNiriOverview && needsForcedRelayout) {
            scheduleVisibleStateRebuild();
            needsForcedRelayout = false;
        }

        if (needsForcedRelayout && !m_visibleStateRebuildScheduled) {
            const auto resolveRelayoutAnchor = [&](const PHLWINDOW& window) -> PHLWINDOW {
                if (!window || !usesDirectNiriScrollingOverview(m_state))
                    return window;
                if (!window->m_pinned && !isFloatingOverviewWindow(window))
                    return window;

                const auto workspace = window->m_pinned ? activeLayoutWorkspace() : window->m_workspace;
                if (!workspace || !isScrollingWorkspace(workspace))
                    return window;

                const auto* sourceManaged = managedWindowFor(m_state, window, true);
                const Rect sourceRect = sourceManaged ? currentPreviewRect(*sourceManaged) : liveGlobalRectForWindow(window);
                PHLWINDOW best;
                double    bestDistance = std::numeric_limits<double>::infinity();
                for (const auto& managed : m_state.windows) {
                    const auto candidate = managed.window;
                    if (!candidate || candidate == window || !candidate->m_isMapped || candidate->m_workspace != workspace || candidate->m_pinned)
                        continue;

                    const auto target = candidate->layoutTarget();
                    if (!target || target->floating() || isFloatingOverviewWindow(candidate))
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

                return best ? best : window;
            };

            auto preferredSelected = resolveRelayoutAnchor(relayoutTriggerWindow ? relayoutTriggerWindow : selectedWindow());
            if (preferredSelected && preferredSelected->m_workspace)
                refreshWorkspaceLayoutSnapshot(preferredSelected->m_workspace);
            if (const auto activeWorkspace = activeLayoutWorkspace(); activeWorkspace && (!preferredSelected || activeWorkspace != preferredSelected->m_workspace))
                refreshWorkspaceLayoutSnapshot(activeWorkspace);

            if (!g_pEventLoopManager || !insideRenderLifecycle()) {
                if (preferredSelected) {
                    selectWindowInState(m_state, preferredSelected);
                    m_state.focusDuringOverview = preferredSelected;
                    m_queuedOverviewSelectionTarget.reset();
                    m_queuedOverviewLiveFocusTarget.reset();
                    if (preferredSelected->m_workspace) {
                        m_pendingLiveFocusWorkspaceChangeTarget = preferredSelected;
                        (void)activateWindowWorkspaceForFocus(preferredSelected);
                    }
                    focusWindowCompat(preferredSelected, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
                    if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == preferredSelected)
                        m_pendingLiveFocusWorkspaceChangeTarget.reset();
                    (void)syncScrollingWorkspaceSpotOnWindow(preferredSelected);
                    refreshNiriScrollingOverviewAfterLayoutScroll("render-geometry-force-refocus");
                }
                rebuildVisibleState(preferredSelected, true);
            } else {
                m_visibleStateRebuildScheduled = true;
                const auto generation = ++m_visibleStateRebuildGeneration;
                g_pEventLoopManager->doLater([this, generation, preferredSelected] {
                    if (g_controller != this || generation != m_visibleStateRebuildGeneration)
                        return;

                    m_visibleStateRebuildScheduled = false;
                    if (!isVisible() || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
                        return;

                    PHLWINDOW target = preferredSelected;
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
                    selectWindowInState(m_state, target);
                    m_state.focusDuringOverview = target;
                    m_queuedOverviewSelectionTarget.reset();
                    m_queuedOverviewLiveFocusTarget.reset();
                    if (target->m_workspace) {
                        m_pendingLiveFocusWorkspaceChangeTarget = target;
                        (void)activateWindowWorkspaceForFocus(target);
                    }
                    focusWindowCompat(target, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
                    if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == target)
                        m_pendingLiveFocusWorkspaceChangeTarget.reset();
                    (void)syncScrollingWorkspaceSpotOnWindow(target);
                    refreshNiriScrollingOverviewAfterLayoutScroll("render-geometry-deferred-refocus");
                    rebuildVisibleState(target, true);
                });
            }
        }
    }

    setFullscreenRenderOverride(true);
    expandRenderDamageToFullMonitor(monitor);

    if (stage == RENDER_PRE_WINDOWS && m_overviewSurfaceFeedbackFrames > 0 && !m_surfaceFeedbackOverrideActive) {
        m_surfaceFeedbackOverrideActive = true;
        m_surfaceFeedbackOverrideBackup = g_pHyprRenderer->m_bBlockSurfaceFeedback;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = false;
    }

    if (stage == RENDER_POST_WALLPAPER) {
        if (m_niriDragSession.active && monitor == m_state.ownerMonitor)
            tickDirectNiriWindowDragEdgeScroll();
        if (m_cursorShapeResetFrames > 0) {
            resetStaleClientCursorShape();
            --m_cursorShapeResetFrames;
        }
        updateOverviewWorkspaceTransition();
        updateAnimation();
        flushQueuedSelectionRetargetDuringOverview();
        flushQueuedRealFocusDuringOverview();
        const bool directNiriHandoff = usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state);
        if (directNiriHandoff && m_deactivatePending) {
            // Final direct-Niri close frame: keep drawing Hymission's native-size
            // wallpaper/layout proxy until deactivate() has actually unhooked us.
            // The populated-workspace path otherwise hides the native layer here
            // while also skipping the overview wallpaper pass, producing a black
            // handoff flash.  Native windows are allowed to render normally below
            // via shouldRenderWindowHook/prepareSurfaceRenderData guards.
            g_directNiriNativeHandoffUntil = {};
        } else if (directNiriHandoff && directNiriNativeHandoffActive()) {
            // Non-deactivation native handoff guard.  During the actual final close
            // frame we intentionally do not take this path: the overview wallpaper
            // proxy must cover the hidden native wallpaper until deactivate().
            return;
        }
        g_pHyprRenderer->m_renderPass.add(makeUnique<OverviewWallpaperPassElement>(this, monitor));
        if ((isAnimating() || m_state.phase == Phase::ClosingSettle || m_state.relayoutActive || m_postOpenRefreshFrames > 0 ||
             m_stripSnapshotSurfaceFeedbackFrames > 0 || m_overviewSurfaceFeedbackFrames > 0) &&
            !m_deactivatePending) {
            damageOwnedMonitors();
        }
    } else if (stage == RENDER_POST_WINDOWS) {
        const bool directNiriHandoff = usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state);
        if (directNiriHandoff && directNiriNativeHandoffActive()) {
            // Match the clean entry handoff: native windows/wallpaper can own the
            // desktop sample, but the overview still owns selection chrome until
            // deactivation. This prevents a one-frame native active-border blink
            // or border dropout at the end of the close animation.
            g_pHyprRenderer->m_renderPass.add(makeUnique<OverviewOverlayPassElement>(this, monitor, true));
            if (m_deactivatePending) {
                if (debugLogsEnabled())
                    debugLog("[hymission] post-windows queue deferred deactivate");
                scheduleDeactivate();
            } else {
                damageOwnedMonitors();
            }
            return;
        }

        if (m_deactivatePending) {
            if (debugLogsEnabled())
                debugLog("[hymission] post-windows queue deferred deactivate");
            scheduleDeactivate();
            return;
        }

        g_pHyprRenderer->m_renderPass.add(makeUnique<OverviewOverlayPassElement>(this, monitor));
        if (workspaceStripEnabled(m_state)) {
            const uint64_t currentStripThemeColor = activeBorderGradientSignature();
            if (!m_lastStripThemeColorValid) {
                m_lastStripThemeColorValid = true;
                m_lastStripThemeColor = currentStripThemeColor;
            } else if (currentStripThemeColor != m_lastStripThemeColor) {
                m_lastStripThemeColor = currentStripThemeColor;
                m_stripSnapshotSurfaceFeedbackFrames = std::max(m_stripSnapshotSurfaceFeedbackFrames, static_cast<std::size_t>(stripThemeSurfaceFeedbackFrames()));
                m_stripSnapshotsDirty = true;
            }
        } else {
            m_lastStripThemeColorValid = false;
        }
        if (shouldContinuouslyRefreshWorkspaceStripSnapshots() || m_stripSnapshotSurfaceFeedbackFrames > 0) {
            m_stripSnapshotsDirty = true;
            scheduleWorkspaceStripSnapshotRefresh();
        }
        if (m_postOpenRefreshFrames > 0)
            --m_postOpenRefreshFrames;
        if (m_overviewSurfaceFeedbackFrames > 0)
            --m_overviewSurfaceFeedbackFrames;
    }
}

void OverviewController::handleConfigReloaded() {
    replaceNativeWorkspaceGestures("config-reloaded");

    const uint64_t currentLayoutSignature = layoutAffectingConfigSignature(m_handle);
    const auto     previousLayoutSignatureIt = g_openOverviewLayoutConfigSignatures.find(this);
    const bool     layoutAffectingConfigChanged =
        isVisible() && previousLayoutSignatureIt != g_openOverviewLayoutConfigSignatures.end() && previousLayoutSignatureIt->second != currentLayoutSignature;
    g_openOverviewLayoutConfigSignatures[this] = currentLayoutSignature;

    if (layoutAffectingConfigChanged && isVisible() && m_state.phase != Phase::Closing && m_state.phase != Phase::ClosingSettle) {
        const auto previousPreviewRects = captureCurrentPreviewRects();
        PHLWINDOW  preferredWindow = m_state.focusDuringOverview;
        if (!preferredWindow || !preferredWindow->m_isMapped || !hasManagedWindow(preferredWindow))
            preferredWindow = selectedWindow();
        if ((!preferredWindow || !preferredWindow->m_isMapped || !hasManagedWindow(preferredWindow)) &&
            Desktop::focusState()->window() && hasManagedWindow(Desktop::focusState()->window()))
            preferredWindow = Desktop::focusState()->window();

        const auto reloadedPolicy = loadCollectionPolicy(m_state.collectionPolicy.requestedScope);
        const bool collectionPolicyChanged = reloadedPolicy.onlyActiveWorkspace != m_state.collectionPolicy.onlyActiveWorkspace ||
            reloadedPolicy.onlyActiveMonitor != m_state.collectionPolicy.onlyActiveMonitor || reloadedPolicy.includeSpecial != m_state.collectionPolicy.includeSpecial;

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] layout-affecting config reload"
                << " directNiri=" << (usesDirectNiriScrollingOverview(m_state) ? 1 : 0)
                << " collectionPolicyChanged=" << (collectionPolicyChanged ? 1 : 0)
                << " phase=" << static_cast<int>(m_state.phase)
                << " focusFit=" << getConfigInt(m_handle, "scrolling:focus_fit_method", 0)
                << " windows=" << m_state.windows.size()
                << " preferred=" << debugWindowLabel(preferredWindow);
            debugLog(out.str());
        }

        if (usesDirectNiriScrollingOverview(m_state) && !collectionPolicyChanged) {
            std::vector<PHLMONITOR> recalculatedMonitors;
            const auto rememberMonitor = [&](const PHLMONITOR& monitor) {
                if (monitor && std::ranges::find(recalculatedMonitors, monitor) == recalculatedMonitors.end())
                    recalculatedMonitors.push_back(monitor);
            };

            const auto reflowScrollingWorkspace = [&](const PHLWORKSPACE& workspace) {
                if (!workspace || !isScrollingWorkspace(workspace))
                    return;

                auto* const scrolling = scrollingAlgorithmForWorkspace(workspace);
                if (workspace->m_space)
                    workspace->m_space->recalculate();

                PHLWINDOW workspaceFocus;
                if (preferredWindow && preferredWindow->m_workspace == workspace && !preferredWindow->m_pinned)
                    workspaceFocus = preferredWindow;
                if (!workspaceFocus)
                    workspaceFocus = focusCandidateForWorkspace(workspace);

                if (scrolling && scrolling->m_scrollingData) {
                    if (workspaceFocus) {
                        const auto target = workspaceFocus->layoutTarget();
                        if (target && !target->floating()) {
                            if (const auto targetData = scrolling->dataFor(target); targetData) {
                                if (const auto column = targetData->column.lock()) {
                                    column->lastFocusedTarget = targetData;
                                    if (scrolling->m_scrollingData->controller) {
                                        if (getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 1)
                                            scrolling->m_scrollingData->fitCol(column);
                                        else
                                            scrolling->m_scrollingData->centerCol(column);
                                    }
                                }
                            }
                        }
                    }
                    scrolling->m_scrollingData->recalculate(true);
                }

                if (workspace->m_space)
                    workspace->m_space->recalculate();

                if (const auto monitor = workspace->m_monitor.lock()) {
                    rememberMonitor(monitor);
                    g_layoutManager->recalculateMonitor(monitor);
                }
            };

            for (const auto& workspace : m_state.managedWorkspaces)
                reflowScrollingWorkspace(workspace);
            if (const auto activeWorkspace = activeLayoutWorkspace(); activeWorkspace)
                reflowScrollingWorkspace(activeWorkspace);
            if (preferredWindow && preferredWindow->m_workspace)
                reflowScrollingWorkspace(preferredWindow->m_workspace);

            if (preferredWindow && preferredWindow->m_workspace && isScrollingWorkspace(preferredWindow->m_workspace))
                (void)syncScrollingWorkspaceSpotOnWindow(preferredWindow, ScrollingSpotTargeting::Configured, ScrollingSpotSyncIntent::FocusChange);

            for (const auto& monitor : recalculatedMonitors)
                g_layoutManager->recalculateMonitor(monitor);
            if (g_pAnimationManager)
                g_pAnimationManager->frameTick();

            m_stripSnapshotsDirty = true;
            scheduleWorkspaceStripSnapshotRefresh();
            refreshNiriScrollingOverviewAfterLayoutScroll("config-reload-layout", &previousPreviewRects);
        } else {
            rebuildVisibleState(preferredWindow, true);
        }
    }

    if (isVisible()) {
        syncNiriWallpaperSnapshots();
        clearNiriWallpaperLayoutLayerRefresh();
        startNiriWallpaperLayoutLayerRefresh();
    }

    if (!refreshPreviewsOnConfigReloadEnabled())
        return;

    const auto refreshFrames = static_cast<std::size_t>(stripThemeSurfaceFeedbackFrames());
    armThemeSurfaceFeedback(refreshFrames);
    if (!isVisible()) {
        m_pendingOverviewSurfaceFeedbackFrames = std::max(m_pendingOverviewSurfaceFeedbackFrames, refreshFrames);
        return;
    }

    m_postOpenRefreshFrames = std::max(m_postOpenRefreshFrames, refreshFrames);
    m_overviewSurfaceFeedbackFrames = std::max(m_overviewSurfaceFeedbackFrames, refreshFrames);
    m_stripSnapshotsDirty = true;
    m_stripSnapshotSurfaceFeedbackFrames = std::max(m_stripSnapshotSurfaceFeedbackFrames, refreshFrames);
    scheduleWorkspaceStripSnapshotRefresh();
    armThemeWorkspaceActivationRefresh();
    damageOwnedMonitors();
}

void OverviewController::handleMouseMove() {
    if (m_restoreScrollingFollowFocusAfterScrollMouseMove && !m_scrollGestureSession.active) {
        if (debugLogsEnabled())
            debugLog("[hymission] restore scrolling:follow_focus after scroll mouse move");
        m_restoreScrollingFollowFocusAfterScrollMouseMove = false;
        setScrollingFollowFocusOverride(false);
    }

    if (m_postCloseForcedFocusLatched && !isVisible()) {
        if (m_ignorePostCloseMouseMoveCount > 0) {
            --m_ignorePostCloseMouseMoveCount;
            return;
        }

        clearPostCloseForcedFocus();
        if (m_restoreInputFollowMouseAfterPostClose) {
            setInputFollowMouseOverride(false);
            m_restoreInputFollowMouseAfterPostClose = false;
        }
    }

    if (!shouldHandleInput())
        return;

    if (m_pressedWindowIndex || m_draggedWindowIndex) {
        updateHoveredFromPointer(false, false, false, false, "mouse-move-drag");

        const Vector2D pointer = g_pInputManager->getMouseCoordsInternal();
        if (m_niriDragSession.active) {
            updateDirectNiriWindowDrag(pointer);
            return;
        }

        if (!m_draggedWindowIndex && m_pressedWindowIndex && *m_pressedWindowIndex < m_state.windows.size()) {
            const double distance = std::hypot(pointer.x - m_pressedWindowPointer.x, pointer.y - m_pressedWindowPointer.y);
            const auto window = m_state.windows[*m_pressedWindowIndex].window;
            if (canDragWindowInDirectNiriOverview(window) && distance >= nativeWindowDragThreshold()) {
                beginDirectNiriWindowDrag(*m_pressedWindowIndex, pointer);
            } else if (!canDragWindowInDirectNiriOverview(window) && distance >= 14.0) {
                const auto& managed = m_state.windows[*m_pressedWindowIndex];
                const Rect  rect = currentPreviewRect(managed);
                m_draggedWindowIndex = m_pressedWindowIndex;
                m_draggedWindowPointerOffset = Vector2D{pointer.x - rect.x, pointer.y - rect.y};
            }
        }

        damageOwnedMonitors();
        return;
    }

    updateHoveredFromPointer(true, true, true, true, "mouse-move");
}

bool OverviewController::handleMouseButton(const IPointer::SButtonEvent& event) {
    if (m_postCloseForcedFocusLatched && !isVisible()) {
        clearPostCloseForcedFocus();
        if (m_restoreInputFollowMouseAfterPostClose) {
            setInputFollowMouseOverride(false);
            m_restoreInputFollowMouseAfterPostClose = false;
        }
    }

    if (!shouldHandleInput())
        return false;

    if (m_state.phase == Phase::Closing)
        return true;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] mouse button event state=" << static_cast<int>(event.state) << " button=" << event.button;
        debugLog(out.str());
    }

    const auto buttonLooksValid = [&](uint32_t button) {
        return button >= BTN_LEFT && button <= BTN_TASK;
    };

    uint32_t effectiveButton = event.button;
    bool     synthesizedButton = false;
    if (!buttonLooksValid(effectiveButton)) {
        effectiveButton = BTN_LEFT;
        synthesizedButton = true;
    }

    wl_pointer_button_state effectiveState = event.state;
    bool                    synthesizedState = false;
    if (effectiveState != WL_POINTER_BUTTON_STATE_PRESSED && effectiveState != WL_POINTER_BUTTON_STATE_RELEASED) {
        effectiveState = m_primaryButtonPressed ? WL_POINTER_BUTTON_STATE_RELEASED : WL_POINTER_BUTTON_STATE_PRESSED;
        synthesizedState = true;
    } else if (synthesizedButton) {
        // Some Hyprland/plugin ABI combinations are delivering a valid callback
        // but a corrupted button code and an unusable edge indicator. Fall back
        // to a minimal local left-button state machine so strip clicks still
        // produce a press edge followed by a release edge.
        effectiveState = m_primaryButtonPressed ? WL_POINTER_BUTTON_STATE_RELEASED : WL_POINTER_BUTTON_STATE_PRESSED;
        synthesizedState = true;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] mouse button resolved rawState=" << static_cast<int>(event.state) << " rawButton=" << event.button
            << " effectiveState=" << static_cast<int>(effectiveState) << " effectiveButton=" << effectiveButton
            << " synthesizedButton=" << (synthesizedButton ? 1 : 0) << " synthesizedState=" << (synthesizedState ? 1 : 0)
            << " primaryDownBefore=" << (m_primaryButtonPressed ? 1 : 0);
        debugLog(out.str());
    }

    if (effectiveButton != BTN_LEFT)
        return true;

    if (effectiveState == WL_POINTER_BUTTON_STATE_PRESSED)
        m_primaryButtonPressed = true;
    else if (effectiveState == WL_POINTER_BUTTON_STATE_RELEASED)
        m_primaryButtonPressed = false;

    const auto cachedHoveredStripIndex = m_state.hoveredStripIndex;
    const auto cachedHoveredIndex = m_state.hoveredIndex;
    const Vector2D pointerBeforeUpdate = g_pInputManager->getMouseCoordsInternal();
    updateHoveredFromPointer(false, false, false, false, "mouse-button-refresh");
    const auto effectiveHoveredStripIndex = m_state.hoveredStripIndex ? m_state.hoveredStripIndex : cachedHoveredStripIndex;
    const auto effectiveHoveredIndex = m_state.hoveredIndex ? m_state.hoveredIndex : cachedHoveredIndex;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] mouse button state=" << static_cast<int>(effectiveState) << " button=" << effectiveButton
            << " ptr=" << vectorToString(pointerBeforeUpdate)
            << " cachedStrip=" << (cachedHoveredStripIndex ? std::to_string(*cachedHoveredStripIndex) : "<null>")
            << " liveStrip=" << (m_state.hoveredStripIndex ? std::to_string(*m_state.hoveredStripIndex) : "<null>")
            << " effectiveStrip=" << (effectiveHoveredStripIndex ? std::to_string(*effectiveHoveredStripIndex) : "<null>")
            << " cachedWindow=" << (cachedHoveredIndex ? std::to_string(*cachedHoveredIndex) : "<null>")
            << " liveWindow=" << (m_state.hoveredIndex ? std::to_string(*m_state.hoveredIndex) : "<null>")
            << " pressedStrip=" << (m_pressedStripIndex ? std::to_string(*m_pressedStripIndex) : "<null>");
        debugLog(out.str());
    }

    if (effectiveState == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (finishDirectNiriWindowDrag()) {
            clearStripWindowDragState();
            return true;
        }

        if (m_draggedWindowIndex && *m_draggedWindowIndex < m_state.windows.size()) {
            const auto window = m_state.windows[*m_draggedWindowIndex].window;
            const auto hoveredStripIndex = m_state.hoveredStripIndex;
            clearStripWindowDragState();

            if (window && hoveredStripIndex && *hoveredStripIndex < m_state.stripEntries.size()) {
                const auto& entry = m_state.stripEntries[*hoveredStripIndex];
                auto        targetWorkspace = entry.workspace ? entry.workspace : g_pCompositor->getWorkspaceByID(entry.workspaceId);
                if (!targetWorkspace && entry.monitor && entry.workspaceId != WORKSPACE_INVALID) {
                    const std::string targetName = entry.workspaceName.empty() ? std::to_string(entry.workspaceId) : entry.workspaceName;
                    targetWorkspace = g_pCompositor->createNewWorkspace(entry.workspaceId, entry.monitor->m_id, targetName);
                }

                if (targetWorkspace && window->m_workspace != targetWorkspace) {
                    const auto sourceWorkspace = window->m_workspace;
                    g_pCompositor->moveWindowToWorkspaceSafe(window, targetWorkspace);

                    // Niri treats a workspace move as an activation of the target workspace when
                    // the moved window remains selected. Mirror that here before the overview
                    // rebuilds, otherwise direct niri single-workspace mode keeps the old owner
                    // workspace and leaves a blank lane between the old and new workspaces.
                    targetWorkspace->m_lastFocusedWindow = window;
                    m_pendingLiveFocusWorkspaceChangeTarget = window;
                    (void)activateWindowWorkspaceForFocus(window);
                    focusWindowCompat(window, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
                    if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == window)
                        m_pendingLiveFocusWorkspaceChangeTarget.reset();
                    recordWindowActivation(window, true);
                    (void)syncScrollingWorkspaceSpotOnWindow(window);

                    commitNonScrollingWorkspaceLayout(sourceWorkspace);
                    if (targetWorkspace != sourceWorkspace)
                        commitNonScrollingWorkspaceLayout(targetWorkspace);

                    if (sourceWorkspace && isScrollingWorkspace(sourceWorkspace))
                        refreshWorkspaceLayoutSnapshot(sourceWorkspace);
                    if (targetWorkspace && targetWorkspace != sourceWorkspace && isScrollingWorkspace(targetWorkspace))
                        refreshWorkspaceLayoutSnapshot(targetWorkspace);

                    // Keep the dragged window as the overview target and force a full rebuild.
                    // Without the forced rebuild, niri-mode can keep the old lane targets when
                    // the window set is unchanged but the window's workspace/lane changed.
                    selectWindowInState(m_state, window);
                    m_state.focusDuringOverview = window;
                    if (niriModeAppliesToState(m_state) && m_state.collectionPolicy.onlyActiveWorkspace && isScrollingWorkspace(targetWorkspace)) {
                        m_state.ownerWorkspace = targetWorkspace;
                        const auto targetMonitorForActivity = entry.monitor ? entry.monitor : targetWorkspace->m_monitor.lock();
                        (void)refreshWorkspaceStripActivity(m_state, targetMonitorForActivity, targetWorkspace->m_id);
                    }
                    m_queuedOverviewSelectionTarget.reset();
                    m_queuedOverviewSelectionSyncScrollingSpot = false;
                    m_queuedOverviewSelectionCenterCursor = false;
                    m_queuedOverviewLiveFocusTarget.reset();
                    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
                    m_queuedOverviewLiveFocusCenterCursor = false;

                    if (g_pAnimationManager)
                        g_pAnimationManager->frameTick();

                    if (g_pEventLoopManager) {
                        if (!m_visibleStateRebuildScheduled) {
                            m_visibleStateRebuildScheduled = true;
                            const auto generation = ++m_visibleStateRebuildGeneration;
                            g_pEventLoopManager->doLater([this, generation, window] {
                                if (g_controller != this || generation != m_visibleStateRebuildGeneration)
                                    return;

                                m_visibleStateRebuildScheduled = false;
                                if (!isVisible() || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
                                    return;

                                rebuildVisibleState(window, true);
                                m_stripSnapshotsDirty = true;
                                scheduleWorkspaceStripSnapshotRefresh();
                                damageOwnedMonitors();
                            });
                        }
                    } else {
                        rebuildVisibleState(window, true);
                    }
                    m_stripSnapshotsDirty = true;
                    scheduleWorkspaceStripSnapshotRefresh();
                }
            }

            damageOwnedMonitors();
            return true;
        }

        if (m_pressedStripIndex && *m_pressedStripIndex < m_state.stripEntries.size()) {
            const auto pressedStripIndex = *m_pressedStripIndex;
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] mouse release activating strip index=" << pressedStripIndex;
                debugLog(out.str());
            }
            clearStripWindowDragState();
            activateStripTarget(pressedStripIndex);
            return true;
        }

        if (m_pressedWindowIndex && *m_pressedWindowIndex < m_state.windows.size()) {
            const auto pressedIndex = *m_pressedWindowIndex;
            const auto pressedWindow = m_state.windows[pressedIndex].window;
            const bool directNiriPressedWindow =
                usesDirectNiriScrollingOverview(m_state) && m_state.collectionPolicy.onlyActiveWorkspace && pressedWindow && pressedWindow->m_workspace &&
                isScrollingWorkspace(pressedWindow->m_workspace);
            const bool wasAlreadySelected = m_state.selectedIndex && *m_state.selectedIndex == pressedIndex;

            if (directNiriPressedWindow && !wasAlreadySelected) {
                const auto pressedWorkspace = pressedWindow && !pressedWindow->m_pinned ? pressedWindow->m_workspace : PHLWORKSPACE{};
                const auto currentNiriWorkspace = m_state.ownerWorkspace ? m_state.ownerWorkspace : activeLayoutWorkspace();
                const bool directNiriCrossWorkspacePress = pressedWorkspace && currentNiriWorkspace && pressedWorkspace != currentNiriWorkspace &&
                    !pressedWorkspace->m_isSpecialWorkspace;

                if (directNiriCrossWorkspacePress) {
                    m_queuedOverviewSelectionTarget.reset();
                    m_queuedOverviewSelectionSyncScrollingSpot = false;
                    m_queuedOverviewSelectionCenterCursor = false;
                    m_queuedOverviewLiveFocusTarget.reset();
                    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
                    m_queuedOverviewLiveFocusCenterCursor = false;
                    clearStripWindowDragState();

                    if (m_workspaceTransition.active)
                        commitActiveNiriWorkspaceTransitionForRetarget();

                    if (m_workspaceTransition.active) {
                        damageOwnedMonitors();
                        return true;
                    }

                    const auto pressedMonitor = pressedWorkspace->m_monitor.lock();
                    auto       transitionMonitor = pressedMonitor ? pressedMonitor : m_state.ownerMonitor;
                    if (!transitionMonitor)
                        transitionMonitor = g_pCompositor->getMonitorFromCursor();

                    if (transitionMonitor) {
                        if (debugLogsEnabled()) {
                            std::ostringstream out;
                            out << "[hymission] mouse release workspace transition target=" << debugWindowLabel(pressedWindow)
                                << " from=" << debugWorkspaceLabel(currentNiriWorkspace)
                                << " to=" << debugWorkspaceLabel(pressedWorkspace)
                                << " deferredPress=1";
                            debugLog(out.str());
                        }

                        const bool startedTransition = beginOverviewWorkspaceTransition(
                            transitionMonitor,
                            pressedWorkspace->m_id,
                            pressedWorkspace->m_name,
                            pressedWorkspace,
                            false,
                            WorkspaceTransitionMode::TimedCommit,
                            std::nullopt,
                            pressedWindow);

                        if (startedTransition)
                            return true;
                    }

                    // Fall through to the same-workspace focus path if the transition could
                    // not be created. This keeps a click from being dropped entirely.
                }

                const auto previousSelectedWindow = selectedWindow();
                const auto previousPreviewRects = captureCurrentPreviewRects();

                m_state.selectedIndex = pressedIndex;
                m_state.focusDuringOverview = pressedWindow;
                m_queuedOverviewSelectionTarget.reset();
                m_queuedOverviewSelectionSyncScrollingSpot = false;
                m_queuedOverviewSelectionCenterCursor = false;
                m_queuedOverviewLiveFocusTarget.reset();
                m_queuedOverviewLiveFocusSyncScrollingSpot = false;
                m_queuedOverviewLiveFocusCenterCursor = false;
                clearStripWindowDragState();

                syncRealFocusDuringOverview(pressedWindow, true, &previousPreviewRects, true);
                updateSelectedWindowLayout(previousSelectedWindow);
                damageOwnedMonitors();
                return true;
            }

            m_state.selectedIndex = pressedIndex;
            clearStripWindowDragState();
            activateSelection();
            return true;
        }

        clearStripWindowDragState();
        return true;
    }

    if (effectiveState != WL_POINTER_BUTTON_STATE_PRESSED)
        return true;

    if (effectiveHoveredStripIndex && *effectiveHoveredStripIndex < m_state.stripEntries.size()) {
        const auto stripWindowIndexAtPointer = [&]() -> std::optional<std::size_t> {
            if (!usesDirectNiriScrollingOverview(m_state) || !m_state.collectionPolicy.onlyActiveWorkspace)
                return std::nullopt;

            const auto& entry = m_state.stripEntries[*effectiveHoveredStripIndex];
            if (!entry.monitor || entry.newWorkspaceSlot || entry.windows.empty())
                return std::nullopt;

            const Rect stripRect = animatedWorkspaceStripRect(currentWorkspaceStripRect(entry), entry.monitor);
            if (!usableOverviewRect(stripRect))
                return std::nullopt;

            std::optional<std::size_t> bestWindowIndex;
            double bestDistance = std::numeric_limits<double>::infinity();
            for (const auto& preview : entry.windows) {
                if (!preview.window || !canDragWindowInDirectNiriOverview(preview.window))
                    continue;

                const auto stateWindowIt = std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) {
                    return managed.window == preview.window;
                });
                if (stateWindowIt == m_state.windows.end())
                    continue;

                const auto stripPreviewRect = stripWindowPreviewRectForHitTest(entry, stripRect, preview.window);
                if (!stripPreviewRect || !rectContainsPoint(*stripPreviewRect, pointerBeforeUpdate.x, pointerBeforeUpdate.y))
                    continue;

                const double distance = rectCenterDistanceSquared(*stripPreviewRect, pointerBeforeUpdate.x, pointerBeforeUpdate.y);
                if (!bestWindowIndex || distance < bestDistance) {
                    bestWindowIndex = static_cast<std::size_t>(std::distance(m_state.windows.begin(), stateWindowIt));
                    bestDistance = distance;
                }
            }

            return bestWindowIndex;
        }();

        if (stripWindowIndexAtPointer && *stripWindowIndexAtPointer < m_state.windows.size()) {
            clearStripWindowDragState();
            // Do not focus/retarget the native scrolling camera on press. A strip
            // press is only a click if it is released under binds:drag_threshold;
            // movement past that threshold becomes a drag/drop operation instead.
            m_pressedWindowIndex = stripWindowIndexAtPointer;
            m_pressedWindowPointer = pointerBeforeUpdate;
            latchHoverSelectionAnchor(m_pressedWindowPointer);
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] mouse press captured strip window index=" << *m_pressedWindowIndex
                    << " strip=" << *effectiveHoveredStripIndex << " deferredFocus=1";
                debugLog(out.str());
            }
            damageOwnedMonitors();
            return true;
        }

        clearStripWindowDragState();
        m_pressedStripIndex = *effectiveHoveredStripIndex;
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] mouse press captured strip index=" << *m_pressedStripIndex;
            debugLog(out.str());
        }
        damageOwnedMonitors();
        return true;
    }

    if (effectiveHoveredIndex) {
        const auto clickedIndex = *effectiveHoveredIndex;
        const auto clickedWindow = clickedIndex < m_state.windows.size() ? m_state.windows[clickedIndex].window : PHLWINDOW{};
        const bool directNiriSingleWorkspaceScrollClick =
            usesDirectNiriScrollingOverview(m_state) && m_state.collectionPolicy.onlyActiveWorkspace && clickedWindow && clickedWindow->m_workspace &&
            isScrollingWorkspace(clickedWindow->m_workspace);
        const auto clickedWorkspace = clickedWindow && !clickedWindow->m_pinned ? clickedWindow->m_workspace : PHLWORKSPACE{};
        const auto currentNiriWorkspace = m_state.ownerWorkspace ? m_state.ownerWorkspace : activeLayoutWorkspace();
        const bool directNiriCrossWorkspaceClick = directNiriSingleWorkspaceScrollClick && clickedWorkspace && currentNiriWorkspace &&
            clickedWorkspace != currentNiriWorkspace && !clickedWorkspace->m_isSpecialWorkspace;

        if (directNiriSingleWorkspaceScrollClick) {
            clearStripWindowDragState();

            // Match Niri's grab semantics: button press only arms a possible
            // operation.  Do not focus, retarget the scrolling camera, or start
            // a workspace transition until release proves this was a click.
            // Movement past binds:drag_threshold turns the armed press into a
            // direct drag, so unfocused windows can still be dragged.
            m_pressedWindowIndex = effectiveHoveredIndex;
            m_pressedWindowPointer = g_pInputManager->getMouseCoordsInternal();
            latchHoverSelectionAnchor(m_pressedWindowPointer);
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] mouse press captured direct niri window index=" << *m_pressedWindowIndex
                    << " deferredFocus=1 threshold=" << nativeWindowDragThreshold();
                if (directNiriCrossWorkspaceClick)
                    out << " crossWorkspace=1";
                debugLog(out.str());
            }
            damageOwnedMonitors();
            return true;
        }

        if (directNiriCrossWorkspaceClick) {
            clearStripWindowDragState();
            m_queuedOverviewSelectionTarget.reset();
            m_queuedOverviewSelectionSyncScrollingSpot = false;
            m_queuedOverviewSelectionCenterCursor = false;
            m_queuedOverviewLiveFocusTarget.reset();
            m_queuedOverviewLiveFocusSyncScrollingSpot = false;
            m_queuedOverviewLiveFocusCenterCursor = false;
            m_pressedWindowIndex.reset();
            m_pressedWindowPointer = g_pInputManager->getMouseCoordsInternal();
            latchHoverSelectionAnchor(m_pressedWindowPointer);

            if (m_workspaceTransition.active)
                commitActiveNiriWorkspaceTransitionForRetarget();

            if (m_workspaceTransition.active) {
                damageOwnedMonitors();
                return true;
            }

            const auto clickedMonitor = clickedWorkspace->m_monitor.lock();
            auto       transitionMonitor = clickedMonitor ? clickedMonitor : m_state.ownerMonitor;
            if (!transitionMonitor)
                transitionMonitor = g_pCompositor->getMonitorFromCursor();

            if (transitionMonitor) {
                if (debugLogsEnabled()) {
                    std::ostringstream out;
                    out << "[hymission] mouse click workspace transition target=" << debugWindowLabel(clickedWindow)
                        << " from=" << debugWorkspaceLabel(currentNiriWorkspace)
                        << " to=" << debugWorkspaceLabel(clickedWorkspace);
                    debugLog(out.str());
                }

                const bool startedTransition = beginOverviewWorkspaceTransition(
                    transitionMonitor,
                    clickedWorkspace->m_id,
                    clickedWorkspace->m_name,
                    clickedWorkspace,
                    false,
                    WorkspaceTransitionMode::TimedCommit,
                    std::nullopt,
                    clickedWindow);

                if (startedTransition)
                    return true;
            }
        }

        if (niriModeAppliesToState(m_state) && m_state.selectedIndex && clickedIndex != *m_state.selectedIndex) {
            const auto previousSelectedWindow = selectedWindow();
            const auto previousPreviewRects = directNiriSingleWorkspaceScrollClick ? captureCurrentPreviewRects() : PreviewRectSnapshot{};
            m_state.selectedIndex = clickedIndex;
            m_state.focusDuringOverview = clickedWindow;
            m_queuedOverviewSelectionTarget.reset();
            m_queuedOverviewSelectionSyncScrollingSpot = false;
            m_queuedOverviewSelectionCenterCursor = false;
            m_queuedOverviewLiveFocusTarget.reset();
            m_queuedOverviewLiveFocusSyncScrollingSpot = false;
            m_queuedOverviewLiveFocusCenterCursor = false;
            m_pressedWindowIndex.reset();
            m_pressedWindowPointer = g_pInputManager->getMouseCoordsInternal();
            latchHoverSelectionAnchor(m_pressedWindowPointer);
            if (directNiriSingleWorkspaceScrollClick) {
                syncRealFocusDuringOverview(clickedWindow, true, &previousPreviewRects, true);
            } else if (clickedWindow) {
                (void)syncScrollingWorkspaceSpotOnWindow(
                    clickedWindow,
                    ScrollingSpotTargeting::Configured,
                    ScrollingSpotSyncIntent::PreserveNativeCamera);
            }
            updateSelectedWindowLayout(previousSelectedWindow);
            if (!directNiriSingleWorkspaceScrollClick)
                refreshNiriScrollingOverviewAfterLayoutScroll("niri-click-focus");
            damageOwnedMonitors();
            return true;
        }

        const auto previousSelectedWindow = selectedWindow();
        m_state.selectedIndex = effectiveHoveredIndex;
        m_state.focusDuringOverview = clickedWindow;
        m_queuedOverviewSelectionTarget.reset();
        m_queuedOverviewSelectionSyncScrollingSpot = false;
        m_queuedOverviewSelectionCenterCursor = false;
        m_queuedOverviewLiveFocusTarget.reset();
        m_queuedOverviewLiveFocusSyncScrollingSpot = false;
        m_queuedOverviewLiveFocusCenterCursor = false;
        m_pressedWindowIndex = effectiveHoveredIndex;
        m_pressedWindowPointer = g_pInputManager->getMouseCoordsInternal();
        latchHoverSelectionAnchor(m_pressedWindowPointer);
        updateSelectedWindowLayout(previousSelectedWindow);
        damageOwnedMonitors();
        return true;
    }

    clearStripWindowDragState();
    if (debugLogsEnabled())
        debugLog("[hymission] mouse press fell through to background close");
    (void)close();
    return true;
}

void OverviewController::handleKeyboard(const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) {
    const auto keyboard = inputKeyboardWithState();
    if (!keyboard || !keyboard->m_xkbState)
        return;

    if (m_toggleSwitchSessionActive)
        updateToggleSwitchSessionReleaseTracking("keyboard");

    if (!shouldHandleInput())
        return;

    if (m_state.phase == Phase::Closing)
        return;

    if (m_toggleSwitchSessionActive && event.state == WL_KEYBOARD_KEY_STATE_RELEASED && isSwitchReleaseEvent(event, keyboard)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] toggle switch release close key=" << switchReleaseKeyConfig() << " keycode=" << event.keycode << " modifiers=" << keyboard->getModifiers();
            debugLog(out.str());
        }
        beginClose(CloseMode::ActivateSelection);
        info.cancelled = true;
        return;
    }

    if (event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;

    const xkb_keysym_t keysym = xkb_state_key_get_one_sym(keyboard->m_xkbState, event.keycode + 8);
    const uint32_t     modifiers = keyboard->getModifiers();
    const bool         hasActionModifier = (modifiers & (HL_MODIFIER_META | HL_MODIFIER_SHIFT | HL_MODIFIER_CTRL | HL_MODIFIER_ALT)) != 0;
    const bool         disablePlainOverviewArrowAndEnter =
        niriModeAppliesToState(m_state) && m_state.collectionPolicy.onlyActiveWorkspace && isScrollingWorkspace(activeLayoutWorkspace());
    const bool         directionalEditKey = keysym == XKB_KEY_Left || keysym == XKB_KEY_Right || keysym == XKB_KEY_Up || keysym == XKB_KEY_Down;
    const bool         openingInputBarrier = isVisible() && m_state.collectionPolicy.onlyActiveWorkspace && niriModeEnabled() &&
        (m_state.phase == Phase::Opening || (m_overviewVisibilityAnimation && m_overviewVisibilityAnimation->isBeingAnimated()) ||
         m_postOpenRefreshFrames > 0 || overviewOpenInputBarrierActive());
    if (openingInputBarrier && hasActionModifier && directionalEditKey) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] consume modified directional key during overview open"
                << " key=" << keysym
                << " modifiers=" << modifiers
                << " phase=" << static_cast<int>(m_state.phase)
                << " openBarrier=" << (overviewOpenInputBarrierActive() ? 1 : 0)
                << " postOpenFrames=" << m_postOpenRefreshFrames;
            debugLog(out.str());
        }
        info.cancelled = true;
        return;
    }

    if (handleNiriOverviewArrowKeybind(keysym, modifiers)) {
        info.cancelled = true;
        return;
    }

    const bool hasSuperOnly = (modifiers & HL_MODIFIER_META) != 0 && (modifiers & (HL_MODIFIER_SHIFT | HL_MODIFIER_CTRL | HL_MODIFIER_ALT)) == 0;
    if (hasSuperOnly && (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter)) {
        bool launched = false;
        if (g_pKeybindManager) {
            const auto dispatcher = g_pKeybindManager->m_dispatchers.find("exec");
            if (dispatcher != g_pKeybindManager->m_dispatchers.end())
                launched = dispatcher->second("~/bin/launch-terminal").success;
        }

        if (!launched) {
            const auto result = HyprlandAPI::invokeHyprctlCommand("dispatch", "exec ~/bin/launch-terminal");
            launched = result == "ok" || result.empty();
        }

        if (launched) {
            info.cancelled = true;
            return;
        }
    }

    // Only the plain arrow keys and plain Enter are owned by the overview navigation model.
    // Modified keys are either handled above or left alone.
    bool handled = true;
    switch (keysym) {
        case XKB_KEY_Escape:
            (void)close();
            break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (hasActionModifier)
                handled = false;
            else if (disablePlainOverviewArrowAndEnter)
                handled = false;
            else
                activateSelection();
            break;
        case XKB_KEY_Left:
            if (hasActionModifier)
                handled = false;
            else if (disablePlainOverviewArrowAndEnter)
                handled = false;
            else
                moveSelection(Direction::Left);
            break;
        case XKB_KEY_Right:
            if (hasActionModifier)
                handled = false;
            else if (disablePlainOverviewArrowAndEnter)
                handled = false;
            else
                moveSelection(Direction::Right);
            break;
        case XKB_KEY_Up:
            if (hasActionModifier)
                handled = false;
            else if (disablePlainOverviewArrowAndEnter)
                handled = false;
            else
                moveSelection(Direction::Up);
            break;
        case XKB_KEY_Down:
            if (hasActionModifier)
                handled = false;
            else if (disablePlainOverviewArrowAndEnter)
                handled = false;
            else
                moveSelection(Direction::Down);
            break;
        default:
            handled = false;
            break;
    }

    if (handled)
        info.cancelled = true;
}

void OverviewController::handleWindowSetChange(PHLWINDOW window, WindowSetChangeKind kind, bool preferDeferredRebuild) {
    if (window && m_postCloseForcedFocusLatched && m_postCloseForcedFocus.lock() == window)
        clearPostCloseForcedFocus();
    if (window && m_pendingLiveFocusWorkspaceChangeTarget.lock() == window)
        m_pendingLiveFocusWorkspaceChangeTarget.reset();
    if (!window) {
        clearPendingWindowGeometryRetry();
        return;
    }

    if (!isVisible())
        return;

    if (kind == WindowSetChangeKind::MoveToWorkspace && activeDirectNiriSingleWorkspaceOverview()) {
        bool removedPlaceholder = removeOccupiedWorkspacePlaceholder(m_state, window);
        if (m_workspaceTransition.active) {
            removedPlaceholder = removeOccupiedWorkspacePlaceholder(m_workspaceTransition.sourceState, window) || removedPlaceholder;
            removedPlaceholder = removeOccupiedWorkspacePlaceholder(m_workspaceTransition.targetState, window) || removedPlaceholder;
        }
        if (removedPlaceholder)
            damageOwnedMonitors();
    }

    if (m_overviewEditingDispatcherInProgress && activeDirectNiriSingleWorkspaceOverview() && kind != WindowSetChangeKind::MoveToWorkspace)
        return;

    if (kind == WindowSetChangeKind::MoveToWorkspace && window->m_pinned) {
        const auto* managed = managedWindowFor(m_state, window);
        const auto nextMonitor = preferredMonitorForWindow(window, m_state);
        if (managed && windowMatchesOverviewScope(window, m_state, false) && managed->targetMonitor && nextMonitor == managed->targetMonitor) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] ignore pinned workspace-move rebuild target=" << debugWindowLabel(window)
                    << " monitor=" << managed->targetMonitor->m_name;
                if (window->m_workspace)
                    out << " workspace=" << debugWorkspaceLabel(window->m_workspace);
                debugLog(out.str());
            }
            return;
        }
    }

    if (m_applyingWorkspaceTransitionCommit) {
        // The transition target state is precomputed before changeWorkspace().
        // Synchronous window-set events from the commit path, especially pinned
        // windows being reassigned, should not replace that state with a fresh
        // post-commit layout and visibly relayout the overview after the pan.
        if (m_workspaceTransition.active) {
            const auto* targetManaged = managedWindowFor(m_workspaceTransition.targetState, window, true);
            const auto  nextMonitor = preferredMonitorForWindow(window, m_workspaceTransition.targetState);
            if (targetManaged && targetManaged->targetMonitor && (!nextMonitor || nextMonitor == targetManaged->targetMonitor) &&
                windowMatchesOverviewScope(window, m_workspaceTransition.targetState, false)) {
                if (debugLogsEnabled()) {
                    std::ostringstream out;
                    out << "[hymission] ignore transition-commit window-set target=" << debugWindowLabel(window);
                    if (window->m_workspace)
                        out << " workspace=" << debugWorkspaceLabel(window->m_workspace);
                    debugLog(out.str());
                }
                return;
            }
        }

        // Unknown window-set changes still need a rebuild, but defer it until
        // after the commit so we don't clear transition state out from under the
        // caller.
        m_rebuildVisibleStateAfterWorkspaceTransitionCommit = true;
        return;
    }

    const bool shouldDeferRebuild = preferDeferredRebuild || insideRenderLifecycle() || kind == WindowSetChangeKind::MoveToWorkspace;

    if (m_workspaceTransition.active) {
        if (shouldDeferRebuild) {
            scheduleVisibleStateRebuild();
        } else {
            clearOverviewWorkspaceTransition();
            rebuildVisibleState();
            updatePendingWindowGeometryRetry(window);
        }
        return;
    }

    if (!shouldAutoCloseFor(window)) {
        if (m_pendingWindowGeometryRetryTarget.lock() == window)
            clearPendingWindowGeometryRetry();
        return;
    }

    const auto directNiriOneToTwoOpenAnchor =
        kind == WindowSetChangeKind::Open ? directNiriOneToTwoColumnOpenAnchor(window) : PHLWINDOW{};
    if (directNiriOneToTwoOpenAnchor)
        stabilizeDirectNiriOneToTwoColumnOpen(directNiriOneToTwoOpenAnchor);

    if (m_state.phase == Phase::Opening || m_state.phase == Phase::Active) {
        if (shouldDeferRebuild) {
            scheduleVisibleStateRebuild();
        } else if (activeDirectNiriSingleWorkspaceOverview()) {
            refreshVisibleStateMetadata(directNiriOneToTwoOpenAnchor ? directNiriOneToTwoOpenAnchor : window);
            updatePendingWindowGeometryRetry(window);
        } else {
            rebuildVisibleState(directNiriOneToTwoOpenAnchor, static_cast<bool>(directNiriOneToTwoOpenAnchor));
            updatePendingWindowGeometryRetry(window);
        }
        return;
    }

    if (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle) {
        clearPendingWindowGeometryRetry();
        beginClose(CloseMode::Abort);
    }
}

void OverviewController::handleWorkspaceChange(PHLWORKSPACE workspace) {
    const bool liveFocusWorkspaceChange = matchesPendingLiveFocusWorkspaceChange(workspace);
    const bool stripWorkspaceChange = matchesPendingStripWorkspaceChange(workspace);
    const auto action = resolveOverviewWorkspaceChangeAction(isVisible(), m_applyingWorkspaceTransitionCommit, m_workspaceTransition.active,
                                                             m_beginCloseInProgress || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle,
                                                             liveFocusWorkspaceChange || stripWorkspaceChange, allowsWorkspaceSwitchInOverview());
    if (action == OverviewWorkspaceChangeAction::Ignore)
        return;

    if (liveFocusWorkspaceChange) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] keep overview open for live focus workspace change";
            if (const auto target = m_pendingLiveFocusWorkspaceChangeTarget.lock())
                out << " target=" << debugWindowLabel(target);
            debugLog(out.str());
        }
        m_pendingLiveFocusWorkspaceChangeTarget.reset();

        if (!m_state.collectionPolicy.onlyActiveWorkspace) {
            const bool stripActivityChanged = refreshWorkspaceStripActivity(m_state);
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] skip overview rebuild for live focus workspace change outside active-workspace scope";
                if (workspace)
                    out << " workspace=" << debugWorkspaceLabel(workspace);
                out << " stripChanged=" << (stripActivityChanged ? 1 : 0);
                debugLog(out.str());
            }

            damageOwnedMonitors();
            return;
        }
    }

    if (stripWorkspaceChange) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] keep overview open for strip workspace change";
            if (const auto target = m_pendingStripWorkspaceChangeTarget.lock())
                out << " target=" << debugWorkspaceLabel(target);
            debugLog(out.str());
        }
        clearPendingStripWorkspaceChange();
    }

    const bool allowExternalTransition =
        action == OverviewWorkspaceChangeAction::Rebuild && !liveFocusWorkspaceChange && !stripWorkspaceChange && !m_workspaceTransition.active;

    if (insideRenderLifecycle()) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] defer workspace change handling until after render action="
                << (action == OverviewWorkspaceChangeAction::Rebuild ? "rebuild" : "abort");
            if (workspace)
                out << " workspace=" << debugWorkspaceLabel(workspace);
            debugLog(out.str());
        }

        scheduleWorkspaceChangeHandling(workspace, action, allowExternalTransition);
        return;
    }

    if (action == OverviewWorkspaceChangeAction::Rebuild) {
        if (allowExternalTransition && beginExternalOverviewWorkspaceTransition(workspace))
            return;

        if (m_workspaceTransition.active)
            clearOverviewWorkspaceTransition();
        rebuildVisibleState();
        return;
    }

    beginClose(CloseMode::Abort);
}

void OverviewController::handleMonitorChange(PHLMONITOR monitor) {
    if (!isVisible() || !monitor || !m_state.ownerMonitor)
        return;

    if (m_workspaceTransition.active) {
        clearOverviewWorkspaceTransition();
        rebuildVisibleState();
    }

    if (monitor == m_state.ownerMonitor) {
        beginClose(CloseMode::Abort);
        return;
    }

    if (ownsMonitor(monitor))
        rebuildVisibleState();
}

bool OverviewController::shouldRenderWindowHook(const PHLWINDOW& window, const PHLMONITOR& monitor) {
    if (!m_shouldRenderWindowOriginal)
        return false;

    if (isVisible() && window && monitor && ownsMonitor(monitor) &&
        (usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state)) &&
        (m_deactivatePending || directNiriNativeHandoffActive())) {
        // During the final direct-Niri handoff, Hyprland owns native window
        // composition again. Do not keep the overview's forced render override
        // alive here, or windows from non-active workspaces can be drawn into
        // the current monitor for one frame.  The overview still draws the
        // wallpaper proxy until deactivate(), which avoids the black wallpaper
        // gap without forcing extra window rendering.
        return m_shouldRenderWindowOriginal(g_pHyprRenderer.get(), window, monitor);
    }

    if (isVisible() && window && monitor && ownsMonitor(monitor) && renderableManagedWindowFor(window, monitor)) {
        if ((usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state)) &&
            m_state.collectionPolicy.onlyActiveWorkspace &&
            window->m_workspace && !window->m_workspace->isVisible() &&
            niri_scrolling_detail::directNiriWorkspaceTransferRenderGuardActive(window)) {
            // A window that has just been moved into an inactive/unvisited
            // scrolling workspace can have native Hyprland surface geometry that
            // still belongs to the old workspace.  If we force the normal
            // Hyprland window pass for that transient frame, browsers can render
            // oversized/off-lane.  Do not skip the overview preview entirely:
            // renderSelectionChrome() still paints the clipped live main-surface
            // overlay for inactive workspaces, which keeps the window visible
            // while avoiding the stale native-geometry pass.
            static std::size_t s_transferNativePassLogBudget = 96;
            if (debugLogsEnabled() && s_transferNativePassLogBudget > 0) {
                std::ostringstream out;
                out << "[hymission] suppress native pass for workspace-transfer preview"
                    << " window=" << debugWindowLabel(window)
                    << " workspace=" << debugWorkspaceLabel(window->m_workspace)
                    << " monitor=" << monitor->m_name
                    << " wsVisible=" << (window->m_workspace->isVisible() ? 1 : 0);
                debugLog(out.str());
                --s_transferNativePassLogBudget;
            }
            return false;
        }

        static std::size_t s_shouldRenderOverrideLogBudget = 24;
        if (debugLogsEnabled() && s_shouldRenderOverrideLogBudget > 0) {
            std::ostringstream out;
            out << "[hymission] shouldRenderWindow override " << debugWindowLabel(window) << " monitor=" << monitor->m_name;
            debugLog(out.str());
            --s_shouldRenderOverrideLogBudget;
            if (s_shouldRenderOverrideLogBudget == 0)
                debugLog("[hymission] shouldRenderWindow override logs muted to preserve strip snapshot diagnostics");
        }
        return true;
    }

    if (isVisible() && niriModeAppliesToState(m_state) && window && monitor && ownsMonitor(monitor) && !hasManagedWindow(window) && window->onSpecialWorkspace() &&
        isFloatingOverviewWindow(window)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] hide niri special floating live window " << debugWindowLabel(window) << " monitor=" << monitor->m_name;
            debugLog(out.str());
        }
        return false;
    }

    return m_shouldRenderWindowOriginal(g_pHyprRenderer.get(), window, monitor);
}

bool OverviewController::shouldHideLayerSurface(const PHLLS& layer, const PHLMONITOR& monitor) const {
    if (!layer || !monitor || !isVisible() || !ownsMonitor(monitor))
        return false;

    const auto layerMonitor = layer->m_monitor.lock();
    const auto layerResource = layer->m_layerSurface.lock();
    if (!layerMonitor || layerMonitor != monitor || !layerResource || !layer->m_mapped || layer->m_readyToDelete)
        return false;

    if (isNiriWallpaperLayer(layer, monitor))
        return true;

    const bool emptyDirectNiriOwnerMonitor = monitor == m_state.ownerMonitor && m_state.collectionPolicy.onlyActiveWorkspace && m_state.windows.empty() &&
        niriModeAppliesToState(m_state) && centeredEmptyWorkspacePlaceholder(m_state);
    const bool hideBarLayer = layerResource->m_current.exclusive > 0 || shouldHideLayerSurfaceNamespace(layer, hideBarNamespaces());

    if (emptyDirectNiriOwnerMonitor && hideBarLayer) {
        // Keep the live layer visible until the first safe delayed proxy capture
        // has succeeded.  Once a Niri wallpaper-layout proxy exists, hide the
        // real layer so renderNiriWorkspaceBackgrounds() can draw the proxy
        // inside the zoomed workspace viewport, matching the normal window-present path.
        const auto* proxy = hiddenStripLayerProxyFor(layer, monitor);
        auto*       framebuffer = proxy && proxy->framebuffer ? proxy->framebuffer.get() : nullptr;
        return proxy && proxy->niriWallpaperLayoutLayer && framebuffer && framebuffer->isAllocated() && framebuffer->getTexture();
    }

    if (isRetainedNiriWallpaperLayoutLayer(layer, monitor))
        return true;

    if (workspaceStripEnabled(m_state) && hideBarsWhenStripShownEnabled())
        return hideBarLayer;

    if (usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state)) {
        if (hideBarsWhenStripShownEnabled() && (layerResource->m_current.exclusive > 0 || shouldHideLayerSurfaceNamespace(layer, hideBarNamespaces())))
            return true;

        return hideOverviewLayersEnabled() && shouldHideLayerSurfaceNamespace(layer, hideOverviewLayerNamespaces());
    }

    return hideOverviewLayersEnabled() && m_state.collectionPolicy.requestedScope == ScopeOverride::ForceAll &&
        shouldHideLayerSurfaceNamespace(layer, hideOverviewLayerNamespaces());
}

bool OverviewController::shouldHideLayerSurfaceNamespace(const PHLLS& layer, const std::string& namespaces) const {
    if (!layer)
        return false;

    const std::string layerNamespace = trimCopy(layer->m_namespace);
    if (layerNamespace.empty())
        return false;

    for (const auto& configuredNamespace : splitCommaTokens(namespaces)) {
        if (!configuredNamespace.empty() && configuredNamespace == layerNamespace)
            return true;
    }

    return false;
}

void OverviewController::renderLayerHook(void* rendererThisptr, PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& now, bool popups, bool lockscreen) {
    if (!m_renderLayerOriginal)
        return;

    if (layer && layer == m_layerSnapshotCaptureLayer) {
        m_renderLayerOriginal(rendererThisptr, layer, monitor, now, popups, lockscreen);
        return;
    }

    if (!lockscreen && shouldHideLayerSurface(layer, monitor)) {
        const bool directNiriHandoff = usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state);
        if (directNiriHandoff && m_deactivatePending) {
            // Keep native wallpaper/layout layers hidden until deactivate() fully
            // unhooks the overview.  The final native-geometry overview frames
            // already draw the proxy at desktop size; letting Hyprland draw the
            // real layer in the same handoff frame creates a visible flash.
            return;
        }
        if (m_deactivatePending && !isNiriWallpaperLayer(layer, monitor) && !isRetainedNiriWallpaperLayoutLayer(layer, monitor)) {
            m_renderLayerOriginal(rendererThisptr, layer, monitor, now, popups, lockscreen);
            return;
        }
        if (!hideBarAnimationEffectsEnabled())
            return;
        if (shouldRenderHiddenStripLayerProxy(layer, monitor))
            return;
        return;
    }

    m_renderLayerOriginal(rendererThisptr, layer, monitor, now, popups, lockscreen);
}

void OverviewController::borderDrawHook(void* borderDecorationThisptr, const PHLMONITOR& monitor, const float& alpha) {
    if (!m_borderDrawOriginal) {
        return;
    }

    const auto window = g_pHyprRenderer->m_renderData.currentWindow.lock();
    const auto stateHasVisibleEmptyPlaceholderOnMonitor = [&](const State& state, WORKSPACEID workspaceId = WORKSPACE_INVALID) {
        if (!state.collectionPolicy.onlyActiveWorkspace)
            return false;

        return std::ranges::any_of(state.emptyWorkspacePlaceholders, [&](const EmptyWorkspacePlaceholder& placeholder) {
            if (placeholder.backingOnly || !placeholder.monitor || placeholder.monitor != monitor)
                return false;

            return workspaceId == WORKSPACE_INVALID || placeholder.workspaceId == workspaceId;
        });
    };
    const auto stateIsCenteredEmptyOnMonitor = [&](const State& state) {
        if (!state.collectionPolicy.onlyActiveWorkspace || !state.windows.empty())
            return false;

        if (const auto* placeholder = centeredEmptyWorkspacePlaceholder(state); placeholder && !placeholder->backingOnly && placeholder->monitor && placeholder->monitor == monitor)
            return true;

        return stateHasVisibleEmptyPlaceholderOnMonitor(state);
    };
    const auto closingTargetsEmptyNiriWorkspaceOnMonitor = [&]() {
        if (!monitor || !isVisible() || !ownsMonitor(monitor) || !niriModeEnabled() || !m_state.collectionPolicy.onlyActiveWorkspace)
            return false;

        if (m_state.pendingExitFocus)
            return false;

        const bool closingNow = m_beginCloseInProgress || m_deactivatePending || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle;
        if (closingNow) {
            if (m_state.pendingExitWorkspace) {
                const auto workspaceMonitor = m_state.pendingExitWorkspace->m_monitor.lock();
                if (!workspaceMonitor || workspaceMonitor == monitor)
                    return true;
            }

            if (stateIsCenteredEmptyOnMonitor(m_state))
                return true;
        }

        if (!m_workspaceTransition.active || !m_workspaceTransition.monitor || m_workspaceTransition.monitor != monitor)
            return false;

        if (m_workspaceTransition.targetWorkspaceSyntheticEmpty)
            return true;

        if (stateIsCenteredEmptyOnMonitor(m_workspaceTransition.targetState))
            return true;

        return stateHasVisibleEmptyPlaceholderOnMonitor(m_workspaceTransition.targetState, m_workspaceTransition.targetWorkspaceId);
    };
    const bool emptyNiriExitOnMonitor = closingTargetsEmptyNiriWorkspaceOnMonitor();
    const auto shouldSuppressStaleEmptyNiriDecoration = [&]() {
        if (!window || !monitor || !isVisible() || !ownsMonitor(monitor) || !niriModeEnabled() || !m_state.collectionPolicy.onlyActiveWorkspace)
            return false;

        if (window->m_pinned || window->onSpecialWorkspace())
            return false;

        if (emptyNiriExitOnMonitor)
            return true;

        if (renderableManagedWindowFor(window, monitor))
            return false;

        return stateIsCenteredEmptyOnMonitor(m_state);
    };

    if (shouldSuppressStaleEmptyNiriDecoration()) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] suppress stale native border on empty niri workspace window=" << debugWindowLabel(window)
                << " monitor=" << monitor->m_name;
            debugLog(out.str());
        }
        return;
    }

    if (!window || !monitor || !isVisible() || !ownsMonitor(monitor) || !renderableManagedWindowFor(window, monitor)) {
        m_borderDrawOriginal(borderDecorationThisptr, monitor, alpha);
        return;
    }
}

void OverviewController::shadowDrawHook(void* shadowDecorationThisptr, const PHLMONITOR& monitor, const float& alpha) {
    if (!m_shadowDrawOriginal) {
        return;
    }

    const auto window = g_pHyprRenderer->m_renderData.currentWindow.lock();
    const auto stateHasVisibleEmptyPlaceholderOnMonitor = [&](const State& state, WORKSPACEID workspaceId = WORKSPACE_INVALID) {
        if (!state.collectionPolicy.onlyActiveWorkspace)
            return false;

        return std::ranges::any_of(state.emptyWorkspacePlaceholders, [&](const EmptyWorkspacePlaceholder& placeholder) {
            if (placeholder.backingOnly || !placeholder.monitor || placeholder.monitor != monitor)
                return false;

            return workspaceId == WORKSPACE_INVALID || placeholder.workspaceId == workspaceId;
        });
    };
    const auto stateIsCenteredEmptyOnMonitor = [&](const State& state) {
        if (!state.collectionPolicy.onlyActiveWorkspace || !state.windows.empty())
            return false;

        if (const auto* placeholder = centeredEmptyWorkspacePlaceholder(state); placeholder && !placeholder->backingOnly && placeholder->monitor && placeholder->monitor == monitor)
            return true;

        return stateHasVisibleEmptyPlaceholderOnMonitor(state);
    };
    const auto closingTargetsEmptyNiriWorkspaceOnMonitor = [&]() {
        if (!monitor || !isVisible() || !ownsMonitor(monitor) || !niriModeEnabled() || !m_state.collectionPolicy.onlyActiveWorkspace)
            return false;

        if (m_state.pendingExitFocus)
            return false;

        const bool closingNow = m_beginCloseInProgress || m_deactivatePending || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle;
        if (closingNow) {
            if (m_state.pendingExitWorkspace) {
                const auto workspaceMonitor = m_state.pendingExitWorkspace->m_monitor.lock();
                if (!workspaceMonitor || workspaceMonitor == monitor)
                    return true;
            }

            if (stateIsCenteredEmptyOnMonitor(m_state))
                return true;
        }

        if (!m_workspaceTransition.active || !m_workspaceTransition.monitor || m_workspaceTransition.monitor != monitor)
            return false;

        if (m_workspaceTransition.targetWorkspaceSyntheticEmpty)
            return true;

        if (stateIsCenteredEmptyOnMonitor(m_workspaceTransition.targetState))
            return true;

        return stateHasVisibleEmptyPlaceholderOnMonitor(m_workspaceTransition.targetState, m_workspaceTransition.targetWorkspaceId);
    };
    const bool emptyNiriExitOnMonitor = closingTargetsEmptyNiriWorkspaceOnMonitor();
    const auto shouldSuppressStaleEmptyNiriDecoration = [&]() {
        if (!window || !monitor || !isVisible() || !ownsMonitor(monitor) || !niriModeEnabled() || !m_state.collectionPolicy.onlyActiveWorkspace)
            return false;

        if (window->m_pinned || window->onSpecialWorkspace())
            return false;

        if (emptyNiriExitOnMonitor)
            return true;

        if (renderableManagedWindowFor(window, monitor))
            return false;

        return stateIsCenteredEmptyOnMonitor(m_state);
    };

    if (shouldSuppressStaleEmptyNiriDecoration()) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] suppress stale native shadow on empty niri workspace window=" << debugWindowLabel(window)
                << " monitor=" << monitor->m_name;
            debugLog(out.str());
        }
        return;
    }

    if (!window || !monitor || !isVisible() || !ownsMonitor(monitor) || !renderableManagedWindowFor(window, monitor)) {
        m_shadowDrawOriginal(shadowDecorationThisptr, monitor, alpha);
        return;
    }
}

void OverviewController::calculateUVForSurfaceHook(const PHLWINDOW& window, SP<CWLSurfaceResource> surface, const PHLMONITOR& monitor, bool main, const Vector2D& projSize,
                                                   const Vector2D& projSizeUnscaled, bool fixMisalignedFSV1) {
    if (!m_calculateUVForSurfaceOriginal)
        return;

    Vector2D adjustedProjSize = projSize;
    Vector2D adjustedProjSizeUnscaled = projSizeUnscaled;
    bool     adjusted = false;

    if (isVisible() && window && surface && monitor && ownsMonitor(monitor) && renderableManagedWindowFor(window, monitor) && !window->m_isX11) {
        const auto expected = expectedSurfaceSizeForUV(window, surface, monitor, main);
        const bool windowSizeMisalign = main && window->wlSurface() && window->wlSurface()->resource() &&
            window->getReportedSize() != window->wlSurface()->resource()->m_current.size;
        const bool projTooSmall = expected && (projSize.x + 1.0 < expected->x || projSize.y + 1.0 < expected->y);
        const bool projTooLargeWhileMisaligned = expected && windowSizeMisalign && (projSize.x > expected->x + 1.0 || projSize.y > expected->y + 1.0);
        if (projTooSmall || projTooLargeWhileMisaligned) {
            adjustedProjSize = *expected;
            if (monitor->m_scale > 0.0)
                adjustedProjSizeUnscaled = *expected / monitor->m_scale;
            adjusted = true;
        }

        if (debugSurfaceLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] uv " << debugWindowLabel(window) << " main=" << main << " proj=" << vectorToString(projSize)
                << " projUnscaled=" << vectorToString(projSizeUnscaled);
            if (expected)
                out << " expected=" << vectorToString(*expected);
            else
                out << " expected=<none>";
            if (adjusted)
                out << " adjustedProj=" << vectorToString(adjustedProjSize) << " adjustedProjUnscaled=" << vectorToString(adjustedProjSizeUnscaled);
            out << " fixMisaligned=" << fixMisalignedFSV1;
            debugSurfaceLog(out.str());
        }
    }

    m_calculateUVForSurfaceOriginal(g_pHyprRenderer.get(), window, std::move(surface), monitor, main, adjustedProjSize, adjustedProjSizeUnscaled, fixMisalignedFSV1);
}

SDispatchResult OverviewController::fullscreenDispatcherHook(std::string args) {
    return runHookedDispatcher(PostCloseDispatcher::Fullscreen, std::move(args));
}

SDispatchResult OverviewController::fullscreenStateDispatcherHook(std::string args) {
    return runHookedDispatcher(PostCloseDispatcher::FullscreenState, std::move(args));
}

SDispatchResult OverviewController::changeWorkspaceDispatcherHook(std::string args) {
    if (!m_changeWorkspaceOriginal)
        return {};

    if (shouldBlockWorkspaceSwitchInOverview()) {
        if (debugLogsEnabled())
            debugLog("[hymission] block changeworkspace during multi-workspace overview");
        return {};
    }

    if (allowsWorkspaceSwitchInOverview()) {
        const auto result = startOverviewWorkspaceTransitionForDispatcher(args, false);
        if (!result.success &&
            (result.error == "overview workspace transition does not support special workspaces" ||
             result.error == "overview workspace transition requires workspace on current monitor"))
            return m_changeWorkspaceOriginal(std::move(args));
        return result;
    }

    return m_changeWorkspaceOriginal(std::move(args));
}

SDispatchResult OverviewController::focusWorkspaceOnCurrentMonitorDispatcherHook(std::string args) {
    if (!m_focusWorkspaceOnCurrentMonitorOriginal)
        return {};

    if (shouldBlockWorkspaceSwitchInOverview()) {
        if (debugLogsEnabled())
            debugLog("[hymission] block focusWorkspaceOnCurrentMonitor during multi-workspace overview");
        return {};
    }

    if (allowsWorkspaceSwitchInOverview()) {
        const auto result = startOverviewWorkspaceTransitionForDispatcher(args, true);
        if (!result.success && result.error == "focusWorkspaceOnCurrentMonitor workspace is on another monitor")
            return m_focusWorkspaceOnCurrentMonitorOriginal(std::move(args));
        return result;
    }

    return m_focusWorkspaceOnCurrentMonitorOriginal(std::move(args));
}



void OverviewController::workspaceSwipeBeginHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureBegin& e) {
    if (!m_workspaceSwipeBeginOriginal)
        return;

    if (shouldBlockWorkspaceSwitchInOverview()) {
        if (debugLogsEnabled())
            debugLog("[hymission] block workspace swipe begin during multi-workspace overview");
        return;
    }

    if (allowsWorkspaceSwitchInOverview()) {
        const auto direction = e.direction != TRACKPAD_GESTURE_DIR_NONE ?
            e.direction :
            (workspaceSwipeUsesVerticalAxis(activeLayoutWorkspace()) ? TRACKPAD_GESTURE_DIR_VERTICAL : TRACKPAD_GESTURE_DIR_HORIZONTAL);
        if (beginOverviewWorkspaceSwipeGesture(direction))
            updateOverviewWorkspaceSwipeGesture(swipeDistanceForDirection(e));
        else if (debugLogsEnabled())
            debugLog("[hymission] consume native workspace swipe begin during active-workspace overview");
        return;
    }

    m_workspaceSwipeBeginOriginal(gestureThisptr, e);
}

void OverviewController::workspaceSwipeUpdateHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (!m_workspaceSwipeUpdateOriginal)
        return;

    if (m_workspaceSwipeGesture.active) {
        updateOverviewWorkspaceSwipeGesture(swipeDistanceForDirection(e));
        return;
    }

    if (shouldBlockWorkspaceSwitchInOverview())
        return;

    if (allowsWorkspaceSwitchInOverview())
        return;

    m_workspaceSwipeUpdateOriginal(gestureThisptr, e);
}

void OverviewController::workspaceSwipeEndHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (!m_workspaceSwipeEndOriginal)
        return;

    if (m_workspaceSwipeGesture.active) {
        endOverviewWorkspaceSwipeGesture(e.swipe ? e.swipe->cancelled : true);
        return;
    }

    if (shouldBlockWorkspaceSwitchInOverview())
        return;

    if (allowsWorkspaceSwitchInOverview())
        return;

    m_workspaceSwipeEndOriginal(gestureThisptr, e);
}

void OverviewController::unifiedWorkspaceSwipeBeginHook(void* gestureThisptr) {
    if (!m_unifiedWorkspaceSwipeBeginOriginal)
        return;

    if (shouldBlockWorkspaceSwitchInOverview()) {
        if (debugLogsEnabled())
            debugLog("[hymission] block unified workspace swipe begin during multi-workspace overview");
        return;
    }

    if (allowsWorkspaceSwitchInOverview()) {
        const auto direction = workspaceSwipeUsesVerticalAxis(activeLayoutWorkspace()) ? TRACKPAD_GESTURE_DIR_VERTICAL : TRACKPAD_GESTURE_DIR_HORIZONTAL;
        if (!beginOverviewWorkspaceSwipeGesture(direction) && debugLogsEnabled())
            debugLog("[hymission] consume unified workspace swipe begin during active-workspace overview");
        return;
    }

    m_unifiedWorkspaceSwipeBeginOriginal(gestureThisptr);
}

void OverviewController::unifiedWorkspaceSwipeUpdateHook(void* gestureThisptr, double delta) {
    if (!m_unifiedWorkspaceSwipeUpdateOriginal)
        return;

    if (m_workspaceSwipeGesture.active) {
        setOverviewWorkspaceSwipeGestureDelta(delta);
        return;
    }

    if (shouldBlockWorkspaceSwitchInOverview() || allowsWorkspaceSwitchInOverview())
        return;

    m_unifiedWorkspaceSwipeUpdateOriginal(gestureThisptr, delta);
}

void OverviewController::unifiedWorkspaceSwipeEndHook(void* gestureThisptr) {
    if (!m_unifiedWorkspaceSwipeEndOriginal)
        return;

    if (m_workspaceSwipeGesture.active) {
        endOverviewWorkspaceSwipeGesture(false);
        return;
    }

    if (shouldBlockWorkspaceSwitchInOverview() || allowsWorkspaceSwitchInOverview())
        return;

    m_unifiedWorkspaceSwipeEndOriginal(gestureThisptr);
}

bool OverviewController::handleTouchDown(const ITouch::SDownEvent& event) {
    if (!isVisible() || m_state.phase != Phase::Active || getConfigInt(m_handle, "gestures:workspace_swipe_touch", 0) == 0)
        return false;

    PHLMONITOR monitor;
    if (event.device && !event.device->m_boundOutput.empty())
        monitor = g_pCompositor->getMonitorFromName(event.device->m_boundOutput);
    if (!monitor)
        monitor = Desktop::focusState()->monitor();
    if (!monitor || !containsHandle(m_state.participatingMonitors, monitor))
        monitor = m_state.ownerMonitor;
    if (!monitor)
        return false;

    const auto workspace = monitor->m_activeWorkspace ? monitor->m_activeWorkspace : activeLayoutWorkspace();
    const bool vertical = workspaceSwipeUsesVerticalAxis(workspace);
    const double primary = vertical ? event.pos.y : event.pos.x;
    constexpr double TOUCH_EDGE_THRESHOLD = 0.08;
    if (primary > TOUCH_EDGE_THRESHOLD && primary < 1.0 - TOUCH_EDGE_THRESHOLD)
        return false;

    if (shouldBlockWorkspaceSwitchInOverview())
        return true;

    if (!allowsWorkspaceSwitchInOverview())
        return false;

    if (m_gestureSession.active || m_workspaceSwipeGesture.active || m_workspaceTransition.active)
        return true;

    if (!beginOverviewWorkspaceSwipeGesture(vertical ? TRACKPAD_GESTURE_DIR_VERTICAL : TRACKPAD_GESTURE_DIR_HORIZONTAL))
        return true;

    m_workspaceSwipeGesture.monitor = monitor;
    m_workspaceSwipeGesture.touchActive = true;
    m_workspaceSwipeGesture.touchId = event.touchID;
    m_workspaceSwipeGesture.touchVertical = vertical;
    m_workspaceSwipeGesture.touchFromHighEdge = primary > 0.5;
    m_workspaceSwipeGesture.gestureDelta = 0.0;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview touch workspace swipe begin monitor=" << monitor->m_name << " edge="
            << (m_workspaceSwipeGesture.touchFromHighEdge ? "high" : "low") << " axis=" << (vertical ? "vertical" : "horizontal");
        debugLog(out.str());
    }

    return true;
}

bool OverviewController::handleTouchMotion(const ITouch::SMotionEvent& event) {
    if (!m_workspaceSwipeGesture.active || !m_workspaceSwipeGesture.touchActive || event.touchID != m_workspaceSwipeGesture.touchId)
        return false;

    const double primary = m_workspaceSwipeGesture.touchVertical ? event.pos.y : event.pos.x;
    const double gestureDistance = gestureSwipeDistance();
    const bool touchInvert = getConfigInt(m_handle, "gestures:workspace_swipe_touch_invert", 0) != 0;

    double adjustedDelta = 0.0;
    if (m_workspaceSwipeGesture.touchFromHighEdge)
        adjustedDelta = gestureDistance * (touchInvert ? primary - 1.0 : 1.0 - primary);
    else
        adjustedDelta = gestureDistance * (touchInvert ? primary : -primary);

    setOverviewWorkspaceSwipeGestureDelta(adjustedDelta);
    return true;
}

bool OverviewController::handleTouchUp(const ITouch::SUpEvent& event, bool cancelled) {
    if (!m_workspaceSwipeGesture.active || !m_workspaceSwipeGesture.touchActive || event.touchID != m_workspaceSwipeGesture.touchId)
        return false;

    m_workspaceSwipeGesture.touchActive = false;
    endOverviewWorkspaceSwipeGesture(cancelled);
    return true;
}




std::vector<UP<IPassElement>> OverviewController::surfaceDrawHook(void* surfacePassThisptr) {
    if (!m_surfaceDrawOriginal) {
        return {};
    }

    if (m_surfaceRenderDataTransformDepth > 0) {
        return m_surfaceDrawOriginal(surfacePassThisptr);
    }

    if ((usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state)) &&
        (m_deactivatePending || directNiriNativeHandoffActive())) {
        if (auto* handoffRenderData = surfaceRenderDataMutable(surfacePassThisptr); handoffRenderData && handoffRenderData->pWindow) {
            const auto handoffMonitor = handoffRenderData->pMonitor.lock();
            if (handoffMonitor && ownsMonitor(handoffMonitor) && renderableManagedWindowFor(handoffRenderData->pWindow, handoffMonitor)) {
                const bool savedBlur = handoffRenderData->blur;
                const bool savedBlockBlurOptimization = handoffRenderData->blockBlurOptimization;
                handoffRenderData->blur = false;
                handoffRenderData->blockBlurOptimization = true;
                auto result = m_surfaceDrawOriginal(surfacePassThisptr);
                handoffRenderData->blur = savedBlur;
                handoffRenderData->blockBlurOptimization = savedBlockBlurOptimization;
                return result;
            }
        }
    }

    CSurfacePassElement::SRenderData* renderData = nullptr;
    PHLMONITOR                        monitor;
    SurfaceRenderDataSnapshot        snapshot;
    if (!prepareSurfaceRenderData(surfacePassThisptr, "draw", renderData, monitor, snapshot)) {
        return m_surfaceDrawOriginal(surfacePassThisptr);
    }

    ++m_surfaceRenderDataTransformDepth;
    auto result = m_surfaceDrawOriginal(surfacePassThisptr);
    --m_surfaceRenderDataTransformDepth;
    restoreSurfaceRenderData(renderData, snapshot);
    return result;
}

bool OverviewController::surfaceNeedsLiveBlurHook(void* surfacePassThisptr) {
    if (!m_surfaceNeedsLiveBlurOriginal)
        return false;

    if (suppressSurfaceBlur(surfacePassThisptr))
        return false;

    auto* renderData = surfaceRenderDataMutable(surfacePassThisptr);
    auto  monitor = renderData ? renderData->pMonitor.lock() : PHLMONITOR{};
    if (!renderData || !renderData->pWindow || !monitor || !isVisible() || !ownsMonitor(monitor) ||
        !renderableManagedWindowFor(renderData->pWindow, monitor))
        return m_surfaceNeedsLiveBlurOriginal(surfacePassThisptr);

    if ((usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state)) &&
        (m_deactivatePending || directNiriNativeHandoffActive())) {
        return m_surfaceNeedsLiveBlurOriginal(surfacePassThisptr);
    }

    const float savedAlpha = renderData->alpha;
    renderData->alpha = managedPreviewAlphaFor(renderData->pWindow, savedAlpha);
    const bool needsBlur = m_surfaceNeedsLiveBlurOriginal(surfacePassThisptr);
    renderData->alpha = savedAlpha;
    return needsBlur;
}

bool OverviewController::surfaceNeedsPrecomputeBlurHook(void* surfacePassThisptr) {
    if (!m_surfaceNeedsPrecomputeBlurOriginal)
        return false;

    if (suppressSurfaceBlur(surfacePassThisptr))
        return false;

    auto* renderData = surfaceRenderDataMutable(surfacePassThisptr);
    auto  monitor = renderData ? renderData->pMonitor.lock() : PHLMONITOR{};
    if (!renderData || !renderData->pWindow || !monitor || !isVisible() || !ownsMonitor(monitor) ||
        !renderableManagedWindowFor(renderData->pWindow, monitor))
        return m_surfaceNeedsPrecomputeBlurOriginal(surfacePassThisptr);

    if ((usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state)) &&
        (m_deactivatePending || directNiriNativeHandoffActive())) {
        return m_surfaceNeedsPrecomputeBlurOriginal(surfacePassThisptr);
    }

    const float savedAlpha = renderData->alpha;
    renderData->alpha = managedPreviewAlphaFor(renderData->pWindow, savedAlpha);
    const bool needsBlur = m_surfaceNeedsPrecomputeBlurOriginal(surfacePassThisptr);
    renderData->alpha = savedAlpha;
    return needsBlur;
}

CBox OverviewController::surfaceTexBoxHook(void* surfacePassThisptr) {
    if (!m_surfaceTexBoxOriginal)
        return {};

    if (m_surfaceRenderDataTransformDepth > 0) {
        CBox box = m_surfaceTexBoxOriginal(surfacePassThisptr);
        auto* renderData = surfaceRenderDataMutable(surfacePassThisptr);
        auto  monitor = renderData ? renderData->pMonitor.lock() : PHLMONITOR{};
        if (renderData && monitor)
            adjustTransformedSurfaceBoxSize(*renderData, monitor, box);
        return box;
    }

    CSurfacePassElement::SRenderData* renderData = nullptr;
    PHLMONITOR                        monitor;
    SurfaceRenderDataSnapshot        snapshot;
    if (!prepareSurfaceRenderData(surfacePassThisptr, "texbox", renderData, monitor, snapshot))
        return m_surfaceTexBoxOriginal(surfacePassThisptr);

    ++m_surfaceRenderDataTransformDepth;
    CBox box = m_surfaceTexBoxOriginal(surfacePassThisptr);
    adjustTransformedSurfaceBoxSize(*renderData, monitor, box);
    --m_surfaceRenderDataTransformDepth;
    restoreSurfaceRenderData(renderData, snapshot);
    return box;
}

std::optional<CBox> OverviewController::surfaceBoundingBoxHook(void* surfacePassThisptr) {
    if (!m_surfaceBoundingBoxOriginal)
        return {};

    if (m_surfaceRenderDataTransformDepth > 0)
        return m_surfaceBoundingBoxOriginal(surfacePassThisptr);

    CSurfacePassElement::SRenderData* renderData = nullptr;
    PHLMONITOR                        monitor;
    SurfaceRenderDataSnapshot        snapshot;
    if (!prepareSurfaceRenderData(surfacePassThisptr, "boundingBox", renderData, monitor, snapshot))
        return m_surfaceBoundingBoxOriginal(surfacePassThisptr);

    ++m_surfaceRenderDataTransformDepth;
    const auto box = m_surfaceBoundingBoxOriginal(surfacePassThisptr);
    --m_surfaceRenderDataTransformDepth;
    restoreSurfaceRenderData(renderData, snapshot);
    return box;
}

CRegion OverviewController::surfaceOpaqueRegionHook(void* surfacePassThisptr) {
    if (!m_surfaceOpaqueRegionOriginal)
        return {};

    if (m_surfaceRenderDataTransformDepth > 0)
        return m_surfaceOpaqueRegionOriginal(surfacePassThisptr);

    CSurfacePassElement::SRenderData* renderData = nullptr;
    PHLMONITOR                        monitor;
    SurfaceRenderDataSnapshot        snapshot;
    if (!prepareSurfaceRenderData(surfacePassThisptr, "opaqueRegion", renderData, monitor, snapshot))
        return m_surfaceOpaqueRegionOriginal(surfacePassThisptr);

    // Overview already damages the full monitor while animating, and the transformed preview
    // geometry is temporary. Returning an empty opaque region avoids pass simplification
    // incorrectly occluding lower previews and causing one-frame flashes.
    if (isVisible() && monitor && ownsMonitor(monitor) && renderableManagedWindowFor(renderData->pWindow, monitor)) {
        restoreSurfaceRenderData(renderData, snapshot);
        return {};
    }

    ++m_surfaceRenderDataTransformDepth;
    const CRegion region = m_surfaceOpaqueRegionOriginal(surfacePassThisptr);
    --m_surfaceRenderDataTransformDepth;
    restoreSurfaceRenderData(renderData, snapshot);
    return region;
}

CRegion OverviewController::surfaceVisibleRegionHook(void* surfacePassThisptr, bool& cancel) {
    if (!m_surfaceVisibleRegionOriginal)
        return {};

    if (m_surfaceRenderDataTransformDepth > 0)
        return m_surfaceVisibleRegionOriginal(surfacePassThisptr, cancel);

    CSurfacePassElement::SRenderData* renderData = nullptr;
    PHLMONITOR                        monitor;
    SurfaceRenderDataSnapshot        snapshot;
    if (!prepareSurfaceRenderData(surfacePassThisptr, "visibleRegion", renderData, monitor, snapshot))
        return m_surfaceVisibleRegionOriginal(surfacePassThisptr, cancel);

    ++m_surfaceRenderDataTransformDepth;
    CBox fullBox = m_surfaceTexBoxOriginal(surfacePassThisptr);
    adjustTransformedSurfaceBoxSize(*renderData, monitor, fullBox);
    --m_surfaceRenderDataTransformDepth;
    fullBox.scale(monitor->m_scale);
    fullBox.round();
    cancel = fullBox.width <= 0.0 || fullBox.height <= 0.0;
    restoreSurfaceRenderData(renderData, snapshot);
    return cancel ? CRegion{} : CRegion(fullBox);
}

LayoutConfig OverviewController::loadLayoutConfig() const {
    const double outerPadding = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding", 32));
    return {
        .engine = parseLayoutEngine(getConfigString(m_handle, "plugin:hymission:layout_engine", "grid")),
        .outerPaddingTop = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_top", static_cast<long>(outerPadding))),
        .outerPaddingRight = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_right", static_cast<long>(outerPadding))),
        .outerPaddingBottom = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_bottom", static_cast<long>(outerPadding))),
        .outerPaddingLeft = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_left", static_cast<long>(outerPadding))),
        .rowSpacing = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:row_spacing", 32)),
        .columnSpacing = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:column_spacing", 32)),
        .smallWindowBoost = getConfigFloat(m_handle, "plugin:hymission:small_window_boost", 1.35),
        .maxPreviewScale = getConfigFloat(m_handle, "plugin:hymission:max_preview_scale", 0.95),
        .minWindowLength = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:min_window_length", 120)),
        .minPreviewShortEdge = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:min_preview_short_edge", 32)),
        .layoutSpaceWeight = getConfigFloat(m_handle, "plugin:hymission:layout_space_weight", 0.10),
        .layoutScaleWeight = getConfigFloat(m_handle, "plugin:hymission:layout_scale_weight", 1.0),
        .minSlotScale = getConfigFloat(m_handle, "plugin:hymission:min_slot_scale", 0.10),
        .naturalScaleFlex = getConfigFloat(m_handle, "plugin:hymission:natural_scale_flex", 0.22),
    };
}

LayoutConfig OverviewController::layoutConfigForState(const State& state) const {
    LayoutConfig config = loadLayoutConfig();
    if (state.collectionPolicy.onlyActiveWorkspace)
        config.maxPreviewScale = getConfigFloat(m_handle, "plugin:hymission:workspace_overview_max_preview_scale", config.maxPreviewScale);
    return config;
}

OverviewController::CollectionPolicy OverviewController::loadCollectionPolicy(ScopeOverride requestedScope) const {
    if (requestedScope == ScopeOverride::OnlyCurrentWorkspace) {
        return {
            .requestedScope = requestedScope,
            .onlyActiveWorkspace = true,
            .onlyActiveMonitor = true,
            .includeSpecial = false,
        };
    }

    if (requestedScope == ScopeOverride::ForceAll) {
        return {
            .requestedScope = requestedScope,
            .onlyActiveWorkspace = false,
            .onlyActiveMonitor = false,
            .includeSpecial = true,
        };
    }

    return {
        .requestedScope = requestedScope,
        .onlyActiveWorkspace = getConfigInt(m_handle, "plugin:hymission:only_active_workspace", 0) != 0,
        .onlyActiveMonitor = getConfigInt(m_handle, "plugin:hymission:only_active_monitor", 0) != 0,
        .includeSpecial = getConfigInt(m_handle, "plugin:hymission:show_special", 0) != 0,
    };
}

std::optional<OverviewController::ScopeOverride> OverviewController::parseScopeOverride(const std::string& args, std::string& error) const {
    const std::string trimmed = trimCopy(args);
    if (trimmed.empty())
        return ScopeOverride::Default;

    if (trimmed == "onlycurrentworkspace")
        return ScopeOverride::OnlyCurrentWorkspace;
    if (trimmed == "forceall")
        return ScopeOverride::ForceAll;

    error = "invalid overview scope: " + trimmed;
    return std::nullopt;
}

bool OverviewController::expandSelectedWindowEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:expand_selected_window", 1) != 0;
}

bool OverviewController::multiWorkspaceExpandSelectedWindowEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:multi_workspace_expand_selected_window", 1) != 0;
}

bool OverviewController::focusFollowsMouseEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:overview_focus_follows_mouse", 1) != 0;
}

bool OverviewController::refreshPreviewsOnConfigReloadEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:refresh_previews_on_config_reload", 1) != 0;
}

int OverviewController::stripThemeSurfaceFeedbackFrames() const {
    return getConfigInt(m_handle, "plugin:hymission:strip_theme_surface_feedback_frames", 300);
}

bool OverviewController::stripThemeWorkspaceActivationRefreshEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:strip_theme_workspace_activation_refresh", 1) != 0;
}

std::string OverviewController::stripThemeWorkspaceActivationRefreshClasses() const {
    return getConfigString(m_handle, "plugin:hymission:strip_theme_workspace_activation_refresh_classes", "kitty");
}

bool OverviewController::workspaceNeedsThemeActivationRefresh(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return false;

    const auto configuredClasses = splitCommaTokens(stripThemeWorkspaceActivationRefreshClasses());
    std::vector<std::string> normalizedClasses;
    normalizedClasses.reserve(configuredClasses.size());
    for (const auto& configuredClass : configuredClasses) {
        const auto normalizedClass = asciiLowerCopy(trimCopy(configuredClass));
        if (!normalizedClass.empty())
            normalizedClasses.push_back(normalizedClass);
    }

    if (normalizedClasses.empty())
        return false;
    if (std::ranges::find(normalizedClasses, "*") != normalizedClasses.end())
        return true;

    for (const auto& window : g_pCompositor->m_windows) {
        if (!window || !window->m_isMapped || window->isHidden() || window->m_workspace != workspace)
            continue;

        const auto windowClass = asciiLowerCopy(window->m_class);
        if (std::ranges::find(normalizedClasses, windowClass) != normalizedClasses.end())
            return true;
    }

    return false;
}

bool OverviewController::multiWorkspaceSortRecentFirstEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:multi_workspace_sort_recent_first", 1) != 0;
}

bool OverviewController::toggleSwitchModeEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:toggle_switch_mode", 1) != 0;
}

bool OverviewController::switchToggleAutoNextEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:switch_toggle_auto_next", 1) != 0;
}

std::string OverviewController::switchReleaseKeyConfig() const {
    return getConfigString(m_handle, "plugin:hymission:switch_release_key", "Super_L");
}

bool OverviewController::gestureInvertVerticalEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:gesture_invert_vertical", 0) != 0;
}

bool OverviewController::workspaceSwipeInvertEnabled() const {
    return getConfigInt(m_handle, "gestures:workspace_swipe_invert", 0) != 0;
}

bool OverviewController::workspaceChangeKeepsOverviewEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:workspace_change_keeps_overview", 1) != 0;
}

bool OverviewController::damageTrackingOverrideEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:damage_tracking_override", 1) != 0;
}

bool OverviewController::closeSpecialWorkspacesOnOpenEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:close_special_workspaces_on_open", 1) != 0;
}

std::chrono::milliseconds OverviewController::postCloseCrossScopeDebounce() const {
    return std::chrono::milliseconds(std::clamp<long>(getConfigInt(m_handle, "plugin:hymission:post_close_cross_scope_debounce_ms", 0), 0, 2000));
}

bool OverviewController::hideBarsWhenStripShownEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:hide_bar_when_strip", 1) != 0;
}

std::string OverviewController::hideBarNamespaces() const {
    return getConfigString(m_handle, "plugin:hymission:hide_bar_namespaces", DEFAULT_HIDE_BAR_NAMESPACES);
}

bool OverviewController::hideOverviewLayersEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:hide_layers_when_overview", 1) != 0;
}

std::string OverviewController::hideOverviewLayerNamespaces() const {
    return getConfigString(m_handle, "plugin:hymission:hide_overview_layer_namespaces", DEFAULT_HIDE_OVERVIEW_LAYER_NAMESPACES);
}

bool OverviewController::hideBarAnimationEffectsEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:hide_bar_animation", 1) != 0;
}

bool OverviewController::hideBarAnimationBlurEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:hide_bar_animation_blur", 1) != 0;
}

double OverviewController::hideBarAnimationMoveMultiplier() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:hide_bar_animation_move_multiplier", 0.8), 0.0, 2.0);
}

double OverviewController::hideBarAnimationScaleDivisor() const {
    return std::max(1.0, getConfigFloat(m_handle, "plugin:hymission:hide_bar_animation_scale_divisor", 1.1));
}

double OverviewController::hideBarAnimationAlphaEnd() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:hide_bar_animation_alpha_end", 0.0), 0.0, 1.0);
}

bool OverviewController::barSingleMissionControlEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:bar_single_mission_control", 0) != 0;
}

bool OverviewController::showFocusIndicatorEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:show_focus_indicator", 0) != 0;
}

double OverviewController::activeBorderWidth() const {
    return std::max(0L, getConfigInt(m_handle, "general:border_size", 2));
}

double OverviewController::inactiveBorderWidth() const {
    return std::max(0.0, activeBorderWidth() - 1.0);
}

double OverviewController::focusedBorderThicknessReduction() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:overview_focused_border_thickness_reduction", 0.25), 0.0, 32.0);
}

double OverviewController::overviewBorderRoundingScale() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:overview_border_rounding_scale", 2.35), 0.1, 4.0);
}














bool OverviewController::debugLogsEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:debug_logs", 0) != 0;
}

bool OverviewController::debugSurfaceLogsEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:debug_surface_logs", 0) != 0;
}














double OverviewController::gestureSwipeDistance() const {
    return std::max(1.0, static_cast<double>(getConfigInt(m_handle, "gestures:workspace_swipe_distance", 300)));
}

double OverviewController::gestureForceSpeedThreshold() const {
    return std::max(0.0, static_cast<double>(getConfigInt(m_handle, "gestures:workspace_swipe_min_speed_to_force", 30)));
}

bool OverviewController::gestureSwipeForeverEnabled() const {
    return getConfigInt(m_handle, "gestures:workspace_swipe_forever", 0) != 0;
}

bool OverviewController::gestureSwipeCreateNewEnabled() const {
    return getConfigInt(m_handle, "gestures:workspace_swipe_create_new", 0) != 0;
}

bool OverviewController::gestureSwipeUseRelativeEnabled() const {
    return getConfigInt(m_handle, "gestures:workspace_swipe_use_r", 0) != 0;
}

bool OverviewController::gestureSwipeDirectionLockEnabled() const {
    return getConfigInt(m_handle, "gestures:workspace_swipe_direction_lock", 0) != 0;
}

double OverviewController::gestureSwipeDirectionLockThreshold() const {
    return std::max(0.0, static_cast<double>(getConfigInt(m_handle, "gestures:workspace_swipe_direction_lock_threshold", 10)));
}

bool OverviewController::allowsWorkspaceSwitchInOverview() const {
    return isVisible() && m_state.collectionPolicy.onlyActiveWorkspace && workspaceChangeKeepsOverviewEnabled();
}

bool OverviewController::shouldBlockWorkspaceSwitchInOverview() const {
    return isVisible() && !m_state.collectionPolicy.onlyActiveWorkspace;
}

bool OverviewController::shouldOverrideWorkspaceNames(const State& state) const {
    return barSingleMissionControlEnabled() && !state.collectionPolicy.onlyActiveWorkspace;
}

std::string OverviewController::workspaceStripAnchor() const {
    switch (parseWorkspaceStripAnchor(getConfigString(m_handle, "plugin:hymission:workspace_strip_anchor", "left"))) {
        case WorkspaceStripAnchor::Left:
            return "left";
        case WorkspaceStripAnchor::Right:
            return "right";
        case WorkspaceStripAnchor::Top:
        default:
            return "top";
    }
}

WorkspaceStripEmptyMode OverviewController::workspaceStripEmptyMode() const {
    return parseWorkspaceStripEmptyMode(getConfigString(m_handle, "plugin:hymission:workspace_strip_empty_mode", "existing"));
}

double OverviewController::workspaceStripThickness(const PHLMONITOR& monitor) const {
    double raw = std::max(64.0, static_cast<double>(getConfigInt(m_handle, "plugin:hymission:workspace_strip_thickness", 160)));
    if (!monitor)
        return raw;

    const bool horizontal = workspaceStripAnchor() == "top";
    const double crossLength = horizontal ? static_cast<double>(monitor->m_size.y) : static_cast<double>(monitor->m_size.x);
    const double limit = crossLength * 0.35;
    return std::clamp(raw, 64.0, std::max(64.0, limit));
}

double OverviewController::workspaceStripGap() const {
    return std::max(0.0, static_cast<double>(getConfigInt(m_handle, "plugin:hymission:workspace_strip_gap", 24)));
}

int OverviewController::workspaceStripLabelFontSize() const {
    return static_cast<int>(std::clamp(getConfigInt(m_handle, "plugin:hymission:workspace_strip_label_font_size", 24), 8L, 96L));
}

double OverviewController::workspaceStripLabelOpacity() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:workspace_strip_label_opacity", 0.30), 0.0, 1.0);
}




bool OverviewController::workspaceStripEnabled(const State& state) const {
    return state.collectionPolicy.onlyActiveWorkspace && !state.suppressWorkspaceStrip && !shouldDisableWorkspaceStripForNiriPreview(state);
}

bool OverviewController::isStripOnlyOverviewState(const State& state) const {
    return workspaceStripEnabled(state) && state.windows.empty() && !state.stripEntries.empty();
}

bool OverviewController::shouldContinuouslyRefreshWorkspaceStripSnapshots() const {
    if (!workspaceStripEnabled(m_state))
        return false;

    if ((m_state.phase != Phase::Opening && m_state.phase != Phase::Active) || m_workspaceTransition.active)
        return false;

    // Once overview has no managed previews left, keep the strip visible but
    // stop re-rendering its snapshots every frame. The empty-state strip is
    // stable until a real workspace/window/monitor change rebuilds it.
    return !isStripOnlyOverviewState(m_state);
}

bool OverviewController::isCurrentActiveWorkspaceStripEntry(const WorkspaceStripEntry& entry) const {
    return entry.monitor && entry.workspace && entry.monitor->m_activeWorkspace && entry.workspace == entry.monitor->m_activeWorkspace;
}


double OverviewController::workspaceSwipeViewportDistance(const PHLMONITOR& monitor, WorkspaceTransitionAxis axis) const {
    if (!monitor)
        return 1.0;

    const double gaps = static_cast<double>(getConfigInt(m_handle, "general:gaps_workspaces", 0));
    return (axis == WorkspaceTransitionAxis::Vertical ? static_cast<double>(monitor->m_size.y) : static_cast<double>(monitor->m_size.x)) + gaps;
}

int OverviewController::resolveOverviewWorkspaceSwipeStep(eTrackpadGestureDirection direction, double totalDelta, double lastDelta) const {
    if (direction != TRACKPAD_GESTURE_DIR_HORIZONTAL && direction != TRACKPAD_GESTURE_DIR_VERTICAL)
        return 0;

    double adjustedTotal = totalDelta;
    double adjustedLast = lastDelta;
    if (workspaceSwipeInvertEnabled()) {
        adjustedTotal = -adjustedTotal;
        adjustedLast = -adjustedLast;
    }

    const double cancelRatio = std::clamp(getConfigFloat(m_handle, "gestures:workspace_swipe_cancel_ratio", 0.5), 0.0, 1.0);
    const double distanceThreshold = gestureSwipeDistance() * cancelRatio;
    const double speedThreshold = gestureForceSpeedThreshold();
    const double decisive = std::abs(adjustedLast) >= speedThreshold ? adjustedLast : adjustedTotal;

    if (std::abs(decisive) < 0.0001 || (std::abs(adjustedTotal) < distanceThreshold && std::abs(adjustedLast) < speedThreshold))
        return 0;

    return decisive < 0.0 ? -1 : 1;
}

bool OverviewController::resolveOverviewWorkspaceTargetByStep(const PHLMONITOR& monitor, int step, WORKSPACEID& workspaceId, std::string& workspaceName,
                                                              PHLWORKSPACE& workspace, bool& syntheticEmpty) const {
    workspaceId = WORKSPACE_INVALID;
    workspaceName.clear();
    workspace.reset();
    syntheticEmpty = false;

    if (!monitor || step == 0 || !monitor->m_activeWorkspace || monitor->m_activeWorkspace->m_isSpecialWorkspace)
        return false;

    if (niriModeAppliesToState(m_state) && !m_state.managedWorkspaces.empty()) {
        WORKSPACEID currentWorkspaceId = monitor->m_activeWorkspace->m_id;
        if (const auto* centeredPlaceholder = centeredEmptyWorkspacePlaceholder(m_state); centeredPlaceholder && centeredPlaceholder->monitor == monitor &&
            centeredPlaceholder->workspaceId != WORKSPACE_INVALID) {
            currentWorkspaceId = centeredPlaceholder->workspaceId;
        } else if (m_state.focusDuringOverview && m_state.focusDuringOverview->m_workspace &&
            m_state.focusDuringOverview->m_workspace->m_monitor.lock() == monitor) {
            currentWorkspaceId = m_state.focusDuringOverview->m_workspace->m_id;
        } else if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size()) {
            const auto selected = m_state.windows[*m_state.selectedIndex].window;
            if (selected && selected->m_workspace && selected->m_workspace->m_monitor.lock() == monitor)
                currentWorkspaceId = selected->m_workspace->m_id;
        }

        std::unordered_map<WORKSPACEID, PHLWORKSPACE> workspaceById;
        std::vector<int64_t> workspaceIds;
        std::vector<int64_t> occupiedWorkspaceIds;
        for (const auto& candidate : m_state.managedWorkspaces) {
            if (!candidate || candidate->m_isSpecialWorkspace || candidate->m_id < 0 || candidate->m_monitor.lock() != monitor)
                continue;

            workspaceById.emplace(candidate->m_id, candidate);
            workspaceIds.push_back(candidate->m_id);
        }

        for (const auto& managed : m_state.windows) {
            if (!managed.window || managed.window->m_pinned || !managed.window->m_workspace ||
                managed.window->m_workspace->m_monitor.lock() != monitor)
                continue;
            occupiedWorkspaceIds.push_back(managed.window->m_workspace->m_id);
        }
        for (const auto& placeholder : m_state.emptyWorkspacePlaceholders) {
            if (placeholder.monitor == monitor && placeholder.workspaceId != WORKSPACE_INVALID)
                workspaceIds.push_back(placeholder.workspaceId);
        }

        const auto emptyMode =
            niriModeShowEmptyWorkspacesBetweenEnabled() ? WorkspaceStripEmptyMode::Continuous : WorkspaceStripEmptyMode::Existing;
        const auto laneWorkspaceIds = niriEmptyWorkspaceLaneIds(workspaceIds, occupiedWorkspaceIds, emptyMode);
        const auto it = std::find(laneWorkspaceIds.begin(), laneWorkspaceIds.end(), static_cast<int64_t>(currentWorkspaceId));
        if (it == laneWorkspaceIds.end())
            return false;

        const long long currentIndex = static_cast<long long>(std::distance(laneWorkspaceIds.begin(), it));
        const long long targetIndex = currentIndex + (step < 0 ? -1 : 1);
        if (targetIndex < 0 || targetIndex >= static_cast<long long>(laneWorkspaceIds.size()))
            return false;

        workspaceId = static_cast<WORKSPACEID>(laneWorkspaceIds[static_cast<std::size_t>(targetIndex)]);
        const auto workspaceIt = workspaceById.find(workspaceId);
        workspace = workspaceIt == workspaceById.end() ? PHLWORKSPACE{} : workspaceIt->second;
        workspaceName = workspace ? workspace->m_name : std::to_string(workspaceId);
        syntheticEmpty = !workspace;
        return true;
    }

    const bool        useRelative = gestureSwipeUseRelativeEnabled();
    const std::string selector = step < 0 ? (useRelative ? "r-1" : "m-1") : (useRelative ? "r+1" : "m+1");
    auto              resolved = getWorkspaceIDNameFromString(selector);

    if (resolved.id == WORKSPACE_INVALID)
        return false;

    workspaceId = resolved.id;
    workspaceName = resolved.name;
    workspace = g_pCompositor->getWorkspaceByID(workspaceId);

    if (step > 0 && gestureSwipeCreateNewEnabled() && (workspaceId <= monitor->m_activeWorkspace->m_id || !workspace)) {
        auto createTarget = getWorkspaceIDNameFromString("r+1");
        if (createTarget.id == WORKSPACE_INVALID)
            return false;

        workspaceId = createTarget.id;
        workspaceName = createTarget.name.empty() ? std::to_string(createTarget.id) : createTarget.name;
        workspace = g_pCompositor->getWorkspaceByID(workspaceId);
        syntheticEmpty = !workspace;
        return true;
    }

    if (workspaceId == monitor->m_activeWorkspace->m_id)
        return false;

    if (!workspace && !gestureSwipeCreateNewEnabled())
        return false;

    syntheticEmpty = !workspace;
    return true;
}

bool OverviewController::switchOverviewWorkspaceByStep(int step) {
    PHLMONITOR monitor = Desktop::focusState()->monitor();
    if (!monitor || !containsHandle(m_state.participatingMonitors, monitor))
        monitor = m_state.ownerMonitor;
    if (!monitor)
        return false;

    return startOverviewWorkspaceTransitionByStep(monitor, step, WorkspaceTransitionMode::TimedCommit);
}

void OverviewController::restoreWorkspaceNameOverrides() {
    for (auto it = m_workspaceNameBackups.rbegin(); it != m_workspaceNameBackups.rend(); ++it) {
        if (!it->workspace)
            continue;

        it->workspace->rename(it->name);
    }

    m_workspaceNameBackups.clear();
}

void OverviewController::applyWorkspaceNameOverrides(const State& state) {
    restoreWorkspaceNameOverrides();

    if (!shouldOverrideWorkspaceNames(state))
        return;

    const auto backupAndRename = [&](const PHLWORKSPACE& workspace, const std::string& name) {
        if (!workspace || workspace->m_name == name)
            return;

        const auto alreadyBackedUp = std::ranges::any_of(m_workspaceNameBackups, [&](const WorkspaceNameBackup& backup) { return backup.workspace == workspace; });
        if (!alreadyBackedUp) {
            m_workspaceNameBackups.push_back({
                .workspace = workspace,
                .name = workspace->m_name,
            });
        }

        workspace->rename(name);
    };

    const bool collapseBar = barSingleMissionControlEnabled();
    PHLWORKSPACE primaryWorkspace;
    if (state.ownerMonitor && state.ownerMonitor->m_activeWorkspace && containsHandle(state.managedWorkspaces, state.ownerMonitor->m_activeWorkspace))
        primaryWorkspace = state.ownerMonitor->m_activeWorkspace;

    if (collapseBar && primaryWorkspace) {
        for (const auto& workspace : state.managedWorkspaces) {
            if (!workspace || workspace->m_isSpecialWorkspace || workspace == primaryWorkspace)
                continue;

            backupAndRename(workspace, std::string{MISSION_CONTROL_HIDDEN_WORKSPACE_PREFIX} + std::to_string(workspace->m_id));
        }
    }

    for (const auto& monitor : state.participatingMonitors) {
        if (!monitor || !monitor->m_activeWorkspace)
            continue;

        if (!containsHandle(state.managedWorkspaces, monitor->m_activeWorkspace))
            continue;

        if (collapseBar && primaryWorkspace && monitor->m_activeWorkspace != primaryWorkspace) {
            backupAndRename(monitor->m_activeWorkspace,
                            std::string{MISSION_CONTROL_HIDDEN_WORKSPACE_PREFIX} + std::to_string(monitor->m_activeWorkspace->m_id));
            continue;
        }

        backupAndRename(monitor->m_activeWorkspace, MISSION_CONTROL_WORKSPACE_NAME);
    }
}

void OverviewController::clearRegisteredTrackpadGestures() {
    if (!g_pTrackpadGestures)
        return;

    for (const auto& gesture : m_registeredGestures)
        (void)g_pTrackpadGestures->removeGesture(gesture.fingerCount, gesture.direction, gesture.modMask, gesture.deltaScale, gesture.disableInhibit);

    m_registeredGestures.clear();
}

void OverviewController::rememberRegisteredTrackpadGesture(const GestureRegistration& gesture) {
    std::erase_if(m_registeredGestures, [&](const GestureRegistration& existing) {
        return existing.fingerCount == gesture.fingerCount && existing.direction == gesture.direction && existing.modMask == gesture.modMask &&
            std::abs(existing.deltaScale - gesture.deltaScale) <= 0.0001F && existing.disableInhibit == gesture.disableInhibit;
    });
    m_registeredGestures.push_back(gesture);
}

void OverviewController::replaceNativeWorkspaceGestures(const char* source) {
    if (!g_pTrackpadGestures)
        return;

    std::size_t replaced = 0;
    for (const auto& gesture : g_pTrackpadGestures->m_gestures) {
        if (!gesture || !gesture->gesture || !dynamic_cast<CWorkspaceSwipeGesture*>(gesture->gesture.get()))
            continue;

        gesture->gesture = makeUnique<CHymissionWorkspaceTrackpadGesture>(gesture->direction, gesture->deltaScale);
        rememberRegisteredTrackpadGesture({
            .fingerCount = gesture->fingerCount,
            .direction = gesture->direction,
            .modMask = gesture->modMask,
            .deltaScale = gesture->deltaScale,
            .disableInhibit = gesture->disableInhibit,
        });
        ++replaced;
    }

    if (replaced > 0 && debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] replaced native workspace gestures count=" << replaced << " source=" << (source ? source : "?");
        debugLog(out.str());
    }
}

std::optional<std::string> OverviewController::handleGestureConfigHook(const std::string& keyword, const std::string& value) {
    const auto fallbackToHyprlandGesture = [&]() -> std::optional<std::string> {
        if (!m_handleGestureOriginal)
            return {};

        return m_handleGestureOriginal(Config::mgr().get(), keyword, value);
    };

    const std::string trimmedKeyword = trimCopy(keyword);
    if (!trimmedKeyword.starts_with("gesture"))
        return fallbackToHyprlandGesture();

    const std::string flags = trimmedKeyword.substr(std::string("gesture").size());
    if (flags.find_first_not_of("p") != std::string::npos)
        return fallbackToHyprlandGesture();

    const auto tokens = splitCommaTokens(value);
    if (tokens.size() < 3)
        return fallbackToHyprlandGesture();

    std::size_t fingerCount = 0;
    try {
        fingerCount = static_cast<std::size_t>(std::stoul(tokens[0]));
    } catch (const std::exception&) {
        return fallbackToHyprlandGesture();
    }

    const auto direction = g_pTrackpadGestures->dirForString(tokens[1]);
    const bool axisDirection = direction == TRACKPAD_GESTURE_DIR_VERTICAL || direction == TRACKPAD_GESTURE_DIR_HORIZONTAL;
    const bool scrollDirection = axisDirection || direction == TRACKPAD_GESTURE_DIR_SWIPE;
    if (!scrollDirection)
        return fallbackToHyprlandGesture();

    uint32_t    modMask = 0;
    float       deltaScale = 1.0F;
    std::size_t actionIndex = 2;
    for (; actionIndex < tokens.size(); ++actionIndex) {
        const auto& token = tokens[actionIndex];
        if (token.starts_with("mod:")) {
            const std::string modValue = trimCopy(token.substr(4));
            modMask = modValue.empty() ? 0 : g_pKeybindManager->stringToModMask(modValue);
            continue;
        }

        if (token.starts_with("scale:")) {
            try {
                deltaScale = std::stof(trimCopy(token.substr(6)));
            } catch (const std::exception&) {
                return std::string{"invalid gesture scale: "} + token;
            }
            continue;
        }

        break;
    }

    if (actionIndex >= tokens.size())
        return fallbackToHyprlandGesture();

    if (axisDirection && tokens[actionIndex] == "workspace" && actionIndex + 1 == tokens.size()) {
        const bool disableInhibit = flags.contains('p');
        (void)g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
        const auto addResult =
            g_pTrackpadGestures->addGesture(makeUnique<CHymissionWorkspaceTrackpadGesture>(direction, deltaScale), fingerCount, direction, modMask, deltaScale, disableInhibit);
        if (!addResult.has_value())
            return addResult.error();

        rememberRegisteredTrackpadGesture({
            .fingerCount = fingerCount,
            .direction = direction,
            .modMask = modMask,
            .deltaScale = deltaScale,
            .disableInhibit = disableInhibit,
        });

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] register workspace gesture fingers=" << fingerCount << " dir="
                << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical") << " scale=" << deltaScale << " modMask=" << modMask
                << " disableInhibit=" << (disableInhibit ? 1 : 0);
            debugLog(out.str());
        }

        return {};
    }

    if (tokens[actionIndex] != "dispatcher")
        return fallbackToHyprlandGesture();

    if (actionIndex + 1 >= tokens.size())
        return fallbackToHyprlandGesture();

    const std::string dispatcher = tokens[actionIndex + 1];
    const std::string dispatcherArgs = joinTokens(tokens, actionIndex + 2);
    const auto        trimmedDispatcherArgs = trimCopy(dispatcherArgs);

    if (dispatcher == "hymission:scroll") {
        const auto scrollMode = parseHymissionScrollMode(trimmedDispatcherArgs);
        if (!scrollMode)
            return "hymission:scroll only supports layout; use gesture = ..., workspace for workspace swipes";

        const bool disableInhibit = flags.contains('p');
        (void)g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
        const auto addResult =
            g_pTrackpadGestures->addGesture(makeUnique<CHymissionScrollTrackpadGesture>(*scrollMode, direction, deltaScale), fingerCount, direction, modMask, deltaScale,
                                            disableInhibit);
        if (!addResult.has_value())
            return addResult.error();

        rememberRegisteredTrackpadGesture({
            .fingerCount = fingerCount,
            .direction = direction,
            .modMask = modMask,
            .deltaScale = deltaScale,
            .disableInhibit = disableInhibit,
        });

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] register niri scroll gesture fingers=" << fingerCount << " dir="
                << (direction == TRACKPAD_GESTURE_DIR_SWIPE ? "swipe" : direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical")
                << " mode=" << trimmedDispatcherArgs << " scale=" << deltaScale << " modMask=" << modMask << " disableInhibit=" << (disableInhibit ? 1 : 0);
            debugLog(out.str());
        }

        return {};
    }

    if (!axisDirection)
        return fallbackToHyprlandGesture();

    GestureDispatcherKind dispatcherKind;
    if (dispatcher == "hymission:toggle") {
        dispatcherKind = GestureDispatcherKind::Toggle;
    } else if (dispatcher == "hymission:open") {
        dispatcherKind = GestureDispatcherKind::Open;
    } else {
        return fallbackToHyprlandGesture();
    }

    bool         recommand = false;
    ScopeOverride requestedScopeValue = ScopeOverride::Default;
    if (trimmedDispatcherArgs == "recommand" || trimmedDispatcherArgs == "recommend") {
        if (dispatcherKind != GestureDispatcherKind::Toggle)
            return "gesture recommand is only supported with hymission:toggle";

        recommand = true;
    } else {
        std::string scopeError;
        const auto  requestedScope = parseScopeOverride(dispatcherArgs, scopeError);
        if (!requestedScope)
            return scopeError;

        requestedScopeValue = *requestedScope;
    }

    const bool disableInhibit = flags.contains('p');
    (void)g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
    const auto addResult =
        g_pTrackpadGestures->addGesture(makeUnique<CHymissionTrackpadGesture>(dispatcherKind, requestedScopeValue, recommand, direction, deltaScale), fingerCount, direction,
                                        modMask, deltaScale, disableInhibit);
    if (!addResult.has_value())
        return addResult.error();

    rememberRegisteredTrackpadGesture({
        .fingerCount = fingerCount,
        .direction = direction,
        .modMask = modMask,
        .deltaScale = deltaScale,
        .disableInhibit = disableInhibit,
    });

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] register gesture fingers=" << fingerCount << " dir=" << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical")
            << " dispatcher=" << dispatcher << " args=" << dispatcherArgs
            << " recommand=" << (recommand ? 1 : 0)
            << " scale=" << deltaScale << " modMask=" << modMask << " disableInhibit=" << (disableInhibit ? 1 : 0);
        debugLog(out.str());
    }

    return {};
}

bool OverviewController::beginTrackpadGesture(bool openOnly, ScopeOverride requestedScope, bool recommand, eTrackpadGestureDirection direction,
                                              const IPointer::SSwipeUpdateEvent& event, float deltaScale) {
    if (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return false;

    const bool opening = !isVisible() || openOnly || m_state.phase == Phase::Opening;
    const double initialDelta = normalizedGestureDelta(event, direction, deltaScale, gestureInvertVerticalEnabled());
    const int    initialDirectionSign = signedUnit(initialDelta);

    if (recommand) {
        if (openOnly || initialDirectionSign == 0)
            return false;

        const bool currentlyVisible = isVisible();
        ScopeOverride compactScope = ScopeOverride::OnlyCurrentWorkspace;
        ScopeOverride initialScope = ScopeOverride::Default;
        double       initialSignedProgress = 0.0;
        bool         gestureOpening = true;
        bool         allowTransfer = false;
        int          directionSign = initialDirectionSign;

        if (currentlyVisible) {
            initialScope = m_state.collectionPolicy.requestedScope;
            const int currentSign = recommandScopeSign(initialScope);
            // `recommand` always uses `onlycurrentworkspace` as its compact side,
            // regardless of the config-driven default scope.
            initialSignedProgress = visualProgress() * static_cast<double>(currentSign);
            allowTransfer = resolveRecommandVisibleGestureMode(currentSign, initialDirectionSign) == RecommandVisibleGestureMode::TransferCapable;
            gestureOpening = false;

            if (m_state.phase == Phase::Active && m_state.relayoutActive) {
                for (auto& managed : m_state.windows) {
                    managed.targetGlobal = currentPreviewRect(managed);
                    managed.relayoutFromGlobal = managed.targetGlobal;
                }
                m_state.relayoutActive = false;
                m_state.relayoutProgress = 1.0;
                m_state.relayoutStart = {};
            }

            prepareGestureCloseExitGeometry();
        } else {
            const auto monitor = g_pCompositor->getMonitorFromCursor();
            if (!monitor)
                return false;

            const auto initialScopeTarget = initialDirectionSign > 0 ? ScopeOverride::ForceAll : compactScope;
            initialScope = initialScopeTarget;
            requestedScope = initialScopeTarget;
            directionSign = recommandScopeSign(initialScopeTarget);

            m_suppressInitialHoverUpdate = true;
            beginOpen(monitor, initialScopeTarget);
            m_suppressInitialHoverUpdate = false;
            if (!isVisible())
                return false;
        }

        m_gestureSession = {
            .active = true,
            .recommand = true,
            .startedVisible = currentlyVisible,
            .opening = gestureOpening,
            .allowRecommandTransfer = allowTransfer,
            .requestedScope = currentlyVisible ? initialScope : requestedScope,
            .initialScope = initialScope,
            .compactScope = compactScope,
            .direction = direction,
            .directionSign = directionSign,
            .openness = currentlyVisible ? std::abs(initialSignedProgress) : visualProgress(),
            .signedProgress = initialSignedProgress,
            .lastAlignedSpeed = 0.0,
            .deltaScale = deltaScale,
        };

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] recommand gesture begin openness=" << m_gestureSession.openness << " signed=" << m_gestureSession.signedProgress
                << " transfer=" << (m_gestureSession.allowRecommandTransfer ? 1 : 0) << " scale=" << deltaScale << " dir="
                << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical");
            debugLog(out.str());
        }

        updateTrackpadGesture(event);
        damageOwnedMonitors();
        return true;
    }

    if (openOnly && isVisible())
        return false;

    // Keeping overview open across workspace changes only affects native workspace swipes.
    // The plugin's own toggle gesture must still be able to close the visible overview.
    if (initialDirectionSign == 0 || (opening && initialDelta <= 0.0)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] gesture ignored mode=" << (opening ? "open" : "close") << " delta=" << initialDelta
                << " dir=" << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical");
            debugLog(out.str());
        }
        return false;
    }

    if (opening) {
        if (!isVisible()) {
            const auto monitor = g_pCompositor->getMonitorFromCursor();
            if (!monitor)
                return false;

            m_suppressInitialHoverUpdate = true;
            beginOpen(monitor, requestedScope);
            m_suppressInitialHoverUpdate = false;
            if (!isVisible())
                return false;
        }
    } else {
        if (m_state.phase == Phase::Active && m_state.relayoutActive) {
            for (auto& managed : m_state.windows) {
                managed.targetGlobal = currentPreviewRect(managed);
                managed.relayoutFromGlobal = managed.targetGlobal;
            }
            m_state.relayoutActive = false;
            m_state.relayoutProgress = 1.0;
            m_state.relayoutStart = {};
        }

        prepareGestureCloseExitGeometry();
    }

    m_gestureSession = {
        .active = true,
        .opening = opening,
        .requestedScope = opening ? requestedScope : m_state.collectionPolicy.requestedScope,
        .direction = direction,
        .directionSign = opening ? 1 : initialDirectionSign,
        .openness = visualProgress(),
        .lastAlignedSpeed = 0.0,
        .deltaScale = deltaScale,
    };

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] gesture begin mode=" << (opening ? "open" : "close") << " openness=" << m_gestureSession.openness << " scale=" << deltaScale
            << " dir=" << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical");
        debugLog(out.str());
    }

    updateTrackpadGesture(event);
    damageOwnedMonitors();
    return true;
}

void OverviewController::updateTrackpadGesture(const IPointer::SSwipeUpdateEvent& event) {
    if (!m_gestureSession.active)
        return;

    const double delta = normalizedGestureDelta(event, m_gestureSession.direction, m_gestureSession.deltaScale, gestureInvertVerticalEnabled());
    const double alignedDelta = delta * static_cast<double>(m_gestureSession.directionSign);

    if (m_gestureSession.recommand) {
        m_gestureSession.lastAlignedSpeed = alignedDelta;
        const double deltaProgress = alignedDelta / gestureSwipeDistance();
        constexpr double MAX_STAGE_PROGRESS = 1.0 + RECOMMAND_STAGE_TRANSFER;

        const auto syncCurrentScopeProgress = [&] {
            m_gestureSession.openness = clampUnit(std::abs(m_gestureSession.signedProgress));
            m_gestureSession.opening = !(m_gestureSession.startedVisible && m_gestureSession.requestedScope == m_gestureSession.initialScope);
        };

        const auto enterScope = [&](ScopeOverride requestedScope, int sign, double openness) -> bool {
            if (m_state.collectionPolicy.requestedScope != requestedScope) {
                if (!retargetGestureScope(requestedScope))
                    return false;

                m_gestureSession.startedVisible = false;
            }

            m_gestureSession.requestedScope = requestedScope;
            m_gestureSession.hiddenGapProgress = 0.0;
            m_gestureSession.signedProgress = static_cast<double>(sign) * clampUnit(openness);
            m_gestureSession.openness = clampUnit(openness);
            m_gestureSession.opening = true;
            m_gestureSession.allowRecommandTransfer = false;
            m_gestureSession.directionSign = sign;
            return true;
        };

        const int currentSign = recommandScopeSign(m_gestureSession.requestedScope);
        const double sideSpaceDelta = static_cast<double>(m_gestureSession.opening ? currentSign : -currentSign) * deltaProgress;

        // Crossing through the hidden workspace should leave a small transfer
        // gap before the opposite scope starts opening.
        if (std::abs(m_gestureSession.hiddenGapProgress) > 0.0001) {
            const double projectedGap = std::clamp(m_gestureSession.hiddenGapProgress + sideSpaceDelta, -MAX_STAGE_PROGRESS, MAX_STAGE_PROGRESS);
            const int    projectedGapSign = signedUnit(projectedGap);

            if (projectedGapSign == 0) {
                m_gestureSession.hiddenGapProgress = 0.0;
                m_gestureSession.signedProgress = 0.0;
                m_gestureSession.openness = 0.0;
            } else if (projectedGapSign == currentSign) {
                m_gestureSession.hiddenGapProgress = 0.0;
                m_gestureSession.signedProgress = static_cast<double>(currentSign) * clampUnit(std::abs(projectedGap));
                syncCurrentScopeProgress();
            } else if (!m_gestureSession.allowRecommandTransfer) {
                m_gestureSession.hiddenGapProgress = 0.0;
                m_gestureSession.signedProgress = 0.0;
                m_gestureSession.openness = 0.0;
            } else if (std::abs(projectedGap) < RECOMMAND_STAGE_TRANSFER) {
                m_gestureSession.hiddenGapProgress = projectedGap;
                m_gestureSession.signedProgress = 0.0;
                m_gestureSession.openness = 0.0;
            } else {
                const auto targetScope = projectedGapSign > 0 ? ScopeOverride::ForceAll : m_gestureSession.compactScope;
                const double targetOpenness = std::abs(projectedGap) - RECOMMAND_STAGE_TRANSFER;
                if (!enterScope(targetScope, projectedGapSign, targetOpenness))
                    return;
            }
        } else {
            const double projectedProgress = std::clamp(m_gestureSession.signedProgress + sideSpaceDelta, -MAX_STAGE_PROGRESS, MAX_STAGE_PROGRESS);
            const int    projectedSign = signedUnit(projectedProgress);

            if (projectedSign == 0) {
                m_gestureSession.signedProgress = 0.0;
                m_gestureSession.openness = 0.0;
            } else if (projectedSign == currentSign) {
                m_gestureSession.signedProgress = static_cast<double>(currentSign) * clampUnit(std::abs(projectedProgress));
                syncCurrentScopeProgress();
            } else if (!m_gestureSession.allowRecommandTransfer) {
                m_gestureSession.hiddenGapProgress = 0.0;
                m_gestureSession.signedProgress = 0.0;
                m_gestureSession.openness = 0.0;
            } else {
                const double overflow = std::abs(projectedProgress);
                if (overflow < RECOMMAND_STAGE_TRANSFER) {
                    m_gestureSession.signedProgress = 0.0;
                    m_gestureSession.hiddenGapProgress = static_cast<double>(projectedSign) * overflow;
                    m_gestureSession.openness = 0.0;
                } else {
                    const auto targetScope = projectedSign > 0 ? ScopeOverride::ForceAll : m_gestureSession.compactScope;
                    const double targetOpenness = overflow - RECOMMAND_STAGE_TRANSFER;
                    if (!enterScope(targetScope, projectedSign, targetOpenness))
                        return;
                }
            }
        }

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] recommand gesture update openness=" << m_gestureSession.openness << " signed=" << m_gestureSession.signedProgress
                << " gap=" << m_gestureSession.hiddenGapProgress << " aligned=" << alignedDelta << " scope=";
            switch (m_gestureSession.requestedScope) {
                case ScopeOverride::ForceAll:
                    out << "forceall";
                    break;
                case ScopeOverride::OnlyCurrentWorkspace:
                    out << "onlycurrentworkspace";
                    break;
                case ScopeOverride::Default:
                default:
                    out << "default";
                    break;
            }
            debugLog(out.str());
        }

        damageOwnedMonitors();
        return;
    }

    m_gestureSession.lastAlignedSpeed = alignedDelta;
    m_gestureSession.openness = clampUnit(m_gestureSession.openness + (m_gestureSession.opening ? alignedDelta : -alignedDelta) / gestureSwipeDistance());

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] gesture update openness=" << m_gestureSession.openness << " aligned=" << alignedDelta;
        debugLog(out.str());
    }

    damageOwnedMonitors();
}

void OverviewController::endTrackpadGesture(bool cancelled) {
    if (!m_gestureSession.active)
        return;

    const GestureSession gesture = m_gestureSession;

    if (gesture.recommand) {
        const double speedThreshold = gestureForceSpeedThreshold();
        const int    commitDirection =
            resolveRecommandGestureCommitDirection(gesture.signedProgress, gesture.opening, gesture.lastAlignedSpeed, speedThreshold, cancelled);

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] recommand gesture end cancelled=" << (cancelled ? 1 : 0) << " target=" << commitDirection << " openness=" << gesture.openness
                << " signed=" << gesture.signedProgress << " lastAligned=" << gesture.lastAlignedSpeed;
            debugLog(out.str());
        }

        if (commitDirection == 0) {
            if (!gesture.opening) {
                m_gestureSession = {};
                beginClose(CloseMode::Normal, gesture.openness, true);
                return;
            }

            m_gestureSession = {};
            clearPostCloseDispatcher();
            m_state.pendingExitFocus = m_state.focusBeforeOpen;
            m_state.pendingExitWorkspace.reset();
            m_state.closeMode = m_state.focusBeforeOpen ? CloseMode::Normal : CloseMode::Abort;
            if (m_state.focusBeforeOpen && m_state.focusBeforeOpen->m_isMapped)
                commitOverviewExitFocus(m_state.focusBeforeOpen);
            m_state.phase = Phase::Closing;
            m_state.animationProgress = 0.0;
            m_state.animationFromVisual = gesture.openness;
            m_state.animationToVisual = 0.0;
            m_state.animationStart = {};
            damageOwnedMonitors();
            return;
        }

        const auto targetScope = commitDirection > 0 ? ScopeOverride::ForceAll : gesture.compactScope;
        if (m_state.collectionPolicy.requestedScope != targetScope && !retargetGestureScope(targetScope)) {
            m_gestureSession = {};
            return;
        }

        m_gestureSession = {};
        m_deactivatePending = false;
        m_state.phase = Phase::Opening;
        m_state.animationProgress = 0.0;
        m_state.animationFromVisual = gesture.openness;
        m_state.animationToVisual = 1.0;
        m_state.animationStart = {};
        damageOwnedMonitors();
        return;
    }

    const double speedThreshold = gestureForceSpeedThreshold();
    const bool   commit = resolveOverviewGestureCommit(gesture.opening, gesture.openness, gesture.lastAlignedSpeed, speedThreshold, cancelled);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] gesture end mode=" << (gesture.opening ? "open" : "close") << " cancelled=" << (cancelled ? 1 : 0) << " commit=" << (commit ? 1 : 0)
            << " openness=" << gesture.openness << " lastAligned=" << gesture.lastAlignedSpeed;
        debugLog(out.str());
    }

    if (!gesture.opening && commit) {
        m_gestureSession = {};
        beginClose(CloseMode::Normal, gesture.openness, true);
        return;
    }

    m_gestureSession = {};

    m_deactivatePending = false;
    if (gesture.opening) {
        if (commit) {
            m_state.phase = Phase::Opening;
            m_state.animationProgress = 0.0;
            m_state.animationFromVisual = gesture.openness;
            m_state.animationToVisual = 1.0;
            m_state.animationStart = {};
        } else {
            clearPostCloseDispatcher();
            m_state.pendingExitFocus = m_state.focusBeforeOpen;
            m_state.pendingExitWorkspace.reset();
            m_state.closeMode = m_state.focusBeforeOpen ? CloseMode::Normal : CloseMode::Abort;
            if (m_state.focusBeforeOpen && m_state.focusBeforeOpen->m_isMapped)
                commitOverviewExitFocus(m_state.focusBeforeOpen);
            m_state.phase = Phase::Closing;
            m_state.animationProgress = 0.0;
            m_state.animationFromVisual = gesture.openness;
            m_state.animationToVisual = 0.0;
            m_state.animationStart = {};
        }
    } else {
        m_state.phase = Phase::Opening;
        m_state.animationProgress = 0.0;
        m_state.animationFromVisual = gesture.openness;
        m_state.animationToVisual = 1.0;
        m_state.animationStart = {};
    }

    damageOwnedMonitors();
}




bool OverviewController::beginOverviewWorkspaceSwipeGesture(eTrackpadGestureDirection direction) {
    if (!isVisible() || !allowsWorkspaceSwitchInOverview() || m_gestureSession.active || m_state.phase != Phase::Active || m_workspaceTransition.active)
        return false;

    PHLMONITOR monitor = Desktop::focusState()->monitor();
    if (!monitor || !containsHandle(m_state.participatingMonitors, monitor))
        monitor = m_state.ownerMonitor;
    if (!monitor)
        return false;

    m_workspaceSwipeGesture = {
        .active = true,
        .monitor = monitor,
        .direction = direction,
    };

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview workspace swipe begin monitor=" << monitor->m_name << " dir="
            << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : direction == TRACKPAD_GESTURE_DIR_VERTICAL ? "vertical" : "other");
        debugLog(out.str());
    }

    return true;
}

OverviewController::State OverviewController::captureOverviewWorkspaceTransitionSourceState() const {
    State source = m_state;
    source.phase = Phase::Active;
    source.relayoutActive = false;
    source.relayoutProgress = 1.0;
    source.relayoutStart = {};
    for (auto& managed : source.windows) {
        if (const auto* currentManaged = managedWindowFor(m_state, managed.window, false))
            managed.targetGlobal = currentPreviewRect(*currentManaged);
        managed.relayoutFromGlobal = managed.targetGlobal;
    }
    return source;
}

bool OverviewController::beginOverviewWorkspaceTransition(const PHLMONITOR& monitor, WORKSPACEID workspaceId, std::string workspaceName, PHLWORKSPACE workspace,
                                                         bool syntheticEmpty, WorkspaceTransitionMode mode, std::optional<State> sourceStateOverride,
                                                         PHLWINDOW preferredTargetFocus) {
    if (!monitor || !isVisible() || m_state.phase != Phase::Active)
        return false;

    // Clear any queued edit dispatchers from a previous transition, as they
    // would be stale for the new workspace transition.
    m_pendingEditDispatchers.clear();

    if (!sourceStateOverride && m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state) && m_state.windows.empty() &&
        niriWallpaperZoomAppliesToState(m_state)) {
        // Preserve the source workspace's layer snapshot before the active
        // workspace changes.  Empty workspaces have no window snapshots to carry
        // their identity, so per-workspace layer snapshots are what keep the
        // source and target wallpaper viewports from showing the same desktop.
        syncNiriWallpaperLayoutLayerProxies();
    }

    if (!sourceStateOverride && m_state.relayoutActive) {
        const bool freezeNiriPlaceholderRelayout = m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state);
        const double relayoutProgress = relayoutVisualProgress();
        for (auto& managed : m_state.windows) {
            managed.targetGlobal = currentPreviewRect(managed);
            managed.relayoutFromGlobal = managed.targetGlobal;
        }
        if (freezeNiriPlaceholderRelayout) {
            for (auto& placeholder : m_state.emptyWorkspacePlaceholders) {
                placeholder.targetGlobal = lerpRect(placeholder.relayoutFromGlobal, placeholder.targetGlobal, relayoutProgress);
                placeholder.relayoutFromGlobal = placeholder.targetGlobal;
            }
        }
        m_relayoutProgressAnimation.reset();
        m_state.relayoutActive = false;
        m_state.relayoutProgress = 1.0;
        m_state.relayoutStart = {};
    }

    State source = sourceStateOverride ? std::move(*sourceStateOverride) : captureOverviewWorkspaceTransitionSourceState();

    const auto anchorMonitor = m_state.ownerMonitor ? m_state.ownerMonitor : monitor;
    std::vector<WorkspaceOverride> overrides = {{
        .monitorId = monitor->m_id,
        .workspace = workspace,
        .workspaceId = workspaceId,
        .workspaceName = std::move(workspaceName),
        .syntheticEmpty = syntheticEmpty,
    }};

    const long scrollingFocusFitMethod = getConfigInt(m_handle, "scrolling:focus_fit_method", 0);
    const bool shouldResolveCenteredScrollingFocus = !preferredTargetFocus && workspace && m_state.collectionPolicy.onlyActiveWorkspace &&
        niriModeAppliesToState(m_state) && isScrollingWorkspace(workspace) && !syntheticEmpty;
    const auto centeredScrollingFocus = shouldResolveCenteredScrollingFocus ? centeredFocusFit0WindowForScrollingWorkspace(workspace) : PHLWINDOW{};
    const bool preserveScrollPastCamera = shouldResolveCenteredScrollingFocus && !centeredScrollingFocus && focusFit0NativeEdgeCameraActive(workspace);
    const auto fallbackTargetFocus = preserveScrollPastCamera ? PHLWINDOW{} : focusCandidateForWorkspace(workspace);
    const auto targetFocus = preferredTargetFocus && preferredTargetFocus->m_isMapped && preferredTargetFocus->m_workspace == workspace ?
        preferredTargetFocus :
        (scrollingFocusFitMethod == 0 && centeredScrollingFocus) ? centeredScrollingFocus : fallbackTargetFocus;

    if (debugLogsEnabled() && shouldResolveCenteredScrollingFocus) {
        std::ostringstream out;
        out << "[hymission] focus-fit transition begin focus resolve"
            << " focusFit=" << scrollingFocusFitMethod
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " centered=" << debugWindowLabel(centeredScrollingFocus)
            << " fallback=" << debugWindowLabel(fallbackTargetFocus)
            << " chosen=" << debugWindowLabel(targetFocus)
            << " preserveScrollPast=" << (preserveScrollPastCamera ? 1 : 0);
        if (auto* scrolling = scrollingAlgorithmForWorkspace(workspace); scrolling && scrolling->m_scrollingData && scrolling->m_scrollingData->controller) {
            out << " offset=" << scrolling->m_scrollingData->controller->getOffset()
                << " columns=" << scrolling->m_scrollingData->columns.size();
        }
        debugLog(out.str());
    }

    State      target = buildState(anchorMonitor, m_state.collectionPolicy.requestedScope, overrides, true, false, targetFocus);
    if (target.participatingMonitors.empty())
        return false;

    if (workspace)
        target.ownerWorkspace = workspace;
    refreshWorkspaceStripActivity(target, monitor, workspaceId);

    const bool targetWorkspaceHasSingleScrollingColumn = scrollingWorkspaceHasSingleColumn(workspace);
    const bool targetOwnerEdgeCameraActive = target.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(target) &&
        directNiriOwnerEdgeCameraActive(target);
    const bool targetHasResolvedFocus = target.focusDuringOverview && target.focusDuringOverview->m_isMapped && workspace &&
        target.focusDuringOverview->m_workspace == workspace;
    const bool preserveTargetEdgeCamera = targetOwnerEdgeCameraActive && !targetWorkspaceHasSingleScrollingColumn &&
        (preserveScrollPastCamera || !targetHasResolvedFocus);
    if (debugLogsEnabled() && target.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(target) && workspace && isScrollingWorkspace(workspace)) {
        std::ostringstream out;
        out << "[hymission] focus-fit target edge-camera decision"
            << " focusFit=" << scrollingFocusFitMethod
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " centered=" << debugWindowLabel(centeredScrollingFocus)
            << " fallback=" << debugWindowLabel(fallbackTargetFocus)
            << " targetFocus=" << debugWindowLabel(targetFocus)
            << " targetStateFocus=" << debugWindowLabel(target.focusDuringOverview)
            << " preserveScrollPast=" << (preserveScrollPastCamera ? 1 : 0)
            << " edgeCamera=" << (targetOwnerEdgeCameraActive ? 1 : 0)
            << " singleColumn=" << (targetWorkspaceHasSingleScrollingColumn ? 1 : 0)
            << " resolvedFocus=" << (targetHasResolvedFocus ? 1 : 0)
            << " preserve=" << (preserveTargetEdgeCamera ? 1 : 0);
        debugLog(out.str());
    }
    if (preserveTargetEdgeCamera) {
        // A multi-column scrolling workspace in Hyprland's edge-camera range is
        // the scroll-past empty viewport.  Do not preserve that focusless state
        // when focus_fit_method=0 has resolved a visible centered partial column;
        // that case is a real focused strip position, not an empty edge camera.
        target.selectedIndex.reset();
        target.focusDuringOverview.reset();
    }

    target.phase = Phase::Active;
    target.focusBeforeOpen = windowMatchesOverviewScope(m_state.focusBeforeOpen, target, false) ? m_state.focusBeforeOpen : PHLWINDOW{};
    target.closeMode = m_state.closeMode;
    target.pendingExitFocus = windowMatchesOverviewScope(m_state.pendingExitFocus, target, false) ? m_state.pendingExitFocus : PHLWINDOW{};
    target.pendingExitWorkspace = containsHandle(target.managedWorkspaces, m_state.pendingExitWorkspace) ? m_state.pendingExitWorkspace : PHLWORKSPACE{};
    target.relayoutActive = false;
    target.relayoutProgress = 1.0;
    target.relayoutStart = {};

    const auto transitionWorkspace = monitor->m_activeWorkspace ? monitor->m_activeWorkspace : source.ownerWorkspace;
    const auto transitionAxis = niriModeAppliesToState(m_state) ? WorkspaceTransitionAxis::Vertical :
        (workspaceSwipeUsesVerticalAxis(transitionWorkspace) ? WorkspaceTransitionAxis::Vertical : WorkspaceTransitionAxis::Horizontal);

    m_workspaceTransition = {
        .active = true,
        .monitor = monitor,
        .gestureDirection = m_workspaceSwipeGesture.direction,
        .axis = transitionAxis,
        .mode = mode,
        .distance = workspaceSwipeViewportDistance(monitor, transitionAxis),
        .delta = 0.0,
        .step = workspaceId > monitor->m_activeWorkspace->m_id ? 1 : -1,
        .initialDirection = 0,
        .avgSpeed = 0.0,
        .speedPoints = 0,
        .targetWorkspaceId = workspaceId,
        .targetWorkspaceName = overrides.front().workspaceName,
        .targetWorkspaceSyntheticEmpty = syntheticEmpty,
        .targetEdgeCameraPreserved = preserveTargetEdgeCamera,
        .sourceState = std::move(source),
        .targetState = std::move(target),
        .animationFromDelta = 0.0,
        .animationToDelta = 0.0,
        .animationProgress = 0.0,
        .animationStart = {},
    };

    if (mode == WorkspaceTransitionMode::TimedCommit)
        m_workspaceTransition.animationToDelta = static_cast<double>(m_workspaceTransition.step) * m_workspaceTransition.distance;

    refreshWorkspaceStripActivity(m_state, monitor, workspaceId);
    if (targetFocus && !preserveTargetEdgeCamera)
        m_state.focusDuringOverview = targetFocus;
    if (usesDirectNiriScrollingOverview(m_state) && workspaceStripEnabled(m_state)) {
        const bool animateStripRelayout = niriOverviewAnimationsEnabled();
        bool stripRelayoutChanged = false;
        for (auto& entry : m_state.stripEntries) {
            const auto targetIt = std::find_if(m_workspaceTransition.targetState.stripEntries.begin(), m_workspaceTransition.targetState.stripEntries.end(),
                                               [&](const WorkspaceStripEntry& targetEntry) {
                                                   return workspaceStripEntriesMatchForSnapshot(entry, targetEntry);
                                               });
            if (targetIt == m_workspaceTransition.targetState.stripEntries.end())
                continue;

            const Rect currentRect = currentWorkspaceStripRect(entry);
            entry.rect = targetIt->rect;
            entry.relayoutFromRect = currentRect;
            entry.hasRelayoutFromRect = animateStripRelayout && !rectApproxEqual(currentRect, entry.rect, 0.5);
            stripRelayoutChanged = stripRelayoutChanged || entry.hasRelayoutFromRect;
            entry.active = targetIt->active;
        }

        if (stripRelayoutChanged) {
            m_state.relayoutActive = true;
            beginOverviewRelayoutAnimation("workspace-strip-transition");
        }
    }

    armWorkspaceTransitionRenderState();

    const bool activateTimedNiriTarget = mode == WorkspaceTransitionMode::TimedCommit && m_state.collectionPolicy.onlyActiveWorkspace &&
        (niriModeAppliesToState(m_workspaceTransition.sourceState) || niriModeAppliesToState(m_workspaceTransition.targetState));
    if (activateTimedNiriTarget && !activateTimedNiriWorkspaceTransitionTarget()) {
        clearOverviewWorkspaceTransition();
        return false;
    }
    if (activateTimedNiriTarget && preferredTargetFocus && !m_workspaceTransition.targetEdgeCameraPreserved &&
        !rebuildTimedNiriWorkspaceTransitionTarget(preferredTargetFocus)) {
        clearOverviewWorkspaceTransition();
        return false;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview workspace transition begin monitor=" << monitor->m_name << " targetId=" << workspaceId
            << " synthetic=" << (syntheticEmpty ? 1 : 0) << " mode="
            << (mode == WorkspaceTransitionMode::Gesture ? "gesture" : mode == WorkspaceTransitionMode::TimedCommit ? "commit" : "revert")
            << " axis=" << (m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical ? "vertical" : "horizontal");
        debugLog(out.str());
    }

    damageOwnedMonitors();
    return true;
}

bool OverviewController::rebuildTimedNiriWorkspaceTransitionTarget(const PHLWINDOW& preferredTargetFocus) {
    if (!m_workspaceTransition.active || !m_workspaceTransition.targetActivatedEarly || !m_workspaceTransition.monitor ||
        !preferredTargetFocus || !preferredTargetFocus->m_isMapped)
        return false;

    const auto targetWorkspace = preferredTargetFocus->m_workspace;
    if (!targetWorkspace || targetWorkspace->m_id != m_workspaceTransition.targetWorkspaceId)
        return false;

    const auto anchorMonitor = m_state.ownerMonitor ? m_state.ownerMonitor : m_workspaceTransition.monitor;
    const std::vector<WorkspaceOverride> overrides = {{
        .monitorId = m_workspaceTransition.monitor->m_id,
        .workspace = targetWorkspace,
        .workspaceId = targetWorkspace->m_id,
        .workspaceName = targetWorkspace->m_name,
        .syntheticEmpty = false,
    }};
    State target = buildState(anchorMonitor, m_state.collectionPolicy.requestedScope, overrides, true, false, preferredTargetFocus);
    if (target.participatingMonitors.empty())
        return false;

    target.ownerWorkspace = targetWorkspace;
    target.phase = Phase::Active;
    target.focusBeforeOpen = windowMatchesOverviewScope(m_state.focusBeforeOpen, target, false) ? m_state.focusBeforeOpen : PHLWINDOW{};
    target.closeMode = m_state.closeMode;
    target.pendingExitFocus = windowMatchesOverviewScope(m_state.pendingExitFocus, target, false) ? m_state.pendingExitFocus : PHLWINDOW{};
    target.pendingExitWorkspace = containsHandle(target.managedWorkspaces, m_state.pendingExitWorkspace) ? m_state.pendingExitWorkspace : PHLWORKSPACE{};
    target.relayoutActive = false;
    target.relayoutProgress = 1.0;
    target.relayoutStart = {};
    refreshWorkspaceStripActivity(target, m_workspaceTransition.monitor, targetWorkspace->m_id);
    selectWindowInState(target, preferredTargetFocus);
    target.focusDuringOverview = preferredTargetFocus;
    m_workspaceTransition.targetState = std::move(target);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] rebuilt timed niri transition target after focus scroll"
            << " workspace=" << debugWorkspaceLabel(targetWorkspace)
            << " focus=" << debugWindowLabel(preferredTargetFocus);
        debugLog(out.str());
    }

    return true;
}

bool OverviewController::beginExternalOverviewWorkspaceTransition(const PHLWORKSPACE& workspace) {
    if (!workspace || workspace->m_isSpecialWorkspace || !allowsWorkspaceSwitchInOverview() || m_workspaceTransition.active || m_gestureSession.active ||
        m_state.phase != Phase::Active)
        return false;

    const auto previousWorkspace = m_state.ownerWorkspace;
    if (!previousWorkspace || previousWorkspace == workspace || previousWorkspace->m_isSpecialWorkspace)
        return false;

    auto monitor = workspace->m_monitor.lock();
    if (!monitor && m_state.ownerMonitor && m_state.ownerMonitor->m_activeWorkspace == workspace)
        monitor = m_state.ownerMonitor;
    if (!monitor)
        monitor = Desktop::focusState()->monitor();
    if (!monitor || !containsHandle(m_state.participatingMonitors, monitor) || monitor->m_activeWorkspace != workspace)
        return false;

    int step = workspace->m_id > previousWorkspace->m_id ? 1 : -1;
    if (shouldWrapWorkspaceIds(workspace->m_id, previousWorkspace->m_id))
        step = -step;

    if (!beginOverviewWorkspaceTransition(monitor, workspace->m_id, workspace->m_name, workspace, false, WorkspaceTransitionMode::TimedCommit))
        return false;

    m_workspaceTransition.step = step;
    m_workspaceTransition.initialDirection = 0;
    m_workspaceTransition.delta = 0.0;
    m_workspaceTransition.animationFromDelta = 0.0;
    m_workspaceTransition.animationToDelta = static_cast<double>(step) * m_workspaceTransition.distance;
    m_workspaceTransition.animationProgress = 0.0;
    m_workspaceTransition.animationStart = {};

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] external workspace change converted to overview transition previous=" << debugWorkspaceLabel(previousWorkspace)
            << " target=" << debugWorkspaceLabel(workspace) << " step=" << step;
        debugLog(out.str());
    }

    damageOwnedMonitors();
    return true;
}

bool OverviewController::startOverviewWorkspaceTransitionByStep(const PHLMONITOR& monitor, int step, WorkspaceTransitionMode mode) {
    if (!allowsWorkspaceSwitchInOverview() || !monitor || step == 0)
        return false;

    WORKSPACEID targetId = WORKSPACE_INVALID;
    std::string targetName;
    PHLWORKSPACE targetWorkspace;
    bool syntheticEmpty = false;
    if (!resolveOverviewWorkspaceTargetByStep(monitor, step, targetId, targetName, targetWorkspace, syntheticEmpty))
        return false;

    if (!beginOverviewWorkspaceTransition(monitor, targetId, std::move(targetName), targetWorkspace, syntheticEmpty, mode))
        return false;

    m_workspaceTransition.step = step < 0 ? -1 : 1;
    if (mode == WorkspaceTransitionMode::TimedCommit)
        m_workspaceTransition.animationToDelta = static_cast<double>(m_workspaceTransition.step) * m_workspaceTransition.distance;
    return true;
}

void OverviewController::updateOverviewWorkspaceSwipeGesture(double delta) {
    updateOverviewWorkspaceSwipeGestureAdjusted(workspaceSwipeInvertEnabled() ? -delta : delta, false);
}

void OverviewController::setOverviewWorkspaceSwipeGestureDelta(double delta) {
    updateOverviewWorkspaceSwipeGestureAdjusted(delta, true);
}

void OverviewController::updateOverviewWorkspaceSwipeGestureAdjusted(double delta, bool absolute) {
    if (!m_workspaceSwipeGesture.active || !m_workspaceSwipeGesture.monitor)
        return;

    const double candidateTotal = absolute ? delta : m_workspaceSwipeGesture.gestureDelta + delta;
    if (std::abs(candidateTotal) < 0.0001) {
        m_workspaceSwipeGesture.gestureDelta = 0.0;
        if (m_workspaceTransition.active) {
            m_workspaceTransition.delta = 0.0;
            damageOwnedMonitors();
        }
        return;
    }

    const int intendedStep = candidateTotal < 0.0 ? -1 : 1;
    if (!m_workspaceTransition.active || m_workspaceTransition.step != intendedStep) {
        if (!startOverviewWorkspaceTransitionByStep(m_workspaceSwipeGesture.monitor, intendedStep, WorkspaceTransitionMode::Gesture))
            return;
    }

    double nextGestureDelta = candidateTotal;
    if (gestureSwipeDirectionLockEnabled()) {
        if (m_workspaceTransition.initialDirection != 0 && m_workspaceTransition.initialDirection != (nextGestureDelta < 0.0 ? -1 : 1)) {
            nextGestureDelta = 0.0;
        } else if (m_workspaceTransition.initialDirection == 0 && std::abs(nextGestureDelta) > gestureSwipeDirectionLockThreshold()) {
            m_workspaceTransition.initialDirection = nextGestureDelta < 0.0 ? -1 : 1;
        }
    }

    const double gestureDistance = gestureSwipeDistance();
    nextGestureDelta = std::clamp(nextGestureDelta, -gestureDistance, gestureDistance);

    const double previousDelta = m_workspaceTransition.delta;
    m_workspaceSwipeGesture.gestureDelta = nextGestureDelta;
    m_workspaceTransition.delta = (nextGestureDelta / gestureDistance) * m_workspaceTransition.distance;
    const double deltaStep = std::abs(previousDelta - m_workspaceTransition.delta);
    m_workspaceTransition.avgSpeed = (m_workspaceTransition.avgSpeed * static_cast<double>(m_workspaceTransition.speedPoints) + deltaStep) /
        static_cast<double>(m_workspaceTransition.speedPoints + 1);
    ++m_workspaceTransition.speedPoints;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview workspace swipe update gestureDelta=" << m_workspaceSwipeGesture.gestureDelta
            << " visualDelta=" << m_workspaceTransition.delta << " avgSpeed=" << m_workspaceTransition.avgSpeed << " step=" << m_workspaceTransition.step;
        debugLog(out.str());
    }

    damageOwnedMonitors();

    if (gestureSwipeForeverEnabled() && std::abs(m_workspaceSwipeGesture.gestureDelta) >= gestureDistance - 0.5)
        requestOverviewWorkspaceTransitionCommit(true, false);
}

void OverviewController::endOverviewWorkspaceSwipeGesture(bool cancelled) {
    const double gestureDelta = m_workspaceSwipeGesture.gestureDelta;
    const bool touchActive = m_workspaceSwipeGesture.touchActive;
    m_workspaceSwipeGesture = {};

    if (!m_workspaceTransition.active)
        return;

    const double cancelRatio = std::clamp(getConfigFloat(m_handle, "gestures:workspace_swipe_cancel_ratio", 0.5), 0.0, 1.0);
    const double gestureDistance = gestureSwipeDistance();
    const double speedThreshold = gestureForceSpeedThreshold();
    const double visualSpeedThreshold = speedThreshold * (m_workspaceTransition.distance / gestureDistance);
    const bool revert =
        cancelled || ((std::abs(gestureDelta) < gestureDistance * cancelRatio &&
                       (speedThreshold == 0.0 || m_workspaceTransition.avgSpeed < visualSpeedThreshold)) ||
                      std::abs(gestureDelta) < 2.0);

    m_workspaceTransition.mode = revert ? WorkspaceTransitionMode::TimedRevert : WorkspaceTransitionMode::TimedCommit;
    m_workspaceTransition.animationFromDelta = m_workspaceTransition.delta;
    m_workspaceTransition.animationToDelta = revert ? 0.0 : static_cast<double>(m_workspaceTransition.step) * m_workspaceTransition.distance;
    m_workspaceTransition.animationProgress = 0.0;
    m_workspaceTransition.animationStart = {};

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview workspace swipe end cancelled=" << (cancelled ? 1 : 0) << " revert=" << (revert ? 1 : 0)
            << " touch=" << (touchActive ? 1 : 0) << " gestureDelta=" << gestureDelta << " visualDelta=" << m_workspaceTransition.delta
            << " avgSpeed=" << m_workspaceTransition.avgSpeed;
        debugLog(out.str());
    }

    damageOwnedMonitors();
}

void OverviewController::updateOverviewWorkspaceTransition() {
    if (!m_workspaceTransition.active || m_workspaceTransition.mode == WorkspaceTransitionMode::Gesture)
        return;

    // Closing the overview owns the camera. If a workspace transition is still
    // active when an empty Niri overview closes, committing/rebuilding that
    // transition mid-close can replace the close geometry with the transition
    // geometry and make all empty wallpaper viewports collapse onto the same
    // monitor-sized target. Leave the transition frozen; deactivate() will clear
    // the overview state after the close animation finishes.
    if (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return;

    const bool hyprlandOwnedAnimation = niriModeAppliesToState(m_workspaceTransition.sourceState) ||
        niriModeAppliesToState(m_workspaceTransition.targetState);
    if (hyprlandOwnedAnimation) {
        if (m_workspaceTransition.animationStart == std::chrono::steady_clock::time_point{}) {
            m_workspaceTransition.animationStart = std::chrono::steady_clock::now();
            beginWorkspaceTransitionAnimation("workspace-switch");
        }

        if (m_workspaceTransitionAnimation)
            m_workspaceTransition.delta = m_workspaceTransitionAnimation->value();
        else
            m_workspaceTransition.delta = m_workspaceTransition.animationToDelta;

        if (m_workspaceTransitionAnimation && m_workspaceTransitionAnimation->isBeingAnimated())
            return;

        finishWorkspaceTransitionAnimation();
        if (m_workspaceTransition.mode == WorkspaceTransitionMode::TimedRevert) {
            clearOverviewWorkspaceTransition();
            damageOwnedMonitors();
            return;
        }

        requestOverviewWorkspaceTransitionCommit(false, false);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_workspaceTransition.animationStart == std::chrono::steady_clock::time_point{}) {
        m_workspaceTransition.animationStart = now;
        m_workspaceTransition.animationProgress = 0.0;
        return;
    }

    const double elapsedMs = std::chrono::duration<double, std::milli>(now - m_workspaceTransition.animationStart).count();
    m_workspaceTransition.animationProgress = clampUnit(elapsedMs / WORKSPACE_TRANSITION_DURATION_MS);
    const double eased = easeOutCubic(m_workspaceTransition.animationProgress);
    m_workspaceTransition.delta =
        m_workspaceTransition.animationFromDelta + (m_workspaceTransition.animationToDelta - m_workspaceTransition.animationFromDelta) * eased;

    if (m_workspaceTransition.animationProgress < 1.0)
        return;

    if (m_workspaceTransition.mode == WorkspaceTransitionMode::TimedRevert) {
        clearOverviewWorkspaceTransition();
        updateHoveredFromPointer(false, false, false, false, "workspace-transition-revert");
        damageOwnedMonitors();
        return;
    }

    requestOverviewWorkspaceTransitionCommit(false, false);
}

void OverviewController::requestOverviewWorkspaceTransitionCommit(bool followGesture, bool forceSync) {
    if (!m_workspaceTransition.active)
        return;

    if (!(g_pHyprOpenGL && g_pHyprRenderer->m_renderData.pMonitor) || forceSync) {
        commitOverviewWorkspaceTransition(followGesture, forceSync);
        return;
    }

    m_pendingWorkspaceTransitionCommitFollowGesture = m_pendingWorkspaceTransitionCommitFollowGesture || followGesture;
    if (m_workspaceTransitionCommitScheduled)
        return;
    if (!g_pEventLoopManager) {
        commitOverviewWorkspaceTransition(m_pendingWorkspaceTransitionCommitFollowGesture, forceSync);
        return;
    }

    m_workspaceTransitionCommitScheduled = true;
    const auto generation = ++m_workspaceTransitionCommitGeneration;

    if (debugLogsEnabled())
        debugLog("[hymission] defer overview workspace transition commit until after render");

    g_pEventLoopManager->doLater([this, generation, forceSync] {
        if (g_controller != this || generation != m_workspaceTransitionCommitGeneration)
            return;

        m_workspaceTransitionCommitScheduled = false;
        if (!m_workspaceTransition.active)
            return;

        const bool followGesture = m_pendingWorkspaceTransitionCommitFollowGesture;
        m_pendingWorkspaceTransitionCommitFollowGesture = false;

        // Workspace transition commit mutates live workspace/window ownership.
        // If a frame is still rendering, reschedule instead of tearing the render
        // state mid-frame.
        if (g_pHyprOpenGL && g_pHyprRenderer->m_renderData.pMonitor) {
            requestOverviewWorkspaceTransitionCommit(followGesture, forceSync);
            return;
        }

        commitOverviewWorkspaceTransition(followGesture, forceSync);
    });
}

bool OverviewController::activateTimedNiriWorkspaceTransitionTarget() {
    if (!m_workspaceTransition.active || m_workspaceTransition.mode != WorkspaceTransitionMode::TimedCommit || !m_workspaceTransition.monitor)
        return false;

    const auto transitionMonitor = m_workspaceTransition.monitor;
    auto       targetWorkspace = g_pCompositor->getWorkspaceByID(m_workspaceTransition.targetWorkspaceId);
    if (!targetWorkspace && m_workspaceTransition.targetWorkspaceSyntheticEmpty) {
        targetWorkspace = g_pCompositor->createNewWorkspace(m_workspaceTransition.targetWorkspaceId, transitionMonitor->m_id,
                                                            m_workspaceTransition.targetWorkspaceName, false);
    }
    if (!targetWorkspace)
        return false;

    const bool preserveTargetEdgeCamera = m_workspaceTransition.targetEdgeCameraPreserved;
    PHLWINDOW targetFocus;
    if (!preserveTargetEdgeCamera && m_workspaceTransition.targetState.focusDuringOverview &&
        m_workspaceTransition.targetState.focusDuringOverview->m_isMapped &&
        m_workspaceTransition.targetState.focusDuringOverview->m_workspace == targetWorkspace)
        targetFocus = m_workspaceTransition.targetState.focusDuringOverview;
    const bool targetIsEmptyNiriWorkspace = niriModeEnabled() && m_state.collectionPolicy.onlyActiveWorkspace &&
        (m_workspaceTransition.targetWorkspaceSyntheticEmpty || m_workspaceTransition.targetState.windows.empty());
    if (targetIsEmptyNiriWorkspace)
        targetFocus = PHLWINDOW{};
    else if (!preserveTargetEdgeCamera && !targetFocus) {
        const bool shouldResolveCenteredFit0Focus = targetWorkspace && m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state) &&
            isScrollingWorkspace(targetWorkspace) && getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 0;
        const auto centeredFit0Focus = shouldResolveCenteredFit0Focus ? centeredFocusFit0WindowForScrollingWorkspace(targetWorkspace) : PHLWINDOW{};
        const auto fallbackTargetFocus = focusCandidateForWorkspace(targetWorkspace);
        targetFocus = centeredFit0Focus ? centeredFit0Focus : fallbackTargetFocus;
        if (debugLogsEnabled() && shouldResolveCenteredFit0Focus) {
            std::ostringstream out;
            out << "[hymission] focus-fit0 early activation focus resolve"
                << " workspace=" << debugWorkspaceLabel(targetWorkspace)
                << " centered=" << debugWindowLabel(centeredFit0Focus)
                << " fallback=" << debugWindowLabel(fallbackTargetFocus)
                << " chosen=" << debugWindowLabel(targetFocus);
            debugLog(out.str());
        }
    }

    ScopedFlag applyingWorkspaceTransitionCommit(m_applyingWorkspaceTransitionCommit);
    const bool alreadyActive = transitionMonitor->m_activeWorkspace == targetWorkspace;
    if (!alreadyActive) {
        const auto oldWorkspace = transitionMonitor->m_activeWorkspace;

        if (targetIsEmptyNiriWorkspace || preserveTargetEdgeCamera) {
            targetWorkspace->m_lastFocusedWindow = PHLWINDOW{};
            clearWindowFocusCompat(transitionMonitor);
        } else if (targetFocus) {
            targetWorkspace->m_lastFocusedWindow = targetFocus;
        }
        transitionMonitor->changeWorkspace(targetWorkspace, true, true, !targetIsEmptyNiriWorkspace && static_cast<bool>(targetFocus));

        if (oldWorkspace) {
            for (const auto& window : g_pCompositor->m_windows) {
                if (!window || window->m_workspace != oldWorkspace || !window->m_pinned)
                    continue;

                window->layoutTarget()->assignToSpace(targetWorkspace->m_space);
            }
        }

        targetWorkspace->m_renderOffset->setValueAndWarp(Vector2D{});
        targetWorkspace->m_alpha->setValueAndWarp(1.F);
        g_layoutManager->recalculateMonitor(transitionMonitor);

        if (g_pEventManager) {
            g_pEventManager->postEvent(SHyprIPCEvent{"workspace", targetWorkspace->m_name});
            g_pEventManager->postEvent(SHyprIPCEvent{"workspacev2", std::format("{},{}", targetWorkspace->m_id, targetWorkspace->m_name)});
        }
        Event::bus()->m_events.workspace.active.emit(targetWorkspace);
    }

    const bool targetFocusIsStableCenteredFit0 = targetFocus && targetWorkspace && m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state) &&
        isScrollingWorkspace(targetWorkspace) && getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 0 &&
        centeredFocusFit0WindowForScrollingWorkspace(targetWorkspace) == targetFocus;

    if (targetFocus) {
        targetWorkspace->m_lastFocusedWindow = targetFocus;
        if (Desktop::focusState()->window() != targetFocus) {
            m_pendingLiveFocusWorkspaceChangeTarget = targetFocus;
            focusWindowCompat(targetFocus, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == targetFocus)
                m_pendingLiveFocusWorkspaceChangeTarget.reset();
        }
        if (!targetFocusIsStableCenteredFit0)
            (void)syncScrollingWorkspaceSpotOnWindow(targetFocus);
    } else {
        clearWindowFocusCompat(transitionMonitor);
    }

    if (g_pAnimationManager)
        g_pAnimationManager->frameTick();

    m_workspaceTransition.targetActivatedEarly = true;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] activate timed niri workspace transition target=" << debugWorkspaceLabel(targetWorkspace)
            << " focus=" << debugWindowLabel(targetFocus)
            << " preserveEdgeCamera=" << (preserveTargetEdgeCamera ? 1 : 0)
            << " alreadyActive=" << (alreadyActive ? 1 : 0);
        debugLog(out.str());
    }

    return true;
}

void OverviewController::commitOverviewWorkspaceTransition(bool followGesture, bool forceSync) {
    if (!m_workspaceTransition.active || !m_workspaceTransition.monitor)
        return;

    if (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return;

    clearPendingWindowGeometryRetry();

    const auto transitionMonitor = m_workspaceTransition.monitor;
    const auto oldWorkspace = transitionMonitor->m_activeWorkspace;
    const auto targetWorkspaceId = m_workspaceTransition.targetWorkspaceId;
    const bool targetWorkspaceSyntheticEmpty = m_workspaceTransition.targetWorkspaceSyntheticEmpty;
    const auto targetWorkspaceName = m_workspaceTransition.targetWorkspaceName;
    const bool targetActivatedEarly = m_workspaceTransition.targetActivatedEarly;
    const bool preserveTargetEdgeCamera = m_workspaceTransition.targetEdgeCameraPreserved;
    State      next = m_workspaceTransition.targetState;
    const bool targetStartedEdgeCamera = directNiriOwnerEdgeCameraActive(next);
    PreviewRectSnapshot edgeCameraPreviewRects;
    if (targetStartedEdgeCamera) {
        // When switching back to a workspace that is currently in Hyprland's
        // scroll-past edge-camera state, remember the overview preview rects from
        // that state even if we are going to reclaim a focused leaf during the
        // switch. If the focus sync recenters/fit-aligns the strip, the rebuilt
        // target state can then relayout from those scroll-past rects instead of
        // snapping straight to the focused layout on the first frame.
        edgeCameraPreviewRects.reserve(next.windows.size());
        for (const auto& managed : next.windows) {
            if (managed.window)
                edgeCameraPreviewRects.emplace_back(managed.window, managed.targetGlobal);
        }
    }
    std::vector<std::tuple<MONITORID, WORKSPACEID, bool, Rect>> transitionPlaceholderRects;
    {
        const auto clampedDelta = std::clamp(m_workspaceTransition.delta, -m_workspaceTransition.distance, m_workspaceTransition.distance);
        const double sourceOffset = -clampedDelta;
        const int targetDirection = clampedDelta < -0.0001 ? -1 : clampedDelta > 0.0001 ? 1 : (m_workspaceTransition.step < 0 ? -1 : 1);
        const double targetOffset = sourceOffset + static_cast<double>(targetDirection) * m_workspaceTransition.distance;
        const double t = m_workspaceTransition.distance > 0.0 ? clampUnit(std::abs(clampedDelta) / m_workspaceTransition.distance) : 1.0;
        const auto translated = [&](const Rect& rect, double offset) {
            return m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical ? translateRect(rect, 0.0, offset) : translateRect(rect, offset, 0.0);
        };
        const auto sourcePlaceholderFor = [&](const EmptyWorkspacePlaceholder& targetPlaceholder) -> const EmptyWorkspacePlaceholder* {
            const auto it = std::find_if(m_workspaceTransition.sourceState.emptyWorkspacePlaceholders.begin(),
                                         m_workspaceTransition.sourceState.emptyWorkspacePlaceholders.end(),
                                         [&](const EmptyWorkspacePlaceholder& sourcePlaceholder) {
                                             return sourcePlaceholder.monitor && targetPlaceholder.monitor &&
                                                 sourcePlaceholder.monitor == targetPlaceholder.monitor &&
                                                 sourcePlaceholder.workspaceId == targetPlaceholder.workspaceId &&
                                                 sourcePlaceholder.backingOnly == targetPlaceholder.backingOnly;
                                         });
            return it == m_workspaceTransition.sourceState.emptyWorkspacePlaceholders.end() ? nullptr : &*it;
        };

        transitionPlaceholderRects.reserve(m_workspaceTransition.targetState.emptyWorkspacePlaceholders.size());
        for (const auto& targetPlaceholder : m_workspaceTransition.targetState.emptyWorkspacePlaceholders) {
            if (!targetPlaceholder.monitor || targetPlaceholder.workspaceId == WORKSPACE_INVALID)
                continue;

            Rect rect = targetPlaceholder.targetGlobal;
            if (const auto* sourcePlaceholder = sourcePlaceholderFor(targetPlaceholder)) {
                const Rect sourceRect = translated(sourcePlaceholder->targetGlobal, sourceOffset);
                const Rect targetRect = translated(targetPlaceholder.targetGlobal, targetOffset);
                rect = lerpRect(sourceRect, targetRect, t);
            } else {
                rect = translated(targetPlaceholder.targetGlobal, targetOffset);
            }

            transitionPlaceholderRects.emplace_back(targetPlaceholder.monitor->m_id, targetPlaceholder.workspaceId, targetPlaceholder.backingOnly, rect);
        }
    }

    auto targetWorkspace = g_pCompositor->getWorkspaceByID(targetWorkspaceId);
    if (!targetWorkspace && targetWorkspaceSyntheticEmpty) {
        targetWorkspace = g_pCompositor->createNewWorkspace(targetWorkspaceId, transitionMonitor->m_id, targetWorkspaceName, false);
    }
    if (!targetWorkspace) {
        clearOverviewWorkspaceTransition();
        damageOwnedMonitors();
        return;
    }

    const bool hyprlandOwnedWorkspaceAnimation = niriModeAppliesToState(m_workspaceTransition.sourceState) ||
        niriModeAppliesToState(m_workspaceTransition.targetState);
    const bool temporarilyDisabledAnimations = !hyprlandOwnedWorkspaceAnimation && !m_animationsEnabledOverridden;
    if (temporarilyDisabledAnimations)
        setAnimationsEnabledOverride(true);

    m_rebuildVisibleStateAfterWorkspaceTransitionCommit = false;
    {
        ScopedFlag applyingWorkspaceTransitionCommit(m_applyingWorkspaceTransitionCommit);

        PHLWINDOW intendedTargetFocus;
        if (!preserveTargetEdgeCamera && next.focusDuringOverview && next.focusDuringOverview->m_isMapped &&
            next.focusDuringOverview->m_workspace == targetWorkspace)
            intendedTargetFocus = next.focusDuringOverview;

        const bool preserveDirectNiriFocus =
            intendedTargetFocus && next.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(next) && isScrollingWorkspace(targetWorkspace);
        if (preserveDirectNiriFocus)
            targetWorkspace->m_lastFocusedWindow = intendedTargetFocus;

        const bool targetIsEmptyNiriWorkspace = niriModeEnabled() && next.collectionPolicy.onlyActiveWorkspace &&
            (targetWorkspaceSyntheticEmpty || next.windows.empty());
        if (targetIsEmptyNiriWorkspace || preserveTargetEdgeCamera) {
            targetWorkspace->m_lastFocusedWindow = PHLWINDOW{};
            clearWindowFocusCompat(transitionMonitor);
        }

        const bool targetHasFocusCandidateBeforeSwitch = !targetIsEmptyNiriWorkspace && !preserveTargetEdgeCamera &&
            (preserveDirectNiriFocus || static_cast<bool>(targetWorkspace->getFocusCandidate()));
        transitionMonitor->changeWorkspace(targetWorkspace, true, true, targetHasFocusCandidateBeforeSwitch);

        if (oldWorkspace && oldWorkspace != targetWorkspace) {
            for (const auto& window : g_pCompositor->m_windows) {
                if (!window || window->m_workspace != oldWorkspace || !window->m_pinned)
                    continue;

                // Match Hyprland's native changeworkspace ordering: the monitor's
                // active workspace flips first, then pinned windows follow.
                window->layoutTarget()->assignToSpace(targetWorkspace->m_space);
            }
        }

        // `internal=true` skips Hyprland's workspace IN animation, so the target
        // workspace can retain its old off-screen renderOffset (e.g. +/- one
        // monitor height). Normalize it immediately or the new active workspace
        // remains visually shifted after the overview transition commits.
        targetWorkspace->m_renderOffset->setValueAndWarp(Vector2D{});
        targetWorkspace->m_alpha->setValueAndWarp(1.F);
        g_layoutManager->recalculateMonitor(transitionMonitor);
        PHLWINDOW targetFocus;
        if (targetIsEmptyNiriWorkspace)
            targetFocus = PHLWINDOW{};
        else if (preserveDirectNiriFocus && intendedTargetFocus && intendedTargetFocus->m_isMapped)
            targetFocus = intendedTargetFocus;
        else if (!preserveTargetEdgeCamera) {
            const bool shouldResolveCenteredFit0Focus = targetWorkspace && next.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(next) &&
                isScrollingWorkspace(targetWorkspace) && getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 0;
            const auto centeredFit0Focus = shouldResolveCenteredFit0Focus ? centeredFocusFit0WindowForScrollingWorkspace(targetWorkspace) : PHLWINDOW{};
            const auto fallbackTargetFocus = focusCandidateForWorkspace(targetWorkspace);
            targetFocus = centeredFit0Focus ? centeredFit0Focus : fallbackTargetFocus;
            if (debugLogsEnabled() && shouldResolveCenteredFit0Focus) {
                std::ostringstream out;
                out << "[hymission] focus-fit0 commit focus resolve"
                    << " workspace=" << debugWorkspaceLabel(targetWorkspace)
                    << " centered=" << debugWindowLabel(centeredFit0Focus)
                    << " fallback=" << debugWindowLabel(fallbackTargetFocus)
                    << " chosen=" << debugWindowLabel(targetFocus);
                debugLog(out.str());
            }
        }
        const bool deferTargetFocusScrollSyncForEdgeRelease = targetStartedEdgeCamera && targetFocus && !preserveTargetEdgeCamera &&
            next.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(next) && isScrollingWorkspace(targetWorkspace) &&
            !scrollingWorkspaceHasSingleColumn(targetWorkspace);
        const bool targetFocusIsStableCenteredFit0 = targetFocus && targetWorkspace && next.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(next) &&
            isScrollingWorkspace(targetWorkspace) && getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 0 &&
            centeredFocusFit0WindowForScrollingWorkspace(targetWorkspace) == targetFocus;
        if (targetFocus) {
            targetWorkspace->m_lastFocusedWindow = targetFocus;
            if (Desktop::focusState()->window() != targetFocus) {
                m_pendingLiveFocusWorkspaceChangeTarget = targetFocus;
                focusWindowCompat(targetFocus, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
                if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == targetFocus)
                    m_pendingLiveFocusWorkspaceChangeTarget.reset();
            }
        }
        if (targetFocus && !deferTargetFocusScrollSyncForEdgeRelease && !targetFocusIsStableCenteredFit0)
            (void)syncScrollingWorkspaceSpotOnWindow(targetFocus);
        else if (!targetFocus)
            clearWindowFocusCompat(transitionMonitor);
        if (g_pAnimationManager)
            g_pAnimationManager->frameTick();

        const bool edgeCameraRepositioned = targetStartedEdgeCamera && !directNiriOwnerEdgeCameraActive(next);
        const bool rebuildDirectNiriTargetAfterFocusSync = targetFocus && !deferTargetFocusScrollSyncForEdgeRelease && !targetFocusIsStableCenteredFit0 &&
            next.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(next) && isScrollingWorkspace(targetWorkspace);
        if (rebuildDirectNiriTargetAfterFocusSync || edgeCameraRepositioned || targetWorkspaceSyntheticEmpty ||
            !containsHandle(next.managedWorkspaces, targetWorkspace) || next.ownerWorkspace != targetWorkspace) {
            const auto rebuildMonitor = m_state.ownerMonitor ? m_state.ownerMonitor : transitionMonitor;
            const std::vector<WorkspaceOverride> overrides = {{
                .monitorId = transitionMonitor->m_id,
                .workspace = targetWorkspace,
                .workspaceId = targetWorkspaceId,
                .workspaceName = targetWorkspaceName,
                .syntheticEmpty = false,
            }};

            if (State rebuilt = buildState(rebuildMonitor, m_state.collectionPolicy.requestedScope, overrides, true, false, targetFocus);
                !rebuilt.participatingMonitors.empty())
                next = std::move(rebuilt);
        }
        if (preserveTargetEdgeCamera) {
            next.selectedIndex.reset();
            next.focusDuringOverview.reset();
        }

        next.phase = Phase::Active;
        next.ownerWorkspace = targetWorkspace;
        refreshWorkspaceStripActivity(next, transitionMonitor, targetWorkspaceId);
        next.focusBeforeOpen = windowMatchesOverviewScope(m_state.focusBeforeOpen, next, false) ? m_state.focusBeforeOpen : PHLWINDOW{};
        next.pendingExitFocus = windowMatchesOverviewScope(m_state.pendingExitFocus, next, false) ? m_state.pendingExitFocus : PHLWINDOW{};
        next.pendingExitWorkspace = containsHandle(next.managedWorkspaces, m_state.pendingExitWorkspace) ? m_state.pendingExitWorkspace : PHLWORKSPACE{};
        next.closeMode = m_state.closeMode;
        next.relayoutActive = false;
        next.relayoutProgress = 1.0;
        next.relayoutStart = {};

        bool windowRelayoutChanged = false;
        if (edgeCameraRepositioned) {
            for (auto& managed : next.windows) {
                const auto previous = std::ranges::find_if(edgeCameraPreviewRects, [&](const auto& candidate) { return candidate.first == managed.window; });
                if (previous == edgeCameraPreviewRects.end())
                    continue;

                managed.relayoutFromGlobal = previous->second;
                windowRelayoutChanged = windowRelayoutChanged || !rectApproxEqual(managed.relayoutFromGlobal, managed.targetGlobal, 0.5);
            }
        }

        bool placeholderRelayoutChanged = false;
        for (auto& placeholder : next.emptyWorkspacePlaceholders) {
            if (!placeholder.monitor || placeholder.workspaceId == WORKSPACE_INVALID)
                continue;

            const auto previousIt = std::find_if(transitionPlaceholderRects.begin(), transitionPlaceholderRects.end(), [&](const auto& previous) {
                return std::get<0>(previous) == placeholder.monitor->m_id && std::get<1>(previous) == placeholder.workspaceId &&
                    std::get<2>(previous) == placeholder.backingOnly;
            });
            if (previousIt == transitionPlaceholderRects.end())
                continue;

            const Rect previousRect = std::get<3>(*previousIt);
            placeholder.relayoutFromGlobal = previousRect;
            if (!rectApproxEqual(previousRect, placeholder.targetGlobal, 0.5))
                placeholderRelayoutChanged = true;
        }

        clearOverviewWorkspaceTransition(targetWorkspace, false);
        const bool stripRelayoutChanged = carryOverWorkspaceStripRelayout(next, m_state);
        carryOverWorkspaceStripSnapshots(next, m_state);
        if (windowRelayoutChanged || stripRelayoutChanged || placeholderRelayoutChanged) {
            next.relayoutActive = true;
            next.relayoutProgress = 0.0;
            next.relayoutStart = {};
        }
        m_state = std::move(next);
        applyWorkspaceNameOverrides(m_state);
        if (targetFocus) {
            selectWindowInState(m_state, targetFocus);
            m_state.focusDuringOverview = targetFocus;
        } else {
            m_state.focusDuringOverview.reset();
            m_state.selectedIndex.reset();
        }
        refreshWorkspaceStripSnapshots();

        // Sync scrolling layout focus after state rebuild.  For scroll-past -> leaf
        // release, do not sync directly here: capture the scroll-past preview first,
        // then let the direct-Niri refresh path rebuild final targets with
        // forceFinalLayoutBox and animate relayoutFromGlobal -> targetGlobal.
        if (deferTargetFocusScrollSyncForEdgeRelease) {
            refreshNiriScrollingOverviewAfterFocusDispatcher("edge-release", targetFocus, true);
        } else if (targetFocus && !targetFocusIsStableCenteredFit0 && m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state) &&
                   isScrollingWorkspace(targetFocus->m_workspace)) {
            (void)syncScrollingWorkspaceSpotOnWindow(targetFocus);
        }

        if (!targetActivatedEarly) {
            if (g_pEventManager) {
                g_pEventManager->postEvent(SHyprIPCEvent{"workspace", targetWorkspace->m_name});
                g_pEventManager->postEvent(SHyprIPCEvent{"workspacev2", std::format("{},{}", targetWorkspace->m_id, targetWorkspace->m_name)});
            }
            Event::bus()->m_events.workspace.active.emit(targetWorkspace);
        }
    }

    // Match movecol's post-workspace-switch safety window for heavier scrolling
    // edits.  swapcol / resizecol / resizeactive can otherwise reach Hyprland
    // right after the workspace transition commits, mutating the layout before
    // the overview relayout animation can start.  Keep movecol/movefocus free,
    // but suppress heavy column/resize edits for the same post-switch settle
    // window used by the overview heavy-edit gate.
    if (m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state)) {
        settleOverviewHeavyEditInputBarrier();
        if (debugLogsEnabled())
            debugLog("[hymission] arm heavy edit delay after workspace switch");
    }

    // Process queued edit dispatchers after the workspace transition commits
    // and state is rebuilt for the new workspace.
    processQueuedEditDispatchers();

    if (temporarilyDisabledAnimations)
        setAnimationsEnabledOverride(false);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview workspace transition commit target=" << targetWorkspace->m_name << " followGesture=" << (followGesture ? 1 : 0);
        debugLog(out.str());
    }

    if (m_rebuildVisibleStateAfterWorkspaceTransitionCommit) {
        m_rebuildVisibleStateAfterWorkspaceTransitionCommit = false;
        rebuildVisibleState();
    } else {
        updateHoveredFromPointer(false, false, false, false, "workspace-transition-commit");
    }
    startNextQueuedOverviewWorkspaceTransition();
    damageOwnedMonitors();
}

void OverviewController::armWorkspaceTransitionRenderState() {
    restoreWorkspaceTransitionRenderState();

    if (!m_workspaceTransition.active)
        return;

    std::vector<PHLWORKSPACE> workspaces;
    const auto appendWorkspace = [&](const PHLWORKSPACE& workspace) {
        if (!workspace || containsHandle(workspaces, workspace))
            return;
        workspaces.push_back(workspace);
    };

    for (const auto& workspace : m_workspaceTransition.sourceState.managedWorkspaces)
        appendWorkspace(workspace);
    for (const auto& workspace : m_workspaceTransition.targetState.managedWorkspaces)
        appendWorkspace(workspace);

    m_workspaceTransitionRenderStateBackups.reserve(workspaces.size());
    for (const auto& workspace : workspaces) {
        m_workspaceTransitionRenderStateBackups.push_back({
            .workspace = workspace,
            .visible = workspace->m_visible,
            .forceRendering = workspace->m_forceRendering,
            .renderOffsetValue = workspace->m_renderOffset->value(),
            .renderOffsetGoal = workspace->m_renderOffset->goal(),
            .alphaValue = workspace->m_alpha->value(),
            .alphaGoal = workspace->m_alpha->goal(),
        });

        workspace->m_visible = true;
        workspace->m_forceRendering = true;
        workspace->m_renderOffset->setValueAndWarp(Vector2D{});
        *workspace->m_renderOffset = Vector2D{};
        workspace->m_alpha->setValueAndWarp(1.F);
        *workspace->m_alpha = 1.F;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] arm workspace transition render state count=" << m_workspaceTransitionRenderStateBackups.size();
        debugLog(out.str());
    }
}

void OverviewController::restoreWorkspaceTransitionRenderState(const PHLWORKSPACE& committedWorkspace) {
    for (const auto& backup : m_workspaceTransitionRenderStateBackups) {
        if (!backup.workspace)
            continue;

        if (committedWorkspace) {
            const auto workspaceMonitor = backup.workspace->m_monitor.lock();
            const bool activeOnMonitor = workspaceMonitor && workspaceMonitor->m_activeWorkspace == backup.workspace;
            const bool committed = backup.workspace == committedWorkspace;

            backup.workspace->m_visible = committed || activeOnMonitor;
            backup.workspace->m_forceRendering = activeOnMonitor && !committed ? backup.forceRendering : false;

            if (backup.workspace->m_visible && !committed) {
                backup.workspace->m_renderOffset->setValueAndWarp(backup.renderOffsetValue);
                if (backup.renderOffsetGoal != backup.renderOffsetValue)
                    *backup.workspace->m_renderOffset = backup.renderOffsetGoal;
                backup.workspace->m_alpha->setValueAndWarp(backup.alphaValue);
                if (std::abs(backup.alphaGoal - backup.alphaValue) > 0.0001F)
                    *backup.workspace->m_alpha = backup.alphaGoal;
            } else {
                backup.workspace->m_renderOffset->setValueAndWarp(Vector2D{});
                *backup.workspace->m_renderOffset = Vector2D{};
                backup.workspace->m_alpha->setValueAndWarp(1.F);
                *backup.workspace->m_alpha = 1.F;
            }
            continue;
        }

        backup.workspace->m_visible = backup.visible;
        backup.workspace->m_forceRendering = backup.forceRendering;
        backup.workspace->m_renderOffset->setValueAndWarp(backup.renderOffsetValue);
        if (backup.renderOffsetGoal != backup.renderOffsetValue)
            *backup.workspace->m_renderOffset = backup.renderOffsetGoal;
        backup.workspace->m_alpha->setValueAndWarp(backup.alphaValue);
        if (std::abs(backup.alphaGoal - backup.alphaValue) > 0.0001F)
            *backup.workspace->m_alpha = backup.alphaGoal;
    }

    m_workspaceTransitionRenderStateBackups.clear();
}

void OverviewController::armOverviewRenderState(const State& state) {
    restoreOverviewRenderState();

    if (state.collectionPolicy.onlyActiveWorkspace)
        return;

    std::vector<PHLWORKSPACE> workspaces;
    workspaces.reserve(state.managedWorkspaces.size());
    const auto appendWorkspace = [&](const PHLWORKSPACE& workspace) {
        if (!workspace || containsHandle(workspaces, workspace))
            return;
        workspaces.push_back(workspace);
    };

    for (const auto& workspace : state.managedWorkspaces)
        appendWorkspace(workspace);

    m_overviewRenderStateBackups.reserve(workspaces.size());
    for (const auto& workspace : workspaces) {
        m_overviewRenderStateBackups.push_back({
            .workspace = workspace,
            .visible = workspace->m_visible,
            .forceRendering = workspace->m_forceRendering,
            .renderOffsetValue = workspace->m_renderOffset->value(),
            .renderOffsetGoal = workspace->m_renderOffset->goal(),
            .alphaValue = workspace->m_alpha->value(),
            .alphaGoal = workspace->m_alpha->goal(),
        });

        workspace->m_visible = true;
        workspace->m_forceRendering = true;
        workspace->m_renderOffset->setValueAndWarp(Vector2D{});
        *workspace->m_renderOffset = Vector2D{};
        workspace->m_alpha->setValueAndWarp(1.F);
        *workspace->m_alpha = 1.F;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] arm overview render state count=" << m_overviewRenderStateBackups.size();
        debugLog(out.str());
    }
}

void OverviewController::restoreOverviewRenderState() {
    for (const auto& backup : m_overviewRenderStateBackups) {
        if (!backup.workspace)
            continue;

        const auto workspaceMonitor = backup.workspace->m_monitor.lock();
        const bool activeRegular = workspaceMonitor && workspaceMonitor->m_activeWorkspace == backup.workspace;
        const bool activeSpecial = workspaceMonitor && workspaceMonitor->m_activeSpecialWorkspace == backup.workspace;
        const bool activeOnMonitor = activeRegular || activeSpecial;

        // Real focus may switch workspaces while all-scope overview is open, so restore
        // visibility from the compositor's current active workspace, not the opening snapshot.
        backup.workspace->m_visible = activeOnMonitor;
        backup.workspace->m_forceRendering = activeOnMonitor ? backup.forceRendering : false;
        backup.workspace->m_renderOffset->setValueAndWarp(Vector2D{});
        *backup.workspace->m_renderOffset = Vector2D{};
        backup.workspace->m_alpha->setValueAndWarp(1.F);
        *backup.workspace->m_alpha = 1.F;
    }

    m_overviewRenderStateBackups.clear();
}

// Process queued edit dispatchers (movefocus, movecol, swapcol) that were
// deferred during an active workspace transition. Call this after the
// transition commits and state is rebuilt for the new workspace.
void OverviewController::processQueuedEditDispatchers() {
    bool anyDispatcherRan = false;
    while (!m_pendingEditDispatchers.empty()) {
        auto pending = std::move(m_pendingEditDispatchers.front());
        m_pendingEditDispatchers.pop_front();

        if (debugLogsEnabled()) {
            const bool overviewActive = isVisible() && m_state.phase == Phase::Active;
            const bool directNiri = activeDirectNiriSingleWorkspaceOverview();
            bool directLiveGeometryAvailable = false;
            if (overviewActive && directNiri) {
                directLiveGeometryAvailable = std::ranges::all_of(m_state.windows, [&](const ManagedWindow& managed) {
                    return !managed.window || !managed.window->m_isMapped || static_cast<bool>(livePreviewRectForManagedWindow(managed));
                });
            }
            const auto selected = selectedWindow();
            const auto focused = Desktop::focusState()->window();
            const auto focusOverview = m_state.focusDuringOverview;
            const auto activeWorkspace = activeLayoutWorkspace();
            
            std::ostringstream out;
            out << "[hymission] process queued edit dispatcher BEGIN"
                << " dispatcher=" << pending.dispatcherName
                << " args=" << pending.args
                << " overviewActive=" << (overviewActive ? 1 : 0)
                << " directNiri=" << (directNiri ? 1 : 0)
                << " directLiveGeometryAvailable=" << (directLiveGeometryAvailable ? 1 : 0)
                << " selected=" << debugWindowLabel(selected)
                << " focusDuringOverview=" << debugWindowLabel(focusOverview)
                << " activeWindow=" << debugWindowLabel(focused)
                << " activeWorkspace=" << debugWorkspaceLabel(activeWorkspace);
            if (activeWorkspace) {
                auto* scrolling = scrollingAlgorithmForWorkspace(activeWorkspace);
                if (scrolling && scrolling->m_scrollingData && scrolling->m_scrollingData->controller) {
                    out << " scrollOffset=" << scrolling->m_scrollingData->controller->getOffset()
                        << " columns=" << scrolling->m_scrollingData->columns.size();
                    for (std::size_t i = 0; i < scrolling->m_scrollingData->columns.size(); ++i) {
                        auto col = scrolling->m_scrollingData->columns[i];
                        if (col) {
                            auto lft = col->lastFocusedTarget.lock();
                            out << " col#" << i << " lastFocused=" << debugWindowLabel(lft ? lft->target.lock()->window() : PHLWINDOW{});
                        }
                    }
                }
            }
            debugLog(out.str());
        }

        // Before running the queued dispatcher, force-sync overview focus state
        // to the current Hyprland focus. This fixes a bug where queued
        // movefocus/movecol/swapcol dispatchers (triggered during a workspace
        // transition) would run with stale focus data from before the transition.
        if (pending.original && *pending.original) {
            const auto currentFocus = Desktop::focusState()->window();
            if (currentFocus && currentFocus->m_isMapped && hasManagedWindow(currentFocus)) {
                // Sync overview internal focus state
                selectWindowInState(m_state, currentFocus);
                m_state.focusDuringOverview = currentFocus;
                // Sync scrolling layout focus for the current workspace
                if (m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state)) {
                    const auto currentWorkspace = currentFocus->m_pinned ? activeLayoutWorkspace() : currentFocus->m_workspace;
                    if (currentWorkspace && isScrollingWorkspace(currentWorkspace)) {
                        (void)syncScrollingWorkspaceSpotOnWindow(currentFocus);
                    }
                }
                if (debugLogsEnabled()) {
                    std::ostringstream out;
                    out << "[hymission] process queued edit dispatcher pre-sync"
                        << " forcedFocus=" << debugWindowLabel(currentFocus)
                        << " selected=" << (*m_state.selectedIndex < m_state.windows.size() ? debugWindowLabel(m_state.windows[*m_state.selectedIndex].window) : std::string("<none>"))
                        << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview);
                    debugLog(out.str());
                }
            }
        }

        if (pending.original && *pending.original) {
            // Run the dispatcher with the updated state
            (void)runOverviewEditingDispatcher(pending.dispatcherName.c_str(), pending.original, std::move(pending.args));
            anyDispatcherRan = true;
        }

        if (debugLogsEnabled()) {
            const auto selectedAfter = selectedWindow();
            const auto focusOverviewAfter = m_state.focusDuringOverview;
            const auto focusedAfter = Desktop::focusState()->window();
            const auto activeWorkspaceAfter = activeLayoutWorkspace();
            
            std::ostringstream out;
            out << "[hymission] process queued edit dispatcher END"
                << " selected=" << debugWindowLabel(selectedAfter)
                << " focusDuringOverview=" << debugWindowLabel(focusOverviewAfter)
                << " activeWindow=" << debugWindowLabel(focusedAfter)
                << " activeWorkspace=" << debugWorkspaceLabel(activeWorkspaceAfter);
            if (activeWorkspaceAfter) {
                auto* scrolling = scrollingAlgorithmForWorkspace(activeWorkspaceAfter);
                if (scrolling && scrolling->m_scrollingData && scrolling->m_scrollingData->controller) {
                    for (std::size_t i = 0; i < scrolling->m_scrollingData->columns.size(); ++i) {
                        auto col = scrolling->m_scrollingData->columns[i];
                        if (col) {
                            auto lft = col->lastFocusedTarget.lock();
                            out << " col#" << i << " lastFocused=" << debugWindowLabel(lft ? lft->target.lock()->window() : PHLWINDOW{});
                        }
                    }
                }
            }
            debugLog(out.str());
        }
    }

    // After all queued dispatchers have run, do a final focus sync to ensure
    // the overview state and scrolling layout's lastFocusedTarget match the
    // actual Hyprland focus. This prevents the focus border from getting stuck
    // on a stale window when multiple movecol/movefocus dispatchers were queued
    // during a workspace transition.
    if (anyDispatcherRan && isVisible() && m_state.phase == Phase::Active &&
        m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state)) {
        const auto finalFocus = Desktop::focusState()->window();
        if (finalFocus && finalFocus->m_isMapped && hasManagedWindow(finalFocus)) {
            const auto currentWorkspace = finalFocus->m_pinned ? activeLayoutWorkspace() : finalFocus->m_workspace;
            if (currentWorkspace && isScrollingWorkspace(currentWorkspace)) {
                // Sync overview internal focus state
                selectWindowInState(m_state, finalFocus);
                m_state.focusDuringOverview = finalFocus;
                // Sync scrolling layout focus for the current workspace
                (void)syncScrollingWorkspaceSpotOnWindow(finalFocus);
                // Do a complete visible state rebuild with the final focus as anchor.
                // This ensures the overview state, preview rects, and strip entries
                // are all consistent with the current Hyprland focus and layout.
                rebuildVisibleState(finalFocus, true);
                // Post-rebuild: ensure scrolling layout's lastFocusedTarget matches
                // the overview's focusDuringOverview. This is critical because
                // rebuildVisibleState doesn't call the post-rebuild sync that
                // refreshVisibleStateMetadata does.
                if (m_state.focusDuringOverview && m_state.collectionPolicy.onlyActiveWorkspace &&
                    niriModeAppliesToState(m_state) &&
                    isScrollingWorkspace(m_state.focusDuringOverview->m_workspace)) {
                    (void)syncScrollingWorkspaceSpotOnWindow(m_state.focusDuringOverview);
                }
                // Refresh strip activity in case the active workspace changed
                if (refreshWorkspaceStripActivity(m_state)) {
                    if (debugLogsEnabled()) {
                        debugLog("[hymission] refreshed workspace strip activity after final focus sync");
                    }
                }
                if (debugLogsEnabled()) {
                    std::ostringstream out;
                    out << "[hymission] process queued edit dispatchers final sync"
                        << " forcedFocus=" << debugWindowLabel(finalFocus)
                        << " selected=" << (*m_state.selectedIndex < m_state.windows.size() ? debugWindowLabel(m_state.windows[*m_state.selectedIndex].window) : std::string("<none>"))
                        << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview);
                    debugLog(out.str());
                }
                damageOwnedMonitors();
            }
        }
    }
}

void OverviewController::clearOverviewWorkspaceTransition(const PHLWORKSPACE& committedWorkspace, bool clearPendingRequests) {
    // For revert/abort cases (clearPendingRequests=true), process any queued
    // edit dispatchers on the current workspace before clearing the transition.
    if (clearPendingRequests)
        processQueuedEditDispatchers();

    m_workspaceTransitionAnimation.reset();
    restoreWorkspaceTransitionRenderState(committedWorkspace);
    clearPendingWindowGeometryRetry();
    m_workspaceTransitionCommitScheduled = false;
    m_pendingWorkspaceTransitionCommitFollowGesture = false;
    ++m_workspaceTransitionCommitGeneration;
    m_workspaceTransition = {};
    if (clearPendingRequests)
        m_pendingWorkspaceTransitionRequests.clear();
}

void OverviewController::commitActiveNiriWorkspaceTransitionForRetarget() {
    if (!m_workspaceTransition.active || m_workspaceTransition.mode != WorkspaceTransitionMode::TimedCommit)
        return;

    const bool niriSingleWorkspaceTransition = m_state.collectionPolicy.onlyActiveWorkspace &&
        (niriModeAppliesToState(m_workspaceTransition.sourceState) || niriModeAppliesToState(m_workspaceTransition.targetState));
    if (!niriSingleWorkspaceTransition)
        return;

    std::vector<std::pair<PHLWINDOW, Rect>> targetPreviewRects;
    targetPreviewRects.reserve(m_workspaceTransition.targetState.windows.size());
    for (const auto& managed : m_workspaceTransition.targetState.windows) {
        if (!managed.window)
            continue;
        if (const auto rect = workspaceTransitionRectForWindow(managed.window); rect)
            targetPreviewRects.emplace_back(managed.window, *rect);
    }

    const auto targetWorkspaceId = m_workspaceTransition.targetWorkspaceId;
    commitOverviewWorkspaceTransition(false, true);
    if (m_workspaceTransition.active || m_state.phase != Phase::Active)
        return;

    bool windowRelayoutChanged = false;
    for (auto& managed : m_state.windows) {
        const auto previous = std::find_if(targetPreviewRects.begin(), targetPreviewRects.end(),
                                           [&](const auto& candidate) { return candidate.first == managed.window; });
        if (previous == targetPreviewRects.end())
            continue;

        managed.relayoutFromGlobal = previous->second;
        windowRelayoutChanged = windowRelayoutChanged || !rectApproxEqual(managed.relayoutFromGlobal, managed.targetGlobal, 0.5);
    }

    const bool preservedRelayout = windowRelayoutChanged || m_state.relayoutActive;
    if (preservedRelayout) {
        m_relayoutProgressAnimation.reset();
        m_state.relayoutActive = true;
        m_state.relayoutProgress = 0.0;
        m_state.relayoutStart = {};
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] retarget overview workspace transition committedTarget=" << targetWorkspaceId
            << " preservedWindows=" << targetPreviewRects.size()
            << " relayout=" << (preservedRelayout ? 1 : 0);
        debugLog(out.str());
    }
}

void OverviewController::startNextQueuedOverviewWorkspaceTransition() {
    while (!m_workspaceTransition.active && !m_pendingWorkspaceTransitionRequests.empty()) {
        auto request = std::move(m_pendingWorkspaceTransitionRequests.front());
        m_pendingWorkspaceTransitionRequests.pop_front();
        (void)startOverviewWorkspaceTransitionForDispatcher(request.args, request.currentMonitorOnly);
    }
}

SDispatchResult OverviewController::startOverviewWorkspaceTransitionForDispatcher(const std::string& args, bool currentMonitorOnly) {
    if (m_beginCloseInProgress || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return {};

    if (m_workspaceTransition.active)
        commitActiveNiriWorkspaceTransitionForRetarget();

    if (m_workspaceTransition.active) {
        m_pendingWorkspaceTransitionRequests.push_back({
            .args = args,
            .currentMonitorOnly = currentMonitorOnly,
        });
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] queue overview workspace transition args=" << args
                << " currentMonitorOnly=" << (currentMonitorOnly ? 1 : 0)
                << " pending=" << m_pendingWorkspaceTransitionRequests.size();
            debugLog(out.str());
        }
        return {};
    }

    if (m_state.phase != Phase::Active)
        return {};

    const auto monitor = Desktop::focusState()->monitor();
    if (!monitor || !monitor->m_activeWorkspace)
        return {.success = false, .error = "no active monitor for overview workspace transition"};

    if (!allowsWorkspaceSwitchInOverview())
        return {.success = false, .error = "overview workspace transition unavailable"};

    WORKSPACEID targetId = WORKSPACE_INVALID;
    std::string targetName;
    PHLWORKSPACE targetWorkspace;
    bool syntheticEmpty = false;

    if (currentMonitorOnly) {
        auto [workspaceId, workspaceName, isAutoID] = getWorkspaceIDNameFromString(args);
        if (workspaceId == WORKSPACE_INVALID)
            return {.success = false, .error = "focusWorkspaceOnCurrentMonitor invalid workspace!"};

        targetId = workspaceId;
        targetName = workspaceName;
        targetWorkspace = g_pCompositor->getWorkspaceByID(targetId);
        if (targetWorkspace && targetWorkspace->m_monitor.lock() != monitor)
            return {.success = false, .error = "focusWorkspaceOnCurrentMonitor workspace is on another monitor"};
        syntheticEmpty = !targetWorkspace;
    } else {
        const auto currentWorkspace = monitor->m_activeWorkspace;
        if (!currentWorkspace)
            return {.success = false, .error = "Last monitor not found"};

        auto resolveWorkspaceToChange = [&](std::string value) -> SWorkspaceIDName {
            if (!value.starts_with("previous"))
                return getWorkspaceIDNameFromString(value);

            const bool perMonitor = value.contains("_per_monitor");
            const auto previous = perMonitor ? Desktop::History::workspaceTracker()->previousWorkspaceIDName(currentWorkspace, monitor) :
                                               Desktop::History::workspaceTracker()->previousWorkspaceIDName(currentWorkspace);
            if (previous.id == -1 || previous.id == currentWorkspace->m_id)
                return {.id = WORKSPACE_NOT_CHANGED};

            if (const auto existing = g_pCompositor->getWorkspaceByID(previous.id); existing)
                return {.id = existing->m_id, .name = existing->m_name};

            return {.id = previous.id, .name = previous.name.empty() ? std::to_string(previous.id) : previous.name};
        };

        static auto PBACKANDFORTH = CConfigValue<Hyprlang::INT>("binds:workspace_back_and_forth");
        const bool explicitPrevious = args.contains("previous");
        const auto resolved = resolveWorkspaceToChange(args);
        if (resolved.id == WORKSPACE_INVALID)
            return {.success = false, .error = "Error in changeworkspace, invalid value"};
        if (resolved.id == WORKSPACE_NOT_CHANGED)
            return {};

        const auto previousWorkspace = args.contains("_per_monitor") ? Desktop::History::workspaceTracker()->previousWorkspaceIDName(currentWorkspace, monitor) :
                                                                       Desktop::History::workspaceTracker()->previousWorkspaceIDName(currentWorkspace);
        const bool targetCurrent = resolved.id == currentWorkspace->m_id;
        if (targetCurrent && ((!*PBACKANDFORTH && !explicitPrevious) || previousWorkspace.id == -1))
            return {};

        targetId = targetCurrent ? previousWorkspace.id : resolved.id;
        targetName = targetCurrent ? (previousWorkspace.name.empty() ? std::to_string(previousWorkspace.id) : previousWorkspace.name) : resolved.name;
        targetWorkspace = g_pCompositor->getWorkspaceByID(targetId);
        if (targetWorkspace && targetWorkspace->m_isSpecialWorkspace)
            return {.success = false, .error = "overview workspace transition does not support special workspaces"};
        if (targetWorkspace && targetWorkspace->m_monitor.lock() != monitor)
            return {.success = false, .error = "overview workspace transition requires workspace on current monitor"};
        syntheticEmpty = !targetWorkspace;
    }

    if (!beginOverviewWorkspaceTransition(monitor, targetId, targetName, targetWorkspace, syntheticEmpty, WorkspaceTransitionMode::TimedCommit))
        return {.success = false, .error = "failed to start overview workspace transition"};

    return {};
}

void OverviewController::setInputFollowMouseOverride(bool disable) {
    if (disable) {
        if (m_inputFollowMouseOverridden)
            return;

        m_inputFollowMouseBackup = getConfigInt(m_handle, "input:follow_mouse", 0);
        const auto err = setConfigKeyword("input:follow_mouse", "0");
        if (!err.empty()) {
            notify("[hymission] failed to disable input:follow_mouse", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
            return;
        }

        m_inputFollowMouseOverridden = true;
        return;
    }

    if (!m_inputFollowMouseOverridden)
        return;

    const auto err = setConfigKeyword("input:follow_mouse", std::to_string(m_inputFollowMouseBackup));
    if (!err.empty()) {
        notify("[hymission] failed to restore input:follow_mouse", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return;
    }

    m_inputFollowMouseOverridden = false;
}

void OverviewController::setScrollingFollowFocusOverride(bool disable) {
    if (!disable && m_restoreScrollingFollowFocusAfterScrollMouseMove)
        m_restoreScrollingFollowFocusAfterScrollMouseMove = false;

    if (!hasScrollingWorkspace() && !isScrollingWorkspace(activeLayoutWorkspace()))
        return;

    if (disable) {
        if (m_scrollingFollowFocusOverridden)
            return;

        m_scrollingFollowFocusBackup = getConfigInt(m_handle, "scrolling:follow_focus", 0);
        const auto err = setConfigKeyword("scrolling:follow_focus", "0");
        if (!err.empty()) {
            notify("[hymission] failed to disable scrolling:follow_focus", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
            return;
        }

        m_scrollingFollowFocusOverridden = true;
        return;
    }

    if (!m_scrollingFollowFocusOverridden)
        return;

    const auto err = setConfigKeyword("scrolling:follow_focus", std::to_string(m_scrollingFollowFocusBackup));
    if (!err.empty()) {
        notify("[hymission] failed to restore scrolling:follow_focus", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return;
    }

    m_scrollingFollowFocusOverridden = false;
}

void OverviewController::setAnimationsEnabledOverride(bool disable, std::optional<std::chrono::milliseconds> restoreDelay) {
    if (disable) {
        if (!m_animationsEnabledOverridden) {
            m_animationsEnabledBackup = getConfigInt(m_handle, "animations:enabled", 1);
            if (m_animationsEnabledBackup == 0)
                return;

            const auto err = setConfigKeyword("animations:enabled", "0");
            if (!err.empty()) {
                notify("[hymission] failed to disable animations:enabled", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
                return;
            }

            m_animationsEnabledOverridden = true;
            if (debugLogsEnabled())
                debugLog("[hymission] disabled animations:enabled");
        }

        if (m_animationsEnabledOverridden && restoreDelay) {
            if (!m_animationsEnabledRestoreTimer) {
                m_animationsEnabledRestoreTimer = makeShared<CEventLoopTimer>(
                    *restoreDelay,
                    [this](SP<CEventLoopTimer> self, void* data) { setAnimationsEnabledOverride(false); },
                    nullptr);
                g_pEventLoopManager->addTimer(m_animationsEnabledRestoreTimer);
            } else {
                m_animationsEnabledRestoreTimer->updateTimeout(*restoreDelay);
            }

            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] animations restore scheduled in " << restoreDelay->count() << "ms";
                debugLog(out.str());
            }
        }

        return;
    }

    if (m_animationsEnabledRestoreTimer) {
        g_pEventLoopManager->removeTimer(m_animationsEnabledRestoreTimer);
        m_animationsEnabledRestoreTimer.reset();
    }

    if (!m_animationsEnabledOverridden)
        return;

    const auto err = setConfigKeyword("animations:enabled", std::to_string(m_animationsEnabledBackup));
    if (!err.empty()) {
        notify("[hymission] failed to restore animations:enabled", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return;
    }

    m_animationsEnabledOverridden = false;
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] restored animations:enabled=" << m_animationsEnabledBackup;
        debugLog(out.str());
    }
}

void OverviewController::setDamageTrackingOverride(bool disable) {
    if (disable) {
        if (!damageTrackingOverrideEnabled() || m_damageTrackingOverridden)
            return;

        m_damageTrackingBackup = getConfigInt(m_handle, "debug:damage_tracking", 2);
        if (m_damageTrackingBackup == 0)
            return;

        const auto err = setConfigKeyword("debug:damage_tracking", "0");
        if (!err.empty()) {
            notify("[hymission] failed to disable debug:damage_tracking", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
            return;
        }

        m_damageTrackingOverridden = true;
        if (debugLogsEnabled())
            debugLog("[hymission] disabled debug:damage_tracking");
        return;
    }

    if (!m_damageTrackingOverridden)
        return;

    const auto err = setConfigKeyword("debug:damage_tracking", std::to_string(m_damageTrackingBackup));
    if (!err.empty()) {
        notify("[hymission] failed to restore debug:damage_tracking", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return;
    }

    m_damageTrackingOverridden = false;
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] restored debug:damage_tracking=" << m_damageTrackingBackup;
        debugLog(out.str());
    }
}

void OverviewController::closeActiveSpecialWorkspaces() {
    if (!closeSpecialWorkspacesOnOpenEnabled())
        return;

    std::vector<PHLWORKSPACE> specialWorkspaces;
    for (const auto& monitor : g_pCompositor->m_monitors) {
        if (!monitor || !monitor->m_activeSpecialWorkspace || containsHandle(specialWorkspaces, monitor->m_activeSpecialWorkspace))
            continue;

        specialWorkspaces.push_back(monitor->m_activeSpecialWorkspace);
    }

    for (const auto& workspace : specialWorkspaces) {
        if (!workspace)
            continue;

        const auto result = Config::Actions::toggleSpecial(workspace);
        if (!result && debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] failed to close special workspace " << debugWorkspaceLabel(workspace) << ": " << result.error().message;
            debugLog(out.str());
        }
    }
}

bool OverviewController::installHooks() {
    const auto activateOptionalHook = [&](CFunctionHook*& hook, auto& original, const char* label) {
        if (!hook)
            return;

        if (!hook->hook()) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] optional hook activation failed: " << label;
                debugLog(out.str());
            }
            HyprlandAPI::removeFunctionHook(m_handle, hook);
            hook = nullptr;
            original = nullptr;
            return;
        }

        using OriginalT = std::remove_reference_t<decltype(original)>;
        original = reinterpret_cast<OriginalT>(hook->m_original);
    };

    if (hookFunction("handleGesture", "CConfigManager::handleGesture(", m_handleGestureHook, reinterpret_cast<void*>(&hkHandleGesture))) {
        if (m_handleGestureHook->hook()) {
            m_handleGestureOriginal = reinterpret_cast<HandleGestureFn>(m_handleGestureHook->m_original);
        } else {
            notify("[hymission] gesture config hook unavailable; dispatcher controls still work", CHyprColor(1.0, 0.65, 0.2, 1.0), 4000);
            HyprlandAPI::removeFunctionHook(m_handle, m_handleGestureHook);
            m_handleGestureHook = nullptr;
            m_handleGestureOriginal = nullptr;
        }
    } else {
        notify("[hymission] gesture config hook not found; dispatcher controls still work", CHyprColor(1.0, 0.65, 0.2, 1.0), 4000);
    }

    if (!hookFunction("shouldRenderWindow",
                      std::vector<std::string>{
                          "IHyprRenderer::shouldRenderWindow(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<CMonitor>)",
                          "CHyprRenderer::shouldRenderWindow(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<CMonitor>)"},
                      m_shouldRenderWindowHook, reinterpret_cast<void*>(&hkShouldRenderWindow))) {
        notify("[hymission] failed to hook shouldRenderWindow(window, monitor)", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    (void)hookFunction("renderLayer", std::vector<std::string>{"IHyprRenderer::renderLayer(", "CHyprRenderer::renderLayer("}, m_renderLayerHook,
                       reinterpret_cast<void*>(&hkRenderLayer));

    if (!hookFunction("getTexBox", "CSurfacePassElement::getTexBox(", m_surfaceTexBoxHook, reinterpret_cast<void*>(&hkSurfaceTexBox))) {
        notify("[hymission] failed to hook getTexBox", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("boundingBox", "CSurfacePassElement::boundingBox(", m_surfaceBoundingBoxHook, reinterpret_cast<void*>(&hkSurfaceBoundingBox))) {
        notify("[hymission] failed to hook boundingBox", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("opaqueRegion", "CSurfacePassElement::opaqueRegion(", m_surfaceOpaqueRegionHook, reinterpret_cast<void*>(&hkSurfaceOpaqueRegion))) {
        notify("[hymission] failed to hook opaqueRegion", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("visibleRegion", "CSurfacePassElement::visibleRegion(", m_surfaceVisibleRegionHook, reinterpret_cast<void*>(&hkSurfaceVisibleRegion))) {
        notify("[hymission] failed to hook visibleRegion", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("draw", "IPassElement::draw(", m_surfaceDrawHook, reinterpret_cast<void*>(&hkSurfaceDraw))) {
        notify("[hymission] failed to hook surface draw", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("needsLiveBlur", "CSurfacePassElement::needsLiveBlur(", m_surfaceNeedsLiveBlurHook, reinterpret_cast<void*>(&hkSurfaceNeedsLiveBlur))) {
        notify("[hymission] failed to hook surface needsLiveBlur", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("needsPrecomputeBlur", "CSurfacePassElement::needsPrecomputeBlur(", m_surfaceNeedsPrecomputeBlurHook,
                      reinterpret_cast<void*>(&hkSurfaceNeedsPrecomputeBlur))) {
        notify("[hymission] failed to hook surface needsPrecomputeBlur", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("draw", "CHyprBorderDecoration::draw(", m_borderDrawHook, reinterpret_cast<void*>(&hkBorderDraw))) {
        notify("[hymission] failed to hook border decoration draw", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("draw", "CHyprDropShadowDecoration::draw(", m_shadowDrawHook, reinterpret_cast<void*>(&hkShadowDraw))) {
        notify("[hymission] failed to hook shadow decoration draw", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("calculateUVForSurface",
                      std::vector<std::string>{"IElementRenderer::calculateUVForSurface(", "CHyprRenderer::calculateUVForSurface("},
                      m_calculateUVForSurfaceHook, reinterpret_cast<void*>(&hkCalculateUVForSurface))) {
        notify("[hymission] failed to hook calculateUVForSurface", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!wrapDispatcher("fullscreen", m_fullscreenActiveOriginal, [this](std::string args) { return fullscreenDispatcherHook(std::move(args)); })) {
        notify("[hymission] failed to wrap fullscreen dispatcher", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }
    m_fullscreenActiveDispatcherWrapped = true;

    if (!wrapDispatcher("fullscreenstate", m_fullscreenStateActiveOriginal, [this](std::string args) { return fullscreenStateDispatcherHook(std::move(args)); })) {
        notify("[hymission] failed to wrap fullscreenstate dispatcher", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }
    m_fullscreenStateDispatcherWrapped = true;

    m_changeWorkspaceDispatcherWrapped =
        wrapDispatcher("workspace", m_changeWorkspaceOriginal, [this](std::string args) { return changeWorkspaceDispatcherHook(std::move(args)); });
    m_focusWorkspaceOnCurrentMonitorDispatcherWrapped = wrapDispatcher(
        "focusworkspaceoncurrentmonitor",
        m_focusWorkspaceOnCurrentMonitorOriginal,
        [this](std::string args) { return focusWorkspaceOnCurrentMonitorDispatcherHook(std::move(args)); });
    m_layoutMessageDispatcherName = "layoutmsg";
    m_layoutMessageDispatcherWrapped =
        wrapDispatcher("layoutmsg", m_layoutMessageOriginal, [this](std::string args) { return layoutMessageDispatcherHook(std::move(args)); });
    if (!m_layoutMessageDispatcherWrapped) {
        m_layoutMessageDispatcherWrapped =
            wrapDispatcher("layout", m_layoutMessageOriginal, [this](std::string args) { return runOverviewEditingDispatcher("layout", &m_layoutMessageOriginal, std::move(args)); });
        if (m_layoutMessageDispatcherWrapped)
            m_layoutMessageDispatcherName = "layout";
    }
    m_moveFocusDispatcherWrapped =
        wrapDispatcher("movefocus", m_moveFocusOriginal, [this](std::string args) { return moveFocusDispatcherHook(std::move(args)); });

    m_overviewEditingDispatchersOriginal.clear();
    std::vector<std::string> overviewEditingDispatchers = {
        "movewindow",
        "movewindoworgroup",
        "swapwindow",
        "movetoworkspace",
        "movetoworkspacesilent",
        "moveactive",
        "resizeactive",
        "resizewindow",
        "resizecol",
        "resizecolumn",
        "togglefloating",
        "setfloating",
        "settiled",
        "pin",
        "movecol",
        "movecolumn",
        "swapcol",
        "swapcolumn",
        "movewindowpixel",
        "resizewindow",

        "resizewindowpixel",
        "window.move",
        "window.swap",
        "window.resize",
        "window.workspace",
    };
    if (g_pKeybindManager) {
        for (const auto& [name, _] : g_pKeybindManager->m_dispatchers) {
            if (!isOverviewEditingDispatcherCandidate(name))
                continue;
            if (std::ranges::find(overviewEditingDispatchers, name) == overviewEditingDispatchers.end())
                overviewEditingDispatchers.push_back(name);
        }
    }

    const auto wrapOverviewEditingDispatcher = [this](const std::string& name) {
        DispatcherHandler original;
        const bool wrapped = wrapDispatcher(name, original, [this, name](std::string args) -> SDispatchResult {
            const auto it = m_overviewEditingDispatchersOriginal.find(name);
            if (it == m_overviewEditingDispatchersOriginal.end())
                return {};
            return runOverviewEditingDispatcher(name.c_str(), &it->second, std::move(args));
        });
        if (wrapped)
            m_overviewEditingDispatchersOriginal[name] = std::move(original);
    };

    for (const auto& dispatcher : overviewEditingDispatchers)
        wrapOverviewEditingDispatcher(dispatcher);

    if (g_pKeybindManager) {
        std::vector<std::string> dispatcherNames;
        dispatcherNames.reserve(g_pKeybindManager->m_dispatchers.size());
        for (const auto& [name, _] : g_pKeybindManager->m_dispatchers)
            dispatcherNames.push_back(name);

        for (const auto& name : dispatcherNames) {
            if (g_openingDispatcherGateOriginals.contains(name))
                continue;

            const auto it = g_pKeybindManager->m_dispatchers.find(name);
            if (it == g_pKeybindManager->m_dispatchers.end())
                continue;

            auto original = it->second;
            if (!original)
                continue;

            g_openingDispatcherGateOriginals[name] = original;
            it->second = [this, name](std::string args) -> SDispatchResult {
                const std::string dispatcherLower = asciiLowerCopy(name);
                const bool hymissionControlDispatcher = isOverviewToggleControlDispatcher(dispatcherLower);
                const bool openDispatcherCooldownActive = overviewOpenInputBarrierActive();
                const bool delayedHeavyEditCooldownActive = isDelayedOverviewOpenEditCommand(name, args) && overviewHeavyEditInputBarrierActive();
                const bool openVisibilityAnimationActive =
                    m_overviewVisibilityAnimation && m_overviewVisibilityAnimation->isBeingAnimated();
                const bool openingNiriSingleWorkspaceDispatcherGate = !hymissionControlDispatcher && isVisible() &&
                    m_state.collectionPolicy.onlyActiveWorkspace && niriModeEnabled() &&
                    (m_state.phase == Phase::Opening || openVisibilityAnimationActive || m_postOpenRefreshFrames > 0 || openDispatcherCooldownActive ||
                     delayedHeavyEditCooldownActive);

                if (openingNiriSingleWorkspaceDispatcherGate) {
                    if (debugLogsEnabled()) {
                        std::ostringstream out;
                        out << "[hymission] block dispatcher during overview open"
                            << " dispatcher=" << name
                            << " args=" << args
                            << " phase=" << static_cast<int>(m_state.phase)
                            << " openBarrier=" << (openDispatcherCooldownActive ? 1 : 0)
                            << " heavyBarrier=" << (delayedHeavyEditCooldownActive ? 1 : 0);
                        debugLog(out.str());
                    }
                    return {};
                }

                const auto originalIt = g_openingDispatcherGateOriginals.find(name);
                if (originalIt == g_openingDispatcherGateOriginals.end() || !originalIt->second)
                    return {};

                return originalIt->second(std::move(args));
            };
        }
    }

    (void)hookFunction("begin", "CWorkspaceSwipeGesture::begin(", m_workspaceSwipeBeginFunctionHook, reinterpret_cast<void*>(&hkWorkspaceSwipeBegin));
    (void)hookFunction("update", "CWorkspaceSwipeGesture::update(", m_workspaceSwipeUpdateFunctionHook, reinterpret_cast<void*>(&hkWorkspaceSwipeUpdate));
    (void)hookFunction("end", "CWorkspaceSwipeGesture::end(", m_workspaceSwipeEndFunctionHook, reinterpret_cast<void*>(&hkWorkspaceSwipeEnd));
    (void)hookFunction("begin", "CUnifiedWorkspaceSwipeGesture::begin(", m_unifiedWorkspaceSwipeBeginFunctionHook,
                       reinterpret_cast<void*>(&hkUnifiedWorkspaceSwipeBegin));
    (void)hookFunction("update", "CUnifiedWorkspaceSwipeGesture::update(", m_unifiedWorkspaceSwipeUpdateFunctionHook,
                       reinterpret_cast<void*>(&hkUnifiedWorkspaceSwipeUpdate));
    (void)hookFunction("end", "CUnifiedWorkspaceSwipeGesture::end(", m_unifiedWorkspaceSwipeEndFunctionHook,
                       reinterpret_cast<void*>(&hkUnifiedWorkspaceSwipeEnd));
    (void)hookFunction("begin", "CScrollMoveTrackpadGesture::begin(", m_scrollMoveGestureBeginFunctionHook, reinterpret_cast<void*>(&hkScrollMoveGestureBegin));
    (void)hookFunction("update", "CScrollMoveTrackpadGesture::update(", m_scrollMoveGestureUpdateFunctionHook, reinterpret_cast<void*>(&hkScrollMoveGestureUpdate));
    (void)hookFunction("end", "CScrollMoveTrackpadGesture::end(", m_scrollMoveGestureEndFunctionHook, reinterpret_cast<void*>(&hkScrollMoveGestureEnd));
    (void)hookFunction("moveToWorkspace", "Config::Actions::moveToWorkspace(", m_moveToWorkspaceActionFunctionHook,
                       reinterpret_cast<void*>(&hkMoveToWorkspaceAction));
    (void)hookFunction("layoutMessage", "Config::Actions::layoutMessage(", g_layoutMessageActionHook, reinterpret_cast<void*>(&hkLayoutMessageAction));
    (void)hookFunction("moveFocus", "Config::Actions::moveFocus(", g_moveFocusActionHook, reinterpret_cast<void*>(&hkMoveFocusAction));
    (void)hookFunction("moveInDirection", "Config::Actions::moveInDirection(", g_moveInDirectionActionHook, reinterpret_cast<void*>(&hkMoveInDirectionAction));
    (void)hookFunction("swapInDirection", "Config::Actions::swapInDirection(", g_swapInDirectionActionHook, reinterpret_cast<void*>(&hkSwapInDirectionAction));
    (void)hookFunction("resize", "Config::Actions::resize(", g_resizeActionHook, reinterpret_cast<void*>(&hkResizeAction));

    m_shouldRenderWindowOriginal = nullptr;
    m_surfaceTexBoxOriginal = nullptr;
    m_surfaceBoundingBoxOriginal = nullptr;
    m_surfaceOpaqueRegionOriginal = nullptr;
    m_surfaceVisibleRegionOriginal = nullptr;
    m_surfaceDrawOriginal = nullptr;
    m_surfaceNeedsLiveBlurOriginal = nullptr;
    m_surfaceNeedsPrecomputeBlurOriginal = nullptr;
    m_borderDrawOriginal = nullptr;
    m_shadowDrawOriginal = nullptr;
    m_calculateUVForSurfaceOriginal = nullptr;
    m_renderLayerOriginal = nullptr;
    if (!m_changeWorkspaceDispatcherWrapped)
        m_changeWorkspaceOriginal = nullptr;
    if (!m_focusWorkspaceOnCurrentMonitorDispatcherWrapped)
        m_focusWorkspaceOnCurrentMonitorOriginal = nullptr;
    if (!m_layoutMessageDispatcherWrapped)
        m_layoutMessageOriginal = nullptr;
    if (!m_moveFocusDispatcherWrapped)
        m_moveFocusOriginal = nullptr;
    m_workspaceSwipeBeginOriginal = nullptr;
    m_workspaceSwipeUpdateOriginal = nullptr;
    m_workspaceSwipeEndOriginal = nullptr;
    m_unifiedWorkspaceSwipeBeginOriginal = nullptr;
    m_unifiedWorkspaceSwipeUpdateOriginal = nullptr;
    m_unifiedWorkspaceSwipeEndOriginal = nullptr;
    m_scrollMoveGestureBeginOriginal = nullptr;
    m_scrollMoveGestureUpdateOriginal = nullptr;
    m_scrollMoveGestureEndOriginal = nullptr;
    m_moveToWorkspaceActionOriginal = nullptr;
    g_layoutMessageActionOriginal = nullptr;
    g_moveFocusActionOriginal = nullptr;
    g_moveInDirectionActionOriginal = nullptr;
    g_swapInDirectionActionOriginal = nullptr;
    g_resizeActionOriginal = nullptr;

    activateOptionalHook(m_workspaceSwipeBeginFunctionHook, m_workspaceSwipeBeginOriginal, "workspace swipe begin");
    activateOptionalHook(m_workspaceSwipeUpdateFunctionHook, m_workspaceSwipeUpdateOriginal, "workspace swipe update");
    activateOptionalHook(m_workspaceSwipeEndFunctionHook, m_workspaceSwipeEndOriginal, "workspace swipe end");
    activateOptionalHook(m_unifiedWorkspaceSwipeBeginFunctionHook, m_unifiedWorkspaceSwipeBeginOriginal, "unified workspace swipe begin");
    activateOptionalHook(m_unifiedWorkspaceSwipeUpdateFunctionHook, m_unifiedWorkspaceSwipeUpdateOriginal, "unified workspace swipe update");
    activateOptionalHook(m_unifiedWorkspaceSwipeEndFunctionHook, m_unifiedWorkspaceSwipeEndOriginal, "unified workspace swipe end");
    activateOptionalHook(m_scrollMoveGestureBeginFunctionHook, m_scrollMoveGestureBeginOriginal, "scroll move gesture begin");
    activateOptionalHook(m_scrollMoveGestureUpdateFunctionHook, m_scrollMoveGestureUpdateOriginal, "scroll move gesture update");
    activateOptionalHook(m_scrollMoveGestureEndFunctionHook, m_scrollMoveGestureEndOriginal, "scroll move gesture end");
    activateOptionalHook(m_moveToWorkspaceActionFunctionHook, m_moveToWorkspaceActionOriginal, "move to workspace action");
    activateOptionalHook(g_layoutMessageActionHook, g_layoutMessageActionOriginal, "layout message action");
    activateOptionalHook(g_moveFocusActionHook, g_moveFocusActionOriginal, "move focus action");
    activateOptionalHook(g_moveInDirectionActionHook, g_moveInDirectionActionOriginal, "move window action");
    activateOptionalHook(g_swapInDirectionActionHook, g_swapInDirectionActionOriginal, "swap window action");
    activateOptionalHook(g_resizeActionHook, g_resizeActionOriginal, "resize window action");
    return true;
}

bool OverviewController::activateHooks() {
    if (m_hooksActive)
        return true;

    if (!m_shouldRenderWindowHook || !m_surfaceTexBoxHook || !m_surfaceBoundingBoxHook || !m_surfaceOpaqueRegionHook || !m_surfaceVisibleRegionHook || !m_surfaceDrawHook ||
        !m_surfaceNeedsLiveBlurHook || !m_surfaceNeedsPrecomputeBlurHook || !m_borderDrawHook || !m_shadowDrawHook || !m_calculateUVForSurfaceHook)
        return false;

    const bool hooked = m_shouldRenderWindowHook->hook() && m_surfaceTexBoxHook->hook() && m_surfaceBoundingBoxHook->hook() && m_surfaceOpaqueRegionHook->hook() &&
        m_surfaceVisibleRegionHook->hook() && m_surfaceDrawHook->hook() && m_surfaceNeedsLiveBlurHook->hook() && m_surfaceNeedsPrecomputeBlurHook->hook() &&
        m_borderDrawHook->hook() && m_shadowDrawHook->hook() && m_calculateUVForSurfaceHook->hook();
    if (!hooked) {
        notify("[hymission] surface pass hook attach failed", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        if (m_shouldRenderWindowHook)
            m_shouldRenderWindowHook->unhook();
        if (m_surfaceTexBoxHook)
            m_surfaceTexBoxHook->unhook();
        if (m_surfaceBoundingBoxHook)
            m_surfaceBoundingBoxHook->unhook();
        if (m_surfaceOpaqueRegionHook)
            m_surfaceOpaqueRegionHook->unhook();
        if (m_surfaceVisibleRegionHook)
            m_surfaceVisibleRegionHook->unhook();
        if (m_surfaceDrawHook)
            m_surfaceDrawHook->unhook();
        if (m_surfaceNeedsLiveBlurHook)
            m_surfaceNeedsLiveBlurHook->unhook();
        if (m_surfaceNeedsPrecomputeBlurHook)
            m_surfaceNeedsPrecomputeBlurHook->unhook();
        if (m_borderDrawHook)
            m_borderDrawHook->unhook();
        if (m_shadowDrawHook)
            m_shadowDrawHook->unhook();
        if (m_calculateUVForSurfaceHook)
            m_calculateUVForSurfaceHook->unhook();
        return false;
    }

    m_shouldRenderWindowOriginal = reinterpret_cast<ShouldRenderWindowFn>(m_shouldRenderWindowHook->m_original);
    m_surfaceTexBoxOriginal = reinterpret_cast<SurfaceGetTexBoxFn>(m_surfaceTexBoxHook->m_original);
    m_surfaceBoundingBoxOriginal = reinterpret_cast<SurfaceBoundingBoxFn>(m_surfaceBoundingBoxHook->m_original);
    m_surfaceOpaqueRegionOriginal = reinterpret_cast<SurfaceOpaqueRegionFn>(m_surfaceOpaqueRegionHook->m_original);
    m_surfaceVisibleRegionOriginal = reinterpret_cast<SurfaceVisibleRegionFn>(m_surfaceVisibleRegionHook->m_original);
    m_surfaceDrawOriginal = reinterpret_cast<SurfaceDrawFn>(m_surfaceDrawHook->m_original);
    m_surfaceNeedsLiveBlurOriginal = reinterpret_cast<SurfaceBlurNeedsFn>(m_surfaceNeedsLiveBlurHook->m_original);
    m_surfaceNeedsPrecomputeBlurOriginal = reinterpret_cast<SurfaceBlurNeedsFn>(m_surfaceNeedsPrecomputeBlurHook->m_original);
    m_borderDrawOriginal = reinterpret_cast<BorderDrawFn>(m_borderDrawHook->m_original);
    m_shadowDrawOriginal = reinterpret_cast<BorderDrawFn>(m_shadowDrawHook->m_original);
    m_calculateUVForSurfaceOriginal = reinterpret_cast<CalculateUVForSurfaceFn>(m_calculateUVForSurfaceHook->m_original);
    if (m_renderLayerHook) {
        if (m_renderLayerHook->hook()) {
            m_renderLayerOriginal = reinterpret_cast<RenderLayerFn>(m_renderLayerHook->m_original);
        } else {
            if (debugLogsEnabled())
                debugLog("[hymission] optional hook activation failed: renderLayer");
            HyprlandAPI::removeFunctionHook(m_handle, m_renderLayerHook);
            m_renderLayerHook = nullptr;
            m_renderLayerOriginal = nullptr;
        }
    }
    m_hooksActive = true;
    return true;
}

void OverviewController::deactivateHooks() {
    if (!m_hooksActive)
        return;

    if (m_shouldRenderWindowHook)
        m_shouldRenderWindowHook->unhook();
    if (m_renderLayerHook)
        m_renderLayerHook->unhook();
    if (m_surfaceTexBoxHook)
        m_surfaceTexBoxHook->unhook();
    if (m_surfaceBoundingBoxHook)
        m_surfaceBoundingBoxHook->unhook();
    if (m_surfaceOpaqueRegionHook)
        m_surfaceOpaqueRegionHook->unhook();
    if (m_surfaceVisibleRegionHook)
        m_surfaceVisibleRegionHook->unhook();
    if (m_surfaceDrawHook)
        m_surfaceDrawHook->unhook();
    if (m_surfaceNeedsLiveBlurHook)
        m_surfaceNeedsLiveBlurHook->unhook();
    if (m_surfaceNeedsPrecomputeBlurHook)
        m_surfaceNeedsPrecomputeBlurHook->unhook();
    if (m_borderDrawHook)
        m_borderDrawHook->unhook();
    if (m_shadowDrawHook)
        m_shadowDrawHook->unhook();
    if (m_calculateUVForSurfaceHook)
        m_calculateUVForSurfaceHook->unhook();
    m_shouldRenderWindowOriginal = nullptr;
    m_surfaceTexBoxOriginal = nullptr;
    m_surfaceBoundingBoxOriginal = nullptr;
    m_surfaceOpaqueRegionOriginal = nullptr;
    m_surfaceVisibleRegionOriginal = nullptr;
    m_surfaceDrawOriginal = nullptr;
    m_surfaceNeedsLiveBlurOriginal = nullptr;
    m_surfaceNeedsPrecomputeBlurOriginal = nullptr;
    m_borderDrawOriginal = nullptr;
    m_shadowDrawOriginal = nullptr;
    m_calculateUVForSurfaceOriginal = nullptr;
    m_renderLayerOriginal = nullptr;
    m_surfaceRenderDataTransformDepth = 0;
    m_hooksActive = false;
    g_pHyprRenderer->m_directScanoutBlocked = false;
}

bool OverviewController::hookFunction(const std::string& symbolName, const std::string& demangledNeedle, CFunctionHook*& hook, void* destination) {
    return hookFunction(symbolName, std::vector<std::string>{demangledNeedle}, hook, destination);
}

bool OverviewController::hookFunction(const std::string& symbolName, const std::vector<std::string>& demangledNeedles, CFunctionHook*& hook, void* destination) {
    void* source = findFunction(symbolName, demangledNeedles);
    if (!source)
        return false;

    hook = HyprlandAPI::createFunctionHook(m_handle, source, destination);
    return hook != nullptr;
}

bool OverviewController::wrapDispatcher(const std::string& name, DispatcherHandler& original, DispatcherHandler replacement) {
    if (!g_pKeybindManager)
        return false;

    const auto it = g_pKeybindManager->m_dispatchers.find(name);
    if (it == g_pKeybindManager->m_dispatchers.end())
        return false;

    original = it->second;
    it->second = std::move(replacement);
    return true;
}


void OverviewController::restoreWrappedDispatchers() {
    if (!g_pKeybindManager)
        return;

    const auto restore = [&](const char* name, bool& wrapped, DispatcherHandler& original) {
        if (!wrapped)
            return;

        if (original)
            g_pKeybindManager->m_dispatchers[name] = std::move(original);

        original = nullptr;
        wrapped = false;
    };

    for (auto& [name, original] : g_openingDispatcherGateOriginals) {
        if (original)
            g_pKeybindManager->m_dispatchers[name] = std::move(original);
    }
    g_openingDispatcherGateOriginals.clear();

    restore("fullscreen", m_fullscreenActiveDispatcherWrapped, m_fullscreenActiveOriginal);
    restore("fullscreenstate", m_fullscreenStateDispatcherWrapped, m_fullscreenStateActiveOriginal);
    restore("workspace", m_changeWorkspaceDispatcherWrapped, m_changeWorkspaceOriginal);
    restore("focusworkspaceoncurrentmonitor", m_focusWorkspaceOnCurrentMonitorDispatcherWrapped, m_focusWorkspaceOnCurrentMonitorOriginal);
    restore(m_layoutMessageDispatcherName.c_str(), m_layoutMessageDispatcherWrapped, m_layoutMessageOriginal);
    restore("movefocus", m_moveFocusDispatcherWrapped, m_moveFocusOriginal);
    for (auto& [name, original] : m_overviewEditingDispatchersOriginal) {
        if (original)
            g_pKeybindManager->m_dispatchers[name] = std::move(original);
    }
    m_overviewEditingDispatchersOriginal.clear();
}

void* OverviewController::findFunction(const std::string& symbolName, const std::string& demangledNeedle) const {
    return findFunction(symbolName, std::vector<std::string>{demangledNeedle});
}

void* OverviewController::findFunction(const std::string& symbolName, const std::vector<std::string>& demangledNeedles) const {
    const auto matches = HyprlandAPI::findFunctionsByName(m_handle, symbolName);
    for (const auto& demangledNeedle : demangledNeedles) {
        const auto it = std::find_if(matches.begin(), matches.end(), [&](const SFunctionMatch& match) {
            return match.demangled.find(demangledNeedle) != std::string::npos;
        });

        if (it != matches.end())
            return it->address;
    }

    return nullptr;
}

bool OverviewController::isAnimating() const {
    return m_gestureSession.active || m_workspaceTransition.active || m_state.phase == Phase::Opening || m_state.phase == Phase::Closing;
}

bool OverviewController::isVisible() const {
    return m_state.phase != Phase::Inactive;
}

bool OverviewController::shouldHandleInput() const {
    if (m_gestureSession.active || m_workspaceTransition.active || m_workspaceSwipeGesture.active)
        return false;

    return isVisible() && (m_state.phase == Phase::Opening || m_state.phase == Phase::Active);
}

std::vector<PHLMONITOR> OverviewController::ownedMonitors() const {
    std::vector<PHLMONITOR> monitors;
    const auto append = [&](const PHLMONITOR& monitor) {
        if (!monitor || containsHandle(monitors, monitor))
            return;
        monitors.push_back(monitor);
    };

    append(m_state.ownerMonitor);

    for (const auto& monitor : m_state.participatingMonitors)
        append(monitor);

    if (m_workspaceTransition.active) {
        append(m_workspaceTransition.sourceState.ownerMonitor);
        append(m_workspaceTransition.targetState.ownerMonitor);
        for (const auto& monitor : m_workspaceTransition.sourceState.participatingMonitors)
            append(monitor);
        for (const auto& monitor : m_workspaceTransition.targetState.participatingMonitors)
            append(monitor);
    }

    return monitors;
}

bool OverviewController::shouldSyncRealFocusDuringOverview() const {
    return shouldSyncOverviewLiveFocus(shouldHandleInput(), focusFollowsMouseEnabled(), m_inputFollowMouseBackup);
}



bool OverviewController::insideRenderLifecycle() const {
    return m_surfaceRenderDataTransformDepth > 0 || m_stripSnapshotRenderDepth > 0 || (g_pHyprOpenGL && g_pHyprRenderer->m_renderData.pMonitor);
}

bool OverviewController::ownsMonitor(const PHLMONITOR& monitor) const {
    if (!monitor)
        return false;

    const auto monitors = ownedMonitors();
    return containsHandle(monitors, monitor);
}

bool OverviewController::ownsWorkspace(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return false;

    if (containsHandle(m_state.managedWorkspaces, workspace))
        return true;

    return m_workspaceTransition.active &&
        (containsHandle(m_workspaceTransition.sourceState.managedWorkspaces, workspace) || containsHandle(m_workspaceTransition.targetState.managedWorkspaces, workspace));
}

bool OverviewController::hasManagedWindow(const PHLWINDOW& window) const {
    return managedWindowFor(window) != nullptr;
}



bool OverviewController::windowHasUsableStateGeometry(const PHLWINDOW& window) const {
    if (!window)
        return false;

    if (hasUsableWindowSize(window->m_realSize->value()))
        return true;

    return hasUsableWindowSize(window->m_realSize->goal());
}

bool OverviewController::windowMatchesOverviewScope(const PHLWINDOW& window, const State& state, bool requireUsableGeometry) const {
    if (!window)
        return false;

    if (!window->m_isMapped || window->m_fadingOut || window->isHidden())
        return false;

    if (requireUsableGeometry && !windowHasUsableStateGeometry(window))
        return false;

    if (window->m_pinned) {
        if (window->onSpecialWorkspace())
            return false;
        return std::ranges::any_of(state.participatingMonitors, [&](const PHLMONITOR& monitor) { return monitor && window->visibleOnMonitor(monitor); });
    }

    if (!window->m_workspace)
        return false;

    if (window->m_workspace->m_isSpecialWorkspace)
        return state.collectionPolicy.includeSpecial && containsHandle(state.managedWorkspaces, window->m_workspace) && window->m_workspace->isVisible();

    return containsHandle(state.managedWorkspaces, window->m_workspace);
}

bool OverviewController::shouldAutoCloseFor(const PHLWINDOW& window) const {
    if (!window || !m_state.ownerMonitor)
        return false;

    if (hasManagedWindow(window))
        return true;

    return windowMatchesOverviewScope(window, m_state, false);
}

bool OverviewController::shouldManageWindow(const PHLWINDOW& window, const State& state) const {
    return windowMatchesOverviewScope(window, state, true);
}

std::string OverviewController::collectionSummary(const PHLMONITOR& monitor) const {
    std::string error;
    const auto requestedScope = parseScopeOverride({}, error);
    const auto policy = loadCollectionPolicy(requestedScope.value_or(ScopeOverride::Default));
    std::size_t total = 0;
    std::size_t accepted = 0;
    std::size_t noWorkspace = 0;
    std::size_t specialWorkspace = 0;
    std::size_t unmapped = 0;
    std::size_t hidden = 0;
    std::size_t fading = 0;
    std::size_t workspaceMismatch = 0;
    std::size_t invalidSize = 0;

    for (const auto& window : g_pCompositor->m_windows) {
        if (!window)
            continue;

        ++total;

        if (!window->m_workspace && !window->m_pinned) {
            ++noWorkspace;
            continue;
        }

        if (window->onSpecialWorkspace() && !policy.includeSpecial) {
            ++specialWorkspace;
            continue;
        }

        if (!window->m_isMapped) {
            ++unmapped;
            continue;
        }

        if (window->isHidden()) {
            ++hidden;
            continue;
        }

        if (window->m_fadingOut) {
            ++fading;
            continue;
        }

        if (!monitor) {
            ++workspaceMismatch;
            continue;
        }

        std::vector<PHLMONITOR> participatingMonitors;
        if (policy.onlyActiveMonitor) {
            participatingMonitors.push_back(monitor);
        } else {
            for (const auto& candidate : g_pCompositor->m_monitors) {
                if (candidate)
                    participatingMonitors.push_back(candidate);
            }
        }

        if (window->m_pinned) {
            const bool visibleOnAnyMonitor =
                std::ranges::any_of(participatingMonitors, [&](const PHLMONITOR& candidate) { return candidate && window->visibleOnMonitor(candidate); });
            if (window->onSpecialWorkspace() || !visibleOnAnyMonitor) {
                ++workspaceMismatch;
                continue;
            }
        } else {
            bool workspaceAccepted = false;
            if (window->m_workspace) {
                if (window->m_workspace->m_isSpecialWorkspace) {
                    workspaceAccepted = policy.includeSpecial && window->m_workspace->isVisible();
                } else if (policy.onlyActiveWorkspace) {
                    workspaceAccepted = std::ranges::any_of(participatingMonitors, [&](const PHLMONITOR& candidate) {
                        return candidate && candidate->m_activeWorkspace && window->m_workspace == candidate->m_activeWorkspace;
                    });
                } else {
                    const auto workspaceMonitor = window->m_workspace->m_monitor.lock();
                    workspaceAccepted = workspaceMonitor && containsHandle(participatingMonitors, workspaceMonitor);
                }
            }

            if (!workspaceAccepted) {
                ++workspaceMismatch;
                continue;
            }
        }

        if (!windowHasUsableStateGeometry(window)) {
            ++invalidSize;
            continue;
        }

        ++accepted;
    }

    std::ostringstream summary;
    summary << "[hymission] collect scope=";
    switch (policy.requestedScope) {
        case ScopeOverride::Default:
            summary << "default";
            break;
        case ScopeOverride::OnlyCurrentWorkspace:
            summary << "onlycurrentworkspace";
            break;
        case ScopeOverride::ForceAll:
            summary << "forceall";
            break;
    }
    summary << " mon=" << (monitor ? monitor->m_name : "?") << " total=" << total << " ok=" << accepted
            << " mismatch=" << workspaceMismatch << " hidden=" << hidden << " unmapped=" << unmapped << " special=" << specialWorkspace;

    if (fading || invalidSize || noWorkspace)
        summary << " fade=" << fading << " size=" << invalidSize << " nows=" << noWorkspace;

    return summary.str();
}

OverviewController::PreviewRectSnapshot OverviewController::captureCurrentPreviewRects() const {
    PreviewRectSnapshot rects;
    rects.reserve(m_state.windows.size());

    for (const auto& window : m_state.windows)
        rects.emplace_back(window.window, currentPreviewRect(window));

    return rects;
}

OverviewController::PreviewRectSnapshot OverviewController::commitActiveNiriRelayoutForRetarget() {
    if (m_state.relayoutActive && m_relayoutProgressAnimation)
        m_state.relayoutProgress = clampUnit(m_relayoutProgressAnimation->value());

    auto previewRects = captureCurrentPreviewRects();
    if (!m_state.relayoutActive)
        return previewRects;

    std::size_t committedPlaceholders = 0;
    for (auto& placeholder : m_state.emptyWorkspacePlaceholders) {
        if (!placeholder.monitor || placeholder.workspaceId == WORKSPACE_INVALID)
            continue;

        const Rect preview = currentEmptyWorkspacePlaceholderRect(placeholder);
        placeholder.targetGlobal = preview;
        placeholder.relayoutFromGlobal = preview;
        ++committedPlaceholders;
    }

    std::size_t committedStripEntries = 0;
    for (auto& entry : m_state.stripEntries) {
        if (!entry.monitor)
            continue;

        const Rect preview = currentWorkspaceStripRect(entry);
        entry.rect = preview;
        entry.relayoutFromRect = preview;
        entry.hasRelayoutFromRect = false;
        ++committedStripEntries;
    }

    m_state.slots.clear();
    m_state.slots.reserve(m_state.windows.size());
    for (auto& managed : m_state.windows) {
        const auto preview = std::ranges::find_if(previewRects, [&](const auto& candidate) { return candidate.first == managed.window; });
        if (preview != previewRects.end()) {
            managed.targetGlobal = preview->second;
            managed.relayoutFromGlobal = preview->second;
            if (managed.targetMonitor)
                managed.slot.target = rectToMonitorLocal(preview->second, managed.targetMonitor);
        }
        m_state.slots.push_back(managed.slot);
    }

    m_relayoutProgressAnimation.reset();
    m_state.relayoutActive = false;
    m_state.relayoutProgress = 1.0;
    m_state.relayoutStart = {};

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] committed active niri relayout for retarget"
            << " windows=" << previewRects.size()
            << " placeholders=" << committedPlaceholders
            << " stripEntries=" << committedStripEntries;
        debugLog(out.str());

        std::size_t logged = 0;
        for (const auto& entry : m_state.stripEntries) {
            if (!entry.monitor || logged >= 6)
                continue;
            std::ostringstream stripOut;
            stripOut << "[hymission] committed active niri strip entry"
                << " workspaceId=" << entry.workspaceId
                << " active=" << (entry.active ? 1 : 0)
                << " rect=" << rectToString(entry.rect);
            debugLog(stripOut.str());
            ++logged;
        }
    }

    return previewRects;
}

std::vector<Rect> OverviewController::targetRects() const {
    std::vector<Rect> rects;
    rects.reserve(m_state.windows.size());

    for (const auto& window : m_state.windows)
        rects.push_back(currentPreviewRect(window));

    return rects;
}

Rect OverviewController::workspaceStripBandRectForMonitor(const PHLMONITOR& monitor, const State& state) const {
    if (!monitor || !workspaceStripEnabled(state))
        return {};

    const auto reservation =
        reserveWorkspaceStripBand(makeRect(monitor->m_position.x, monitor->m_position.y, monitor->m_size.x, monitor->m_size.y),
                                  parseWorkspaceStripAnchor(workspaceStripAnchor()), workspaceStripThickness(monitor), workspaceStripGap());
    return makeRect(reservation.band.x, reservation.band.y, reservation.band.width, reservation.band.height);
}

Rect OverviewController::overviewContentRectForMonitor(const PHLMONITOR& monitor, const State& state) const {
    if (!monitor)
        return {};

    if (!workspaceStripEnabled(state))
        return makeRect(0.0, 0.0, monitor->m_size.x, monitor->m_size.y);

    const auto reservation = reserveWorkspaceStripBand(makeRect(0.0, 0.0, monitor->m_size.x, monitor->m_size.y),
                                                       parseWorkspaceStripAnchor(workspaceStripAnchor()), workspaceStripThickness(monitor), workspaceStripGap());
    return makeRect(reservation.content.x, reservation.content.y, reservation.content.width, reservation.content.height);
}

Vector2D OverviewController::stripThumbnailPreviewOffset(const PHLMONITOR& monitor, const State& state) const {
    if (!monitor || !workspaceStripEnabled(state))
        return {};

    const Rect previewArea = overviewContentRectForMonitor(monitor, state);
    const Rect fullArea = makeRect(0.0, 0.0, monitor->m_size.x, monitor->m_size.y);
    return Vector2D{
        (previewArea.x + previewArea.width * 0.5) - (fullArea.x + fullArea.width * 0.5),
        (previewArea.y + previewArea.height * 0.5) - (fullArea.y + fullArea.height * 0.5),
    };
}

std::vector<Rect> OverviewController::stripRects() const {
    std::vector<Rect> rects;
    rects.reserve(m_state.stripEntries.size());

    for (const auto& entry : m_state.stripEntries)
        rects.push_back(animatedWorkspaceStripRect(currentWorkspaceStripRect(entry), entry.monitor));

    return rects;
}

const OverviewController::ManagedWindow* OverviewController::managedWindowFor(const State& state, const PHLWINDOW& window, bool includeTransient) const {
    const auto it = std::find_if(state.windows.begin(), state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == window; });
    if (it != state.windows.end())
        return &*it;

    if (!includeTransient)
        return nullptr;

    const auto transientIt =
        std::find_if(state.transientClosingWindows.begin(), state.transientClosingWindows.end(), [&](const ManagedWindow& managed) { return managed.window == window; });
    return transientIt == state.transientClosingWindows.end() ? nullptr : &*transientIt;
}

const OverviewController::ManagedWindow* OverviewController::managedWindowForWorkspaceTransition(const PHLWINDOW& window) const {
    if (!m_workspaceTransition.active)
        return nullptr;

    const auto* sourceManaged = managedWindowFor(m_workspaceTransition.sourceState, window, true);
    const auto* targetManaged = managedWindowFor(m_workspaceTransition.targetState, window, true);
    if (sourceManaged && !targetManaged)
        return sourceManaged;

    return targetManaged;
}

const OverviewController::ManagedWindow* OverviewController::managedWindowFor(const PHLWINDOW& window) const {
    if (m_stripPreviewContext.active)
        return managedWindowFor(m_stripPreviewContext.state, window, true);

    if (const auto* managed = managedWindowForWorkspaceTransition(window); managed)
        return managed;

    if (const auto* managed = managedWindowFor(m_state, window, true); managed)
        return managed;

    return nullptr;
}

const OverviewController::ManagedWindow* OverviewController::renderableManagedWindowFor(const PHLWINDOW& window, const PHLMONITOR& monitor) const {
    if (!window || !monitor)
        return nullptr;

    const auto matchesTargetMonitor = [&](const ManagedWindow* managed) {
        return managed && managed->targetMonitor && managed->targetMonitor == monitor;
    };

    if (m_stripPreviewContext.active) {
        const auto* managed = managedWindowFor(m_stripPreviewContext.state, window, true);
        if (!matchesTargetMonitor(managed) || !windowMatchesOverviewScope(window, m_stripPreviewContext.state, false))
            return nullptr;
        return managed;
    }

    if (m_workspaceTransition.active) {
        const auto* managed = managedWindowForWorkspaceTransition(window);
        return matchesTargetMonitor(managed) ? managed : nullptr;
    }

    const auto* managed = managedWindowFor(m_state, window, true);
    if (!matchesTargetMonitor(managed))
        return nullptr;

    if ((m_state.phase == Phase::Opening || m_state.phase == Phase::Active) && !windowMatchesOverviewScope(window, m_state, false))
        return nullptr;

    return managed;
}

PHLWINDOW OverviewController::selectedWindow() const {
    if (!m_state.selectedIndex || *m_state.selectedIndex >= m_state.windows.size())
        return {};

    return m_state.windows[*m_state.selectedIndex].window;
}

float OverviewController::managedPreviewAlphaFor(const PHLWINDOW& window, float fallback) const {
    const auto* managed = managedWindowFor(window);
    if (m_stripPreviewContext.active && managed)
        return std::clamp(managed->previewAlpha > 0.0F ? managed->previewAlpha : 1.0F, 0.0F, 1.0F);

    if (managed && m_workspaceTransition.active && m_state.collectionPolicy.onlyActiveWorkspace &&
        (niriModeAppliesToState(m_workspaceTransition.sourceState) || niriModeAppliesToState(m_workspaceTransition.targetState)))
        return std::clamp(window->alphaTotal(), 0.0F, 1.0F);

    return directNiriDraggedPreviewAlpha(window, managed ? managed->previewAlpha : fallback);
}

PHLMONITOR OverviewController::focusMonitorForWindow(const PHLWINDOW& window) const {
    if (!window)
        return {};

    if (const auto monitor = window->m_monitor.lock(); monitor)
        return monitor;

    if (window->m_workspace) {
        if (const auto workspaceMonitor = window->m_workspace->m_monitor.lock(); workspaceMonitor)
            return workspaceMonitor;
    }

    return g_pCompositor->getMonitorFromID(window->monitorID());
}


PHLMONITOR OverviewController::preferredMonitorForWindow(const PHLWINDOW& window, const State& state) const {
    if (!window)
        return {};

    if (window->m_pinned) {
        for (const auto& monitor : state.participatingMonitors) {
            if (monitor && window->visibleOnMonitor(monitor))
                return monitor;
        }
    }

    if (const auto monitor = window->m_monitor.lock(); monitor && containsHandle(state.participatingMonitors, monitor))
        return monitor;

    if (window->m_workspace) {
        if (const auto workspaceMonitor = window->m_workspace->m_monitor.lock(); workspaceMonitor && containsHandle(state.participatingMonitors, workspaceMonitor))
            return workspaceMonitor;
    }

    if (const auto monitor = g_pCompositor->getMonitorFromID(window->monitorID()); monitor && containsHandle(state.participatingMonitors, monitor))
        return monitor;

    return state.ownerMonitor;
}

PHLMONITOR OverviewController::previewMonitorForWindow(const PHLWINDOW& window) const {
    if (m_stripPreviewContext.active) {
        const auto* managed = managedWindowFor(m_stripPreviewContext.state, window, true);
        return managed && managed->targetMonitor ? managed->targetMonitor : PHLMONITOR{};
    }

    if (const auto* managed = managedWindowForWorkspaceTransition(window); managed && managed->targetMonitor)
        return managed->targetMonitor;

    const auto* managed = managedWindowFor(window);
    if (!managed || !managed->targetMonitor)
        return {};

    return managed->targetMonitor;
}

PHLWINDOW OverviewController::hoveredWindow() const {
    if (!m_state.hoveredIndex || *m_state.hoveredIndex >= m_state.windows.size())
        return {};

    return m_state.windows[*m_state.hoveredIndex].window;
}











const OverviewController::FullscreenWorkspaceBackup* OverviewController::fullscreenBackupForWorkspace(const PHLWORKSPACE& workspace) const {
    const auto it = std::find_if(m_state.fullscreenBackups.begin(), m_state.fullscreenBackups.end(),
                                 [&](const FullscreenWorkspaceBackup& backup) { return backup.workspace == workspace; });
    return it == m_state.fullscreenBackups.end() ? nullptr : &*it;
}

const OverviewController::FullscreenWorkspaceBackup* OverviewController::fullscreenBackupForWindow(const PHLWINDOW& window) const {
    return window ? fullscreenBackupForWorkspace(window->m_workspace) : nullptr;
}

Rect OverviewController::liveGlobalRectForWindow(const PHLWINDOW& window) const {
    if (!window)
        return {};

    return sceneGlobalRectForWindow(window);
}

Rect OverviewController::goalGlobalRectForWindow(const PHLWINDOW& window) const {
    if (!window)
        return {};

    return sceneGlobalRectForWindow(window, true);
}

bool OverviewController::shouldUseGoalGeometryForStateSnapshot(const PHLWINDOW& window) const {
    if (!window)
        return false;

    if (isVisible() && m_animationsEnabledOverridden && !window->m_isFloating && window->m_workspace && window->m_workspace->isVisible()) {
        const Rect live = stateSnapshotGlobalRectForWindow(window);
        const Rect goal = stateSnapshotGlobalRectForWindow(window, true);
        if (!rectApproxEqual(live, goal, 0.5))
            return true;
    }

    if (window->m_workspace && !window->m_workspace->isVisible())
        return true;

    if (hasUsableWindowSize(window->m_realSize->value()))
        return false;

    return hasUsableWindowSize(window->m_realSize->goal());
}


void OverviewController::commitNonScrollingWorkspaceLayout(const PHLWORKSPACE& workspace) const {
    if (!workspace || !workspace->m_space || isScrollingWorkspace(workspace))
        return;

    workspace->m_space->recalculate();
    if (const auto monitor = workspace->m_monitor.lock(); monitor && g_layoutManager)
        g_layoutManager->recalculateMonitor(monitor);
    workspace->m_space->recalculate();

    for (const auto& window : g_pCompositor->m_windows) {
        if (!window || !window->m_isMapped || window->m_fadingOut || window->m_workspace != workspace)
            continue;

        const auto target = window->layoutTarget();
        if (!target || target->floating())
            continue;

        target->recalc();
        target->warpPositionSize();
        target->damageEntire();
        window->updateWindowDecos();
    }
}







std::optional<OverviewController::WindowTransform> OverviewController::windowTransformFor(const PHLWINDOW& window, const PHLMONITOR& monitor) const {
    if (!window || !monitor || !isVisible() || !ownsMonitor(monitor))
        return std::nullopt;

    const auto* managed = renderableManagedWindowFor(window, monitor);
    if (!managed)
        return std::nullopt;

    Rect current;
    if (m_stripPreviewContext.active) {
        // Strip thumbnails should snapshot the fully-open mini preview, not the
        // main overview's current animation frame. In strip-only openings from
        // an empty workspace, using the opening snapshot geometry leaves hidden
        // workspaces at their off-screen render offsets and the thumbnail falls
        // back to wallpaper-only until another refresh happens.
        current = managed->targetGlobal;
    } else {
        current = workspaceTransitionRectForWindow(window).value_or(currentPreviewRect(*managed));
    }
    if (m_niriDragSession.active && m_niriDragSession.window.lock() == window)
        current = directNiriDraggedPreviewRect();
    const Rect   actual = surfaceRenderGlobalRectForWindow(window);
    const double actualWidth = std::max(1.0, actual.width);
    const double actualHeight = std::max(1.0, actual.height);

    const double uniformScale = std::max(0.0, std::min(current.width / actualWidth, current.height / actualHeight));
    const Rect   fitted = makeRect(current.centerX() - actualWidth * uniformScale * 0.5, current.centerY() - actualHeight * uniformScale * 0.5,
                                   actualWidth * uniformScale, actualHeight * uniformScale);
    if (debugLogsEnabled() && window->m_workspace && consumeTwoColumnSwapPreviewTrace(window->m_workspace)) {
        std::ostringstream out;
        out << "[hymission] swapcol window-transform"
            << " window=" << debugWindowLabel(window)
            << " current=" << rectToString(current)
            << " actual=" << rectToString(actual)
            << " fitted=" << rectToString(fitted)
            << " targetGlobal=" << rectToString(managed->targetGlobal)
            << " relayoutFrom=" << rectToString(managed->relayoutFromGlobal)
            << " relayoutActive=" << (m_state.relayoutActive ? 1 : 0)
            << " stripPreview=" << (m_stripPreviewContext.active ? 1 : 0);
        debugLog(out.str());
    }
    return WindowTransform{
        .actualGlobal = actual,
        .targetGlobal = fitted,
        .scaleX = uniformScale,
        .scaleY = uniformScale,
    };
}

bool OverviewController::transformSurfaceRenderDataForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor, CSurfacePassElement::SRenderData& renderData) const {
    const auto transform = windowTransformFor(window, monitor);
    if (!transform)
        return false;

    const Vector2D originalPos = renderData.pos;
    const Vector2D originalLocalPos = renderData.localPos;
    const Vector2D effectiveOrigin = originalPos + originalLocalPos;
    const Vector2D transformedLocalPos = Vector2D(originalLocalPos.x * transform->scaleX, originalLocalPos.y * transform->scaleY);
    const Vector2D transformedEffectiveOrigin = Vector2D(transform->targetGlobal.x + (effectiveOrigin.x - transform->actualGlobal.x) * transform->scaleX,
                                                         transform->targetGlobal.y + (effectiveOrigin.y - transform->actualGlobal.y) * transform->scaleY);

    renderData.pos = transformedEffectiveOrigin - transformedLocalPos;
    renderData.localPos = transformedLocalPos;
    renderData.w = std::max(1.0, renderData.w * transform->scaleX);
    renderData.h = std::max(1.0, renderData.h * transform->scaleY);
    if (!renderData.dontRound && renderData.rounding > 0) {
        double scale = std::max(0.0, std::min(std::abs(transform->scaleX), std::abs(transform->scaleY)));

        // Strip snapshots render the workspace into a smaller framebuffer first.
        // Match window rounding to that extra downscale so mini previews do not
        // keep the full-size corner radius.
        if (m_stripPreviewContext.active) {
            const auto   fbSize = m_stripPreviewContext.framebufferSize;
            const double monitorPixelWidth = std::max(1.0, static_cast<double>(monitor->m_size.x) * renderScaleForMonitor(monitor));
            const double monitorPixelHeight = std::max(1.0, static_cast<double>(monitor->m_size.y) * renderScaleForMonitor(monitor));
            const double fbScale =
                std::clamp(std::min(fbSize.x / monitorPixelWidth, fbSize.y / monitorPixelHeight), 0.0, 1.0);
            scale *= fbScale;
        }

        renderData.rounding = std::max(0, static_cast<int>(std::lround(static_cast<double>(renderData.rounding) * scale)));
        renderData.dontRound = renderData.rounding <= 0;
    }

    // Keep overview previews independent from the normal-layout monitor clip,
    // but still clip every transformed surface to Hymission's preview rect.
    // When a newly-mapped or recently-resized client is between configure/commit
    // sizes, Hyprland can render a surface whose buffer/viewport is temporarily
    // larger than the overview preview.  Clearing the clip lets that transient
    // buffer leak across adjacent workspace lanes for a few seconds.  Clipping to
    // the overview target preserves off-screen workspace previews without letting
    // unstable client buffers bleed outside their card.
    renderData.clipBox = toBox(transform->targetGlobal);

    return true;
}

bool OverviewController::adjustTransformedSurfaceBoxSize(const CSurfacePassElement::SRenderData& renderData, const PHLMONITOR& monitor, CBox& box) const {
    if (renderData.mainSurface)
        return false;

    const auto transform = windowTransformFor(renderData.pWindow, monitor);
    if (!transform)
        return false;

    Vector2D baseSize{box.width, box.height};
    if (renderData.surface) {
        if (renderData.surface->m_current.viewport.hasDestination)
            baseSize = renderData.surface->m_current.viewport.destination;
        else if (renderData.surface->m_current.viewport.hasSource)
            baseSize = renderData.surface->m_current.viewport.source.size();
        else
            baseSize = renderData.surface->m_current.size;
    }

    box.width = std::max(1.0, baseSize.x * transform->scaleX);
    box.height = std::max(1.0, baseSize.y * transform->scaleY);
    return true;
}

double OverviewController::hiddenStripLayerProgress(const PHLLS& layer, const PHLMONITOR& monitor) const {
    if (!shouldHideLayerSurface(layer, monitor))
        return 0.0;

    return clampUnit(visualProgress());
}

void OverviewController::clearHiddenStripLayerProxies() {
    m_hiddenStripLayerProxies.clear();
}

void OverviewController::clearNiriWallpaperSnapshots() {
    m_layerSnapshotCaptureLayer.reset();
    m_niriWallpaperSnapshots.clear();
}

SP<Render::IFramebuffer> OverviewController::captureLayerFramebuffer(const PHLLS& layer) {
    if (!layer || !g_pHyprRenderer)
        return nullptr;

    m_layerSnapshotCaptureLayer = layer;
    g_pHyprRenderer->makeSnapshot(layer);
    m_layerSnapshotCaptureLayer.reset();
    return layer->m_snapshotFB;
}

bool OverviewController::isNiriWallpaperLayer(const PHLLS& layer, const PHLMONITOR& monitor) const {
    if (!layer || !monitor || !niriWallpaperZoomAppliesToMonitor(m_state, monitor))
        return false;

    const auto layerMonitor = layer->m_monitor.lock();
    return layerMonitor == monitor && layer->m_mapped && !layer->m_readyToDelete &&
        layer->m_layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND &&
        shouldHideLayerSurfaceNamespace(layer, niriModeWallpaperZoomLayerNamespaces());
}

bool OverviewController::isNiriWallpaperLayoutLayer(const PHLLS& layer, const PHLMONITOR& monitor) const {
    if (!layer || !monitor || !niriWallpaperZoomAppliesToMonitor(m_state, monitor))
        return false;

    const auto layerMonitor = layer->m_monitor.lock();
    const auto layerResource = layer->m_layerSurface.lock();
    return layerMonitor == monitor && layerResource && layer->m_mapped && !layer->m_readyToDelete &&
        layer->m_layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP && layerResource->m_current.exclusive > 0;
}

bool OverviewController::isRetainedNiriWallpaperLayoutLayer(const PHLLS& layer, const PHLMONITOR& monitor) const {
    if (isNiriWallpaperLayoutLayer(layer, monitor))
        return true;
    if (!layer || !monitor || !niriWallpaperZoomAppliesToMonitor(m_state, monitor) || !layer->m_mapped || layer->m_readyToDelete ||
        layer->m_monitor.lock() != monitor)
        return false;

    const auto* proxy = hiddenStripLayerProxyFor(layer, monitor);
    return proxy && proxy->niriWallpaperLayoutLayer;
}

void OverviewController::syncNiriWallpaperSnapshots() {
    clearNiriWallpaperSnapshots();
    if (!isVisible() || !niriWallpaperZoomAppliesToState(m_state) || !g_pHyprRenderer || !g_pHyprOpenGL)
        return;

    g_pHyprOpenGL->makeEGLCurrent();
    for (const auto& monitor : ownedMonitors()) {
        if (!monitor || !niriWallpaperZoomAppliesToMonitor(m_state, monitor))
            continue;

        PHLLS wallpaperLayer;
        for (const auto& layerRef : monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
            const auto layer = layerRef.lock();
            if (!isNiriWallpaperLayer(layer, monitor))
                continue;

            wallpaperLayer = layer;
            break;
        }

        if (!wallpaperLayer) {
            if (debugLogsEnabled())
                debugLog("[hymission] niri wallpaper snapshot missing layer monitor=" + monitor->m_name);
            continue;
        }

        const auto framebuffer = captureLayerFramebuffer(wallpaperLayer);
        if (!framebuffer || !framebuffer->isAllocated() || !framebuffer->getTexture()) {
            if (debugLogsEnabled())
                debugLog("[hymission] niri wallpaper snapshot capture failed namespace=" + wallpaperLayer->m_namespace + " monitor=" + monitor->m_name);
            continue;
        }

        setFramebufferLinearFiltering(*framebuffer);
        m_niriWallpaperSnapshots.push_back({
            .monitor = monitor,
            .layer = wallpaperLayer,
            .framebuffer = framebuffer,
        });
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] niri wallpaper snapshot captured namespace=" << wallpaperLayer->m_namespace << " monitor=" << monitor->m_name
                << " fb=(" << framebuffer->m_size.x << 'x' << framebuffer->m_size.y << ')';
            debugLog(out.str());
        }
    }
}

SP<Render::ITexture> OverviewController::niriWallpaperTextureForMonitor(const PHLMONITOR& monitor) const {
    const auto it = std::find_if(m_niriWallpaperSnapshots.begin(), m_niriWallpaperSnapshots.end(),
                                 [&](const NiriWallpaperSnapshot& snapshot) {
                                     return snapshot.monitor == monitor && snapshot.framebuffer && snapshot.framebuffer->isAllocated();
                                 });
    return it == m_niriWallpaperSnapshots.end() ? nullptr : it->framebuffer->getTexture();
}

OverviewController::HiddenStripLayerProxy* OverviewController::hiddenStripLayerProxyFor(const PHLLS& layer, const PHLMONITOR& monitor) {
    const auto it = std::find_if(m_hiddenStripLayerProxies.begin(), m_hiddenStripLayerProxies.end(),
                                 [&](const HiddenStripLayerProxy& proxy) { return proxy.layer == layer && proxy.monitor == monitor; });
    return it == m_hiddenStripLayerProxies.end() ? nullptr : &*it;
}

const OverviewController::HiddenStripLayerProxy* OverviewController::hiddenStripLayerProxyFor(const PHLLS& layer, const PHLMONITOR& monitor) const {
    const auto it = std::find_if(m_hiddenStripLayerProxies.begin(), m_hiddenStripLayerProxies.end(),
                                 [&](const HiddenStripLayerProxy& proxy) { return proxy.layer == layer && proxy.monitor == monitor; });
    return it == m_hiddenStripLayerProxies.end() ? nullptr : &*it;
}

bool OverviewController::captureHiddenStripLayerProxy(const PHLLS& layer, const PHLMONITOR& monitor) {
    const bool allowEmptyDirectNiriLayoutLayerCapture = layer && monitor && monitor == m_state.ownerMonitor &&
        m_state.collectionPolicy.onlyActiveWorkspace && m_state.windows.empty() && niriModeAppliesToState(m_state) &&
        centeredEmptyWorkspacePlaceholder(m_state) && isNiriWallpaperLayoutLayer(layer, monitor);

    if (!layer || !monitor || !g_pHyprRenderer || !g_pHyprOpenGL || (!shouldHideLayerSurface(layer, monitor) && !allowEmptyDirectNiriLayoutLayerCapture))
        return false;

    constexpr double kHiddenStripBlurPaddingLogical = 24.0;
    const Rect capturedRectGlobal = makeRect(layer->m_geometry.x, layer->m_geometry.y, layer->m_geometry.w, layer->m_geometry.h);
    const Rect proxyRectGlobal =
        makeRect(capturedRectGlobal.x - kHiddenStripBlurPaddingLogical, capturedRectGlobal.y - kHiddenStripBlurPaddingLogical,
                 capturedRectGlobal.width + kHiddenStripBlurPaddingLogical * 2.0, capturedRectGlobal.height + kHiddenStripBlurPaddingLogical * 2.0);
    if (capturedRectGlobal.width <= 1.0 || capturedRectGlobal.height <= 1.0) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar capture skipped namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                << " captured=" << rectToString(capturedRectGlobal);
            debugLog(out.str());
        }
        return false;
    }

    const int fbWidth = std::max(1, static_cast<int>(std::ceil(proxyRectGlobal.width * renderScaleForMonitor(monitor))));
    const int fbHeight = std::max(1, static_cast<int>(std::ceil(proxyRectGlobal.height * renderScaleForMonitor(monitor))));

    g_pHyprOpenGL->makeEGLCurrent();
    auto sourceFramebuffer = captureLayerFramebuffer(layer);
    if (!sourceFramebuffer || !sourceFramebuffer->isAllocated() || !sourceFramebuffer->getTexture()) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar capture missing source namespace=" << layer->m_namespace << " monitor=" << monitor->m_name;
            debugLog(out.str());
        }
        return false;
    }

    const bool rawNiriLayoutLayer = isNiriWallpaperLayoutLayer(layer, monitor);
    WORKSPACEID niriWallpaperWorkspaceId = WORKSPACE_INVALID;
    if (rawNiriLayoutLayer && monitor) {
        if (monitor->m_activeWorkspace && !monitor->m_activeWorkspace->m_isSpecialWorkspace)
            niriWallpaperWorkspaceId = monitor->m_activeWorkspace->m_id;
        else if (m_state.ownerWorkspace && m_state.ownerWorkspace->m_monitor.lock() == monitor && !m_state.ownerWorkspace->m_isSpecialWorkspace)
            niriWallpaperWorkspaceId = m_state.ownerWorkspace->m_id;
    }

    auto existingIt = std::find_if(m_hiddenStripLayerProxies.begin(), m_hiddenStripLayerProxies.end(),
                                   [&](const HiddenStripLayerProxy& proxy) {
                                       if (proxy.layer != layer || proxy.monitor != monitor)
                                           return false;
                                       if (rawNiriLayoutLayer || proxy.niriWallpaperLayoutLayer)
                                           return proxy.niriWallpaperLayoutLayer && proxy.niriWallpaperWorkspaceId == niriWallpaperWorkspaceId;
                                       return !proxy.niriWallpaperLayoutLayer;
                                   });
    auto*      existing = existingIt == m_hiddenStripLayerProxies.end() ? nullptr : &*existingIt;
    const bool niriLayoutLayer = rawNiriLayoutLayer || (existing && existing->niriWallpaperLayoutLayer);
    if (!existing) {
        HiddenStripLayerProxy proxy;
        proxy.layer = layer;
        proxy.monitor = monitor;
        proxy.capturedRectGlobal = capturedRectGlobal;
        proxy.proxyRectGlobal = proxyRectGlobal;
        proxy.snapshotSize = Vector2D{static_cast<double>(fbWidth), static_cast<double>(fbHeight)};
        proxy.framebuffer = createFramebuffer("hymission hidden strip layer");
        proxy.niriWallpaperLayoutLayer = niriLayoutLayer;
        proxy.niriWallpaperWorkspaceId = niriLayoutLayer ? niriWallpaperWorkspaceId : WORKSPACE_INVALID;
        if (!niriLayoutLayer) {
            for (auto& blurredFramebuffer : proxy.blurredFramebuffers)
                blurredFramebuffer = createFramebuffer("hymission hidden strip layer blur");
        }
        m_hiddenStripLayerProxies.push_back(std::move(proxy));
        existing = &m_hiddenStripLayerProxies.back();
    }

    existing->capturedRectGlobal = capturedRectGlobal;
    existing->proxyRectGlobal = proxyRectGlobal;
    existing->snapshotSize = Vector2D{static_cast<double>(fbWidth), static_cast<double>(fbHeight)};
    existing->niriWallpaperLayoutLayer = niriLayoutLayer;
    existing->niriWallpaperWorkspaceId = niriLayoutLayer ? niriWallpaperWorkspaceId : WORKSPACE_INVALID;
    if (!existing->framebuffer)
        existing->framebuffer = createFramebuffer("hymission hidden strip layer");
    if (niriLayoutLayer) {
        for (auto& blurredFramebuffer : existing->blurredFramebuffers)
            blurredFramebuffer.reset();
    } else {
        for (auto& blurredFramebuffer : existing->blurredFramebuffers) {
            if (!blurredFramebuffer)
                blurredFramebuffer = createFramebuffer("hymission hidden strip layer blur");
        }
    }

    if (!existing->framebuffer->isAllocated() || std::abs(existing->framebuffer->m_size.x - fbWidth) > 0.5 || std::abs(existing->framebuffer->m_size.y - fbHeight) > 0.5) {
        existing->framebuffer->release();
        if (!existing->framebuffer->alloc(fbWidth, fbHeight)) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] strip-bar capture framebuffer alloc failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                    << " fb=(" << fbWidth << "x" << fbHeight << ")";
                debugLog(out.str());
            }
            return false;
        }
        setFramebufferLinearFiltering(*existing->framebuffer);
    }

    if (!niriLayoutLayer) {
        for (auto& blurredFramebuffer : existing->blurredFramebuffers) {
            if (!blurredFramebuffer->isAllocated() || std::abs(blurredFramebuffer->m_size.x - fbWidth) > 0.5 ||
                std::abs(blurredFramebuffer->m_size.y - fbHeight) > 0.5) {
                blurredFramebuffer->release();
                if (!blurredFramebuffer->alloc(fbWidth, fbHeight)) {
                    if (debugLogsEnabled()) {
                        std::ostringstream out;
                        out << "[hymission] strip-bar capture blur framebuffer alloc failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                            << " fb=(" << fbWidth << "x" << fbHeight << ")";
                        debugLog(out.str());
                    }
                    return false;
                }
                setFramebufferLinearFiltering(*blurredFramebuffer);
            }
        }
    }

    const double monitorRenderWidth = std::max(1.0, static_cast<double>(monitor->m_size.x) * renderScaleForMonitor(monitor));
    const double monitorRenderHeight = std::max(1.0, static_cast<double>(monitor->m_size.y) * renderScaleForMonitor(monitor));
    const Rect   proxyRectRenderLocal = rectToMonitorRenderLocal(proxyRectGlobal, monitor);
    const Rect   capturedRectRenderLocal = rectToMonitorRenderLocal(capturedRectGlobal, monitor);
    const double targetOffsetX = capturedRectRenderLocal.x - proxyRectRenderLocal.x;
    const double targetOffsetY = capturedRectRenderLocal.y - proxyRectRenderLocal.y;
    constexpr double kSnapshotSizeTolerance = 2.0;
    const bool       sourceMatchesProxy =
        std::abs(sourceFramebuffer->m_size.x - static_cast<double>(fbWidth)) <= kSnapshotSizeTolerance &&
        std::abs(sourceFramebuffer->m_size.y - static_cast<double>(fbHeight)) <= kSnapshotSizeTolerance;
    const bool sourceMatchesCaptured =
        std::abs(sourceFramebuffer->m_size.x - capturedRectRenderLocal.width) <= kSnapshotSizeTolerance &&
        std::abs(sourceFramebuffer->m_size.y - capturedRectRenderLocal.height) <= kSnapshotSizeTolerance;
    const bool sourceMatchesMonitor =
        std::abs(sourceFramebuffer->m_size.x - monitorRenderWidth) <= kSnapshotSizeTolerance &&
        std::abs(sourceFramebuffer->m_size.y - monitorRenderHeight) <= kSnapshotSizeTolerance;

    Rect         sourceRect = makeRect(0.0, 0.0, sourceFramebuffer->m_size.x, sourceFramebuffer->m_size.y);
    Rect         targetRect = makeRect(targetOffsetX, targetOffsetY, sourceFramebuffer->m_size.x, sourceFramebuffer->m_size.y);
    SP<Render::IFramebuffer> croppedFramebuffer;
    bool                     useCroppedFramebuffer = false;
    if (sourceMatchesProxy) {
        targetRect = makeRect(0.0, 0.0, sourceFramebuffer->m_size.x, sourceFramebuffer->m_size.y);
    } else if (sourceMatchesCaptured) {
        targetRect = makeRect(targetOffsetX, targetOffsetY, sourceFramebuffer->m_size.x, sourceFramebuffer->m_size.y);
    } else if (sourceMatchesMonitor) {
        const double sourceX = std::clamp(capturedRectRenderLocal.x * sourceFramebuffer->m_size.x / monitorRenderWidth, 0.0,
                                          std::max(0.0, sourceFramebuffer->m_size.x - capturedRectRenderLocal.width));
        const double sourceY = std::clamp(capturedRectRenderLocal.y * sourceFramebuffer->m_size.y / monitorRenderHeight, 0.0,
                                          std::max(0.0, sourceFramebuffer->m_size.y - capturedRectRenderLocal.height));
        const int croppedWidth = std::max(1, static_cast<int>(std::lround(capturedRectRenderLocal.width)));
        const int croppedHeight = std::max(1, static_cast<int>(std::lround(capturedRectRenderLocal.height)));
        croppedFramebuffer = createFramebuffer("hymission hidden strip layer crop");
        if (!croppedFramebuffer || !croppedFramebuffer->alloc(croppedWidth, croppedHeight)) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] strip-bar capture cropped framebuffer alloc failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                    << " fb=(" << croppedWidth << "x" << croppedHeight << ")";
                debugLog(out.str());
            }
            return false;
        }
        setFramebufferLinearFiltering(*croppedFramebuffer);

        if (!renderTextureIntoFramebuffer(monitor, croppedFramebuffer, sourceFramebuffer->getTexture(),
                                          CBox(-sourceX, -sourceY, sourceFramebuffer->m_size.x, sourceFramebuffer->m_size.y))) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] strip-bar capture cropped blit failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                    << " sourceOffset=(" << sourceX << "," << sourceY << ")";
                debugLog(out.str());
            }
            return false;
        }

        sourceRect = makeRect(0.0, 0.0, croppedFramebuffer->m_size.x, croppedFramebuffer->m_size.y);
        targetRect = makeRect(targetOffsetX, targetOffsetY, capturedRectRenderLocal.width, capturedRectRenderLocal.height);
        useCroppedFramebuffer = true;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] strip-bar capture namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
            << " captured=" << rectToString(capturedRectGlobal) << " proxy=" << rectToString(proxyRectGlobal)
            << " capturedRender=" << rectToString(capturedRectRenderLocal) << " proxyRender=" << rectToString(proxyRectRenderLocal)
            << " targetOffset=(" << targetOffsetX << "," << targetOffsetY << ")"
            << " sourceFb=(" << sourceFramebuffer->m_size.x << "x" << sourceFramebuffer->m_size.y << ")"
            << " fb=(" << fbWidth << "x" << fbHeight << ")"
            << " matchProxy=" << sourceMatchesProxy << " matchCaptured=" << sourceMatchesCaptured << " matchMonitor=" << sourceMatchesMonitor
            << " sourceRect=" << rectToString(sourceRect) << " targetRect=" << rectToString(targetRect);
        debugLog(out.str());
    }

    auto& blitSourceFramebuffer = useCroppedFramebuffer ? *croppedFramebuffer : *sourceFramebuffer;
    if (!blitFramebufferRegion(blitSourceFramebuffer, *existing->framebuffer, sourceRect, targetRect)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar capture blit failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                << " sourceRect=" << rectToString(sourceRect) << " targetRect=" << rectToString(targetRect);
            debugLog(out.str());
        }
        return false;
    }

    if (!niriLayoutLayer && !buildBlurredProxyFramebuffers(existing->framebuffer, existing->blurredFramebuffers)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar capture blur build failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name;
            debugLog(out.str());
        }
        return true;
    }

    return true;
}

void OverviewController::syncHiddenStripLayerProxies() {
    if (!isVisible() || (!hideBarAnimationEffectsEnabled() && !niriWallpaperZoomAppliesToState(m_state))) {
        clearHiddenStripLayerProxies();
        return;
    }

    if (niriModeAppliesToState(m_state) && m_state.collectionPolicy.onlyActiveWorkspace && m_state.windows.empty()) {
        // In the empty direct-Niri path, regular hide-bar snapshots are unsafe
        // during open, but wallpaper-layout proxies are captured by the delayed
        // niri_mode_wallpaper_zoom_layer_refresh_ms timer and must be retained
        // so those layers keep zooming with the workspace viewport.
        std::erase_if(m_hiddenStripLayerProxies, [](const HiddenStripLayerProxy& proxy) { return !proxy.niriWallpaperLayoutLayer; });
        return;
    }

    std::vector<std::pair<PHLLS, PHLMONITOR>> desired;
    for (const auto& layer : g_pCompositor->m_layers) {
        const auto monitor = layer ? layer->m_monitor.lock() : PHLMONITOR{};
        if (!shouldHideLayerSurface(layer, monitor))
            continue;
        if (isNiriWallpaperLayer(layer, monitor))
            continue;

        desired.emplace_back(layer, monitor);
        const bool captured = captureHiddenStripLayerProxy(layer, monitor);
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar sync namespace=" << layer->m_namespace << " monitor="
                << (monitor ? monitor->m_name : std::string("<null-monitor>")) << " captured=" << (captured ? 1 : 0);
            debugLog(out.str());
        }
        if (!captured) {
            std::erase_if(m_hiddenStripLayerProxies, [&](const HiddenStripLayerProxy& proxy) { return proxy.layer == layer && proxy.monitor == monitor; });
        }
    }

    m_hiddenStripLayerProxies.erase(std::remove_if(m_hiddenStripLayerProxies.begin(), m_hiddenStripLayerProxies.end(),
                                                   [&](const HiddenStripLayerProxy& proxy) {
                                                       return std::none_of(desired.begin(), desired.end(), [&](const auto& entry) {
                                                           return entry.first == proxy.layer && entry.second == proxy.monitor;
                                                       });
                                                   }),
                                    m_hiddenStripLayerProxies.end());
}

void OverviewController::syncNiriWallpaperLayoutLayerProxies() {
    if (!isVisible() || !niriWallpaperZoomAppliesToState(m_state))
        return;

    std::vector<std::pair<PHLLS, PHLMONITOR>> desired;
    for (const auto& layer : g_pCompositor->m_layers) {
        const auto monitor = layer ? layer->m_monitor.lock() : PHLMONITOR{};
        if (!isRetainedNiriWallpaperLayoutLayer(layer, monitor))
            continue;

        desired.emplace_back(layer, monitor);
        if (!captureHiddenStripLayerProxy(layer, monitor) && debugLogsEnabled())
            debugLog("[hymission] niri layout layer refresh failed namespace=" + layer->m_namespace + " monitor=" + monitor->m_name);
    }

    std::erase_if(m_hiddenStripLayerProxies, [&](const HiddenStripLayerProxy& proxy) {
        if (!proxy.niriWallpaperLayoutLayer)
            return false;

        const bool layerStillDesired = std::any_of(desired.begin(), desired.end(), [&](const auto& entry) {
            return entry.first == proxy.layer && entry.second == proxy.monitor;
        });
        if (!layerStillDesired)
            return true;

        // Keep per-workspace captures while the overview is open.  Empty Niri
        // workspaces can be synthetic/no-window workspaces, so the only stable
        // thing distinguishing their layer snapshots is the workspace id captured
        // when that workspace was active.  Replacing the single monitor-wide proxy
        // made every empty wallpaper viewport show the current desktop.
        return false;
    });
}

void OverviewController::startNiriWallpaperLayoutLayerRefresh() {
    const auto interval = niriModeWallpaperZoomLayerRefreshInterval();
    if (interval.count() <= 0 || !g_pEventLoopManager || !isVisible() || !niriWallpaperZoomAppliesToState(m_state))
        return;

    if (!m_niriWallpaperLayoutLayerRefreshTimer) {
        m_niriWallpaperLayoutLayerRefreshTimer = makeShared<CEventLoopTimer>(
            interval,
            [this](SP<CEventLoopTimer> self, void*) {
                if (g_controller != this || !isVisible() || !niriWallpaperZoomAppliesToState(m_state) ||
                    m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle) {
                    self->updateTimeout(std::nullopt);
                    return;
                }

                if (g_pHyprRenderer && g_pHyprRenderer->m_renderData.pMonitor) {
                    self->updateTimeout(THEME_SURFACE_FEEDBACK_INTERVAL);
                    return;
                }

                syncNiriWallpaperLayoutLayerProxies();
                damageOwnedMonitors();
                const auto nextInterval = niriModeWallpaperZoomLayerRefreshInterval();
                self->updateTimeout(nextInterval.count() > 0 ? std::optional{nextInterval} : std::nullopt);
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_niriWallpaperLayoutLayerRefreshTimer);
        return;
    }

    m_niriWallpaperLayoutLayerRefreshTimer->updateTimeout(interval);
}

void OverviewController::clearNiriWallpaperLayoutLayerRefresh() {
    if (!m_niriWallpaperLayoutLayerRefreshTimer)
        return;

    m_niriWallpaperLayoutLayerRefreshTimer->cancel();
    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_niriWallpaperLayoutLayerRefreshTimer);
    m_niriWallpaperLayoutLayerRefreshTimer.reset();
}

Rect OverviewController::hiddenStripLayerProxyRect(const HiddenStripLayerProxy& proxy) const {
    const double hiddenness = hiddenStripLayerProgress(proxy.layer, proxy.monitor);
    if (!workspaceStripEnabled(m_state))
        return proxy.proxyRectGlobal;

    const auto   anchor = parseWorkspaceStripAnchor(workspaceStripAnchor());
    const double scaleTarget = 1.0 / hideBarAnimationScaleDivisor();
    const double scale = 1.0 - (1.0 - scaleTarget) * hiddenness;
    const Rect   stripBand = workspaceStripBandRectForMonitor(proxy.monitor, m_state);
    const double moveMultiplier = hideBarAnimationMoveMultiplier();

    Rect rect = scaleRectFromAnchor(proxy.proxyRectGlobal, proxy.capturedRectGlobal, anchor, scale, scale);
    switch (anchor) {
        case WorkspaceStripAnchor::Left:
            rect = translateRect(rect, stripBand.width * hiddenness * moveMultiplier, 0.0);
            break;
        case WorkspaceStripAnchor::Right:
            rect = translateRect(rect, -stripBand.width * hiddenness * moveMultiplier, 0.0);
            break;
        case WorkspaceStripAnchor::Top:
        default:
            rect = translateRect(rect, 0.0, stripBand.height * hiddenness * moveMultiplier);
            break;
    }

    return rect;
}

bool OverviewController::shouldRenderHiddenStripLayerProxy(const PHLLS& layer, const PHLMONITOR& monitor) const {
    if (!shouldHideLayerSurface(layer, monitor))
        return false;

    // Closing completion schedules deferred deactivate before the overlay pass
    // is emitted again. Hand rendering back to the real layer immediately so
    // there is no one-frame gap where both the live bar and proxy are absent.
    if (m_deactivatePending)
        return false;

    const auto* proxy = hiddenStripLayerProxyFor(layer, monitor);
    auto*       framebuffer = proxy && proxy->framebuffer ? proxy->framebuffer.get() : nullptr;
    return proxy && framebuffer && framebuffer->isAllocated() && framebuffer->getTexture();
}

void OverviewController::renderHiddenStripLayerProxies() const {
    if (m_hiddenStripLayerProxies.empty() || !g_pHyprOpenGL || !hideBarAnimationEffectsEnabled())
        return;

    const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!renderMonitor)
        return;

    for (const auto& proxy : m_hiddenStripLayerProxies) {
        if (!proxy.layer || !proxy.monitor || proxy.monitor != renderMonitor)
            continue;
        if (!shouldHideLayerSurface(proxy.layer, renderMonitor))
            continue;
        if (proxy.niriWallpaperLayoutLayer)
            continue;

        auto* sourceFramebuffer = proxy.framebuffer ? proxy.framebuffer.get() : nullptr;
        if (!sourceFramebuffer || !sourceFramebuffer->isAllocated() || !sourceFramebuffer->getTexture())
            continue;

        const double hiddenness = hiddenStripLayerProgress(proxy.layer, renderMonitor);
        const double alphaEnd = hideBarAnimationAlphaEnd();
        const float  proxyAlpha = static_cast<float>(std::clamp(1.0 + (alphaEnd - 1.0) * hiddenness, 0.0, 1.0));
        if (proxyAlpha <= 0.001F)
            continue;

        const float blurStrength =
            hideBarAnimationBlurEnabled() ? static_cast<float>(easeOutCubic(clampUnit((hiddenness - 0.12) / 0.38))) : 0.0F;
        const float sharpAlpha = hideBarAnimationBlurEnabled() ? (proxyAlpha * std::pow(std::max(0.0F, 1.0F - blurStrength), 2.0F)) : proxyAlpha;
        const float blurredAlpha = hideBarAnimationBlurEnabled() ? (proxyAlpha * std::clamp(0.2F + 0.8F * blurStrength, 0.0F, 1.0F)) : 0.0F;
        const Rect  targetRect = rectToMonitorRenderLocal(hiddenStripLayerProxyRect(proxy), renderMonitor);
        if (targetRect.width <= 0.0 || targetRect.height <= 0.0)
            continue;

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar render namespace=" << proxy.layer->m_namespace << " monitor=" << renderMonitor->m_name
                << " hiddenness=" << hiddenness << " proxyAlpha=" << proxyAlpha << " blurStrength=" << blurStrength
                << " proxyRect=" << rectToString(proxy.proxyRectGlobal) << " capturedRect=" << rectToString(proxy.capturedRectGlobal)
                << " targetRect=" << rectToString(targetRect);
            debugLog(out.str());
        }

        if (sharpAlpha > 0.002F)
            g_pHyprOpenGL->renderTexture(sourceFramebuffer->getTexture(), toBox(targetRect), {.a = sharpAlpha});

        if (blurredAlpha <= 0.002F)
            continue;

        constexpr std::size_t blurLevelCount = 4;
        const float           blurLevel = std::clamp(blurStrength * static_cast<float>(blurLevelCount - 1), 0.0F, static_cast<float>(blurLevelCount - 1));
        const auto            lowerIndex = static_cast<std::size_t>(std::floor(blurLevel));
        const auto            upperIndex = std::min(blurLevelCount - 1, lowerIndex + 1);
        const float           upperWeight = std::clamp(blurLevel - static_cast<float>(lowerIndex), 0.0F, 1.0F);
        const float           lowerWeight = 1.0F - upperWeight;

        const auto renderBlurLevel = [&](std::size_t index, float alpha) {
            auto* blurredFramebuffer = index < proxy.blurredFramebuffers.size() && proxy.blurredFramebuffers[index] ? proxy.blurredFramebuffers[index].get() : nullptr;
            if (!blurredFramebuffer || !blurredFramebuffer->isAllocated() || !blurredFramebuffer->getTexture() || alpha <= 0.002F)
                return;
            g_pHyprOpenGL->renderTexture(blurredFramebuffer->getTexture(), toBox(targetRect), {.a = alpha});
        };

        renderBlurLevel(lowerIndex, blurredAlpha * lowerWeight);
        if (upperIndex != lowerIndex)
            renderBlurLevel(upperIndex, blurredAlpha * upperWeight);
    }
}

bool OverviewController::suppressSurfaceBlur(void* surfacePassThisptr) const {
    auto* renderData = surfaceRenderDataMutable(surfacePassThisptr);
    if (!renderData || !renderData->pWindow || renderData->popup || !renderData->blur)
        return false;

    const auto monitor = renderData->pMonitor.lock();
    if (!monitor || !ownsMonitor(monitor) || !renderableManagedWindowFor(renderData->pWindow, monitor))
        return false;

    const bool wallpaperWorkspace = niriWallpaperZoomAppliesToMonitor(m_state, monitor);
    if (!wallpaperWorkspace && !isAnimating())
        return false;

    if (debugSurfaceLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] blur suppress " << debugWindowLabel(renderData->pWindow) << " phase="
            << (m_state.phase == Phase::Opening ? "opening" : m_state.phase == Phase::Closing ? "closing" : "active")
            << " wallpaperWorkspace=" << (wallpaperWorkspace ? 1 : 0);
        debugSurfaceLog(out.str());
    }

    renderData->blur = false;
    return true;
}

bool OverviewController::prepareSurfaceRenderData(void* surfacePassThisptr, const char* context, CSurfacePassElement::SRenderData*& renderData, PHLMONITOR& monitor,
                                                  SurfaceRenderDataSnapshot& snapshot) const {
    renderData = surfaceRenderDataMutable(surfacePassThisptr);
    if (!renderData || !renderData->pWindow)
        return false;

    monitor = renderData->pMonitor.lock();
    if (!monitor || !isVisible() || !ownsMonitor(monitor) || !renderableManagedWindowFor(renderData->pWindow, monitor))
        return false;

    if ((usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state)) &&
        (m_deactivatePending || directNiriNativeHandoffActive()))
        return false;

    snapshot = {
        .pos = renderData->pos,
        .localPos = renderData->localPos,
        .w = renderData->w,
        .h = renderData->h,
        .rounding = renderData->rounding,
        .dontRound = renderData->dontRound,
        .roundingPower = renderData->roundingPower,
        .alpha = renderData->alpha,
        .fadeAlpha = renderData->fadeAlpha,
        .blur = renderData->blur,
        .blockBlurOptimization = renderData->blockBlurOptimization,
        .clipBox = renderData->clipBox,
    };

    const bool suppressBlur = suppressSurfaceBlur(surfacePassThisptr);
    const bool transformed = transformSurfaceRenderDataForWindow(renderData->pWindow, monitor, *renderData);
    if (transformed) {
        renderData->alpha = managedPreviewAlphaFor(renderData->pWindow, snapshot.alpha);
        if (!renderData->pWindow->m_fadingOut)
            renderData->fadeAlpha = 1.0F;
        if (suppressBlur)
            renderData->blur = false;
    }

    if (transformed && debugSurfaceLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] surface " << (context ? context : "?") << ' ' << debugWindowLabel(renderData->pWindow) << " main=" << renderData->mainSurface
            << " popup=" << renderData->popup << " monitor=" << monitor->m_name << " pos=" << vectorToString(snapshot.pos) << "->" << vectorToString(renderData->pos)
            << " local=" << vectorToString(snapshot.localPos) << "->" << vectorToString(renderData->localPos) << " size=" << snapshot.w << 'x' << snapshot.h << "->"
            << renderData->w << 'x' << renderData->h << " alpha=" << snapshot.alpha << "->" << renderData->alpha << " fadeAlpha=" << snapshot.fadeAlpha << "->"
            << renderData->fadeAlpha << " clip=" << boxToString(snapshot.clipBox) << "->"
            << boxToString(renderData->clipBox);
        debugSurfaceLog(out.str());
    }

    return transformed;
}

void OverviewController::restoreSurfaceRenderData(CSurfacePassElement::SRenderData* renderData, const SurfaceRenderDataSnapshot& snapshot) const {
    if (!renderData)
        return;

    renderData->pos = snapshot.pos;
    renderData->localPos = snapshot.localPos;
    renderData->w = snapshot.w;
    renderData->h = snapshot.h;
    renderData->rounding = snapshot.rounding;
    renderData->dontRound = snapshot.dontRound;
    renderData->roundingPower = snapshot.roundingPower;
    renderData->alpha = snapshot.alpha;
    renderData->fadeAlpha = snapshot.fadeAlpha;
    renderData->blur = snapshot.blur;
    renderData->blockBlurOptimization = snapshot.blockBlurOptimization;
    renderData->clipBox = snapshot.clipBox;
}

std::optional<std::size_t> OverviewController::hitTestTarget(double x, double y) const {
    const auto hitLayer = [&](bool floatingOverlay) -> std::optional<std::size_t> {
        std::optional<std::size_t> bestIndex;
        double                     bestDistance = std::numeric_limits<double>::infinity();

        for (std::size_t index = 0; index < m_state.windows.size(); ++index) {
            const auto& managed = m_state.windows[index];
            if (managed.isNiriFloatingOverlay != floatingOverlay)
                continue;

            const Rect rect = currentPreviewRect(managed);
            if (!rectContainsPoint(rect, x, y))
                continue;

            const double distance = rectCenterDistanceSquared(rect, x, y);
            if (!bestIndex || distance < bestDistance) {
                bestIndex = index;
                bestDistance = distance;
            }
        }

        return bestIndex;
    };

    if (const auto floating = hitLayer(true))
        return floating;

    return hitLayer(false);
}

std::optional<std::size_t> OverviewController::hitTestStripTarget(double x, double y) const {
    return hitTestWorkspaceStrip(stripRects(), x, y);
}

std::optional<Rect> OverviewController::workspaceTransitionRectForWindow(const PHLWINDOW& window) const {
    if (!m_workspaceTransition.active)
        return std::nullopt;

    const auto* sourceManaged = managedWindowFor(m_workspaceTransition.sourceState, window, true);
    const auto* targetManaged = managedWindowFor(m_workspaceTransition.targetState, window, true);
    if (!sourceManaged && !targetManaged)
        return std::nullopt;

    const double clampedDelta = std::clamp(m_workspaceTransition.delta, -m_workspaceTransition.distance, m_workspaceTransition.distance);
    const double sourceOffset = -clampedDelta;
    const int    targetDirection = clampedDelta < -0.0001 ? -1 : clampedDelta > 0.0001 ? 1 : (m_workspaceTransition.step < 0 ? -1 : 1);
    const double targetOffset = sourceOffset + static_cast<double>(targetDirection) * m_workspaceTransition.distance;
    const double t = m_workspaceTransition.distance > 0.0 ? clampUnit(std::abs(clampedDelta) / m_workspaceTransition.distance) : 1.0;

    if ((sourceManaged && sourceManaged->isPinned) || (targetManaged && targetManaged->isPinned)) {
        if (sourceManaged && targetManaged)
            return lerpRect(sourceManaged->targetGlobal, targetManaged->targetGlobal, t);

        return sourceManaged ? sourceManaged->targetGlobal : targetManaged->targetGlobal;
    }

    double sourceDx = 0.0;
    double sourceDy = 0.0;
    double targetDx = 0.0;
    double targetDy = 0.0;
    if (m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical) {
        sourceDy = sourceOffset;
        targetDy = targetOffset;
    } else {
        sourceDx = sourceOffset;
        targetDx = targetOffset;
    }

    if (sourceManaged && targetManaged) {
        const Rect sourceRect = translateRect(sourceManaged->targetGlobal, sourceDx, sourceDy);
        const Rect targetRect = translateRect(targetManaged->targetGlobal, targetDx, targetDy);
        return lerpRect(sourceRect, targetRect, t);
    }

    if (sourceManaged)
        return translateRect(sourceManaged->targetGlobal, sourceDx, sourceDy);

    return translateRect(targetManaged->targetGlobal, targetDx, targetDy);
}

Rect OverviewController::stablePreviewOrderRect(const ManagedWindow& window) const {
    switch (m_state.phase) {
        case Phase::Opening:
            return lerpRect(window.naturalGlobal, window.targetGlobal, visualProgress());
        case Phase::Active:
            if (m_state.relayoutActive)
                return lerpRect(window.relayoutFromGlobal, window.targetGlobal, relayoutVisualProgress());
            return window.targetGlobal;
        case Phase::ClosingSettle:
        case Phase::Closing:
            return lerpRect(window.exitGlobal, window.targetGlobal, visualProgress());
        case Phase::Inactive:
            return window.naturalGlobal;
    }

    return window.targetGlobal;
}


double OverviewController::visualProgress() const {
    if (m_gestureSession.active)
        return clampUnit(m_gestureSession.openness);

    if ((m_state.phase == Phase::Opening || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle) &&
        m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state)) {
        if (m_overviewVisibilityAnimation)
            return clampUnit(m_overviewVisibilityAnimation->value());
        return clampUnit(m_state.animationFromVisual);
    }

    switch (m_state.phase) {
        case Phase::Opening:
            return std::clamp(m_state.animationFromVisual + (m_state.animationToVisual - m_state.animationFromVisual) * easeOutCubic(m_state.animationProgress), 0.0, 1.0);
        case Phase::ClosingSettle:
            return clampUnit(m_state.animationFromVisual);
        case Phase::Closing:
            return std::clamp(m_state.animationFromVisual + (m_state.animationToVisual - m_state.animationFromVisual) * easeInCubic(m_state.animationProgress), 0.0, 1.0);
        case Phase::Active:
            return 1.0;
        case Phase::Inactive:
            return 0.0;
    }

    return 0.0;
}

double OverviewController::workspaceStripEnterProgress() const {
    return visualProgress();
}

Vector2D OverviewController::workspaceStripEnterOffset(const PHLMONITOR& monitor) const {
    if (!monitor || !workspaceStripEnabled(m_state))
        return {};

    if (usesDirectNiriScrollingOverview(m_state))
        return {};

    const double progress = workspaceStripEnterProgress();
    if (progress >= 1.0)
        return {};

    const Rect band = workspaceStripBandRectForMonitor(monitor, m_state);
    const double hiddenFraction = 1.0 - progress;

    switch (parseWorkspaceStripAnchor(workspaceStripAnchor())) {
        case WorkspaceStripAnchor::Left:
            return Vector2D{-band.width * hiddenFraction, 0.0};
        case WorkspaceStripAnchor::Right:
            return Vector2D{band.width * hiddenFraction, 0.0};
        case WorkspaceStripAnchor::Top:
        default:
            return Vector2D{0.0, -band.height * hiddenFraction};
    }
}

Rect OverviewController::animatedWorkspaceStripRect(const Rect& rect, const PHLMONITOR& monitor) const {
    const auto offset = workspaceStripEnterOffset(monitor);
    if (offset.x == 0.0 && offset.y == 0.0)
        return rect;

    return translateRect(rect, offset.x, offset.y);
}

Rect OverviewController::currentWorkspaceStripRect(const WorkspaceStripEntry& entry) const {
    if (m_state.phase == Phase::Active && m_state.relayoutActive && entry.hasRelayoutFromRect)
        return lerpRect(entry.relayoutFromRect, entry.rect, relayoutVisualProgress());

    return entry.rect;
}

double OverviewController::relayoutVisualProgress() const {
    if (!m_state.relayoutActive)
        return 1.0;

    return clampUnit(m_state.relayoutProgress);
}

void OverviewController::beginOverviewRelayoutAnimation(const char* source) {
    m_state.relayoutProgress = 0.0;
    m_state.relayoutStart = std::chrono::steady_clock::now();
    m_relayoutProgressAnimation.reset();

    const auto config = windowsMoveAnimationConfig();
    if (!g_pAnimationManager || !config) {
        finishOverviewRelayoutAnimation();
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] relayout anim skipped source=" << (source ? source : "?") << " reason=missing-windowsMove-config";
            debugLog(out.str());
        }
        return;
    }

    g_pAnimationManager->createAnimation(0.0F, m_relayoutProgressAnimation, config, AVARDAMAGE_NONE);
    m_relayoutProgressAnimation->setUpdateCallback([this](auto) {
        if (isVisible())
            damageOwnedMonitors();
    });
    *m_relayoutProgressAnimation = 1.0F;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] relayout anim start source=" << (source ? source : "?");
        if (const auto values = config->pValues.lock()) {
            out << " enabled=" << values->internalEnabled
                << " speed=" << values->internalSpeed
                << " bezier=" << values->internalBezier
                << " style=" << values->internalStyle;
        }
        debugLog(out.str());
    }
}

void OverviewController::finishOverviewRelayoutAnimation() {
    if (m_relayoutProgressAnimation) {
        m_relayoutProgressAnimation->setValueAndWarp(1.0F);
        m_relayoutProgressAnimation.reset();
    }

    m_state.relayoutProgress = 1.0;
    m_state.relayoutActive = false;
    m_state.relayoutStart = {};
}

void OverviewController::beginOverviewVisibilityAnimation(const char* source) {
    m_overviewVisibilityAnimation.reset();
    m_overviewVisibilityAnimationConfig.reset();
    const auto config =
        scaledAnimationConfig(windowsMoveAnimationConfig(), niriOverviewOpenCloseSpeedMultiplier());
    if (!niriOverviewAnimationsEnabled() || !g_pAnimationManager || !config) {
        m_state.animationProgress = 1.0;
        m_state.animationFromVisual = m_state.animationToVisual;
        return;
    }

    m_overviewVisibilityAnimationConfig = config;
    g_pAnimationManager->createAnimation(static_cast<float>(m_state.animationFromVisual), m_overviewVisibilityAnimation, config, AVARDAMAGE_NONE);
    m_overviewVisibilityAnimation->setUpdateCallback([this](auto) {
        if (isVisible())
            damageOwnedMonitors();
    });
    *m_overviewVisibilityAnimation = static_cast<float>(m_state.animationToVisual);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview visibility anim start source=" << (source ? source : "?")
            << " speedMultiplier=" << niriOverviewOpenCloseSpeedMultiplier();
        if (const auto values = config->pValues.lock())
            out << " speed=" << values->internalSpeed << " bezier=" << values->internalBezier << " style=" << values->internalStyle;
        debugLog(out.str());
    }
}

void OverviewController::finishOverviewVisibilityAnimation() {
    if (m_overviewVisibilityAnimation) {
        m_overviewVisibilityAnimation->setValueAndWarp(static_cast<float>(m_state.animationToVisual));
        m_overviewVisibilityAnimation.reset();
    }
    m_overviewVisibilityAnimationConfig.reset();
    m_state.animationProgress = 1.0;
    m_state.animationFromVisual = m_state.animationToVisual;
    m_state.animationStart = {};
}

void OverviewController::beginWorkspaceTransitionAnimation(const char* source) {
    m_workspaceTransitionAnimation.reset();
    const auto config = workspaceAnimationConfig();
    if (!niriOverviewAnimationsEnabled() || !g_pAnimationManager || !config) {
        m_workspaceTransition.delta = m_workspaceTransition.animationToDelta;
        return;
    }

    g_pAnimationManager->createAnimation(static_cast<float>(m_workspaceTransition.animationFromDelta), m_workspaceTransitionAnimation, config, AVARDAMAGE_NONE);
    m_workspaceTransitionAnimation->setUpdateCallback([this](auto) {
        if (isVisible())
            damageOwnedMonitors();
    });
    *m_workspaceTransitionAnimation = static_cast<float>(m_workspaceTransition.animationToDelta);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] workspace transition anim start source=" << (source ? source : "?");
        debugLog(out.str());
    }
}

void OverviewController::finishWorkspaceTransitionAnimation() {
    if (m_workspaceTransitionAnimation) {
        m_workspaceTransitionAnimation->setValueAndWarp(static_cast<float>(m_workspaceTransition.animationToDelta));
        m_workspaceTransitionAnimation.reset();
    }
    m_workspaceTransition.delta = m_workspaceTransition.animationToDelta;
    m_workspaceTransition.animationProgress = 1.0;
    m_workspaceTransition.animationStart = {};
}

PHLWINDOW OverviewController::resolveExitFocus(CloseMode mode) const {
    if (mode == CloseMode::Abort)
        return {};

    if (resolveExitWorkspace(mode))
        return {};

    if (const auto target = preferredOverviewExitFocus(); target)
        return target;

    return m_state.focusBeforeOpen;
}

PHLWORKSPACE OverviewController::resolveExitWorkspace(CloseMode mode) const {
    if (mode == CloseMode::Abort)
        return {};

    const auto* placeholder = centeredEmptyWorkspacePlaceholder(m_state);
    if (placeholder)
        return placeholder->workspace;

    if (m_state.collectionPolicy.onlyActiveWorkspace && usesDirectNiriScrollingOverview(m_state) && m_state.ownerWorkspace &&
        !preferredOverviewExitFocus())
        return m_state.ownerWorkspace;

    return {};
}


OverviewController::EmptyWorkspacePlaceholder* OverviewController::pendingExitWorkspacePlaceholder() {
    if (m_state.pendingExitWorkspace) {
        const auto it = std::find_if(m_state.emptyWorkspacePlaceholders.begin(), m_state.emptyWorkspacePlaceholders.end(),
                                     [&](const EmptyWorkspacePlaceholder& placeholder) {
                                         return placeholder.workspace == m_state.pendingExitWorkspace ||
                                             placeholder.workspaceId == m_state.pendingExitWorkspace->m_id;
                                     });
        if (it != m_state.emptyWorkspacePlaceholders.end())
            return &*it;
    }

    if (!m_state.collectionPolicy.onlyActiveWorkspace || !niriModeAppliesToState(m_state) || !m_state.ownerMonitor)
        return nullptr;

    const Rect content = overviewContentRectForMonitor(m_state.ownerMonitor, m_state);
    const double centerX = m_state.ownerMonitor->m_position.x + content.centerX();
    const double centerY = m_state.ownerMonitor->m_position.y + content.centerY();

    EmptyWorkspacePlaceholder* best = nullptr;
    double bestDistance2 = std::numeric_limits<double>::max();
    for (auto& placeholder : m_state.emptyWorkspacePlaceholders) {
        if (placeholder.backingOnly || placeholder.monitor != m_state.ownerMonitor || placeholder.workspaceId == WORKSPACE_INVALID)
            continue;

        const double dx = placeholder.targetGlobal.centerX() - centerX;
        const double dy = placeholder.targetGlobal.centerY() - centerY;
        const double distance2 = dx * dx + dy * dy;
        if (distance2 < bestDistance2) {
            best = &placeholder;
            bestDistance2 = distance2;
        }
    }

    return best;
}

bool OverviewController::exitFocusChangedWorkspace(const PHLWINDOW& window) const {
    if (!window || !window->m_workspace || window->m_workspace->m_isSpecialWorkspace)
        return false;

    if (!m_state.focusBeforeOpen || !m_state.focusBeforeOpen->m_workspace || m_state.focusBeforeOpen->m_workspace->m_isSpecialWorkspace)
        return false;

    return window->m_workspace != m_state.focusBeforeOpen->m_workspace;
}

bool OverviewController::shouldPreferGoalExitGeometry(const PHLWINDOW& window) const {
    return window && window->m_workspace && (isScrollingWorkspace(window->m_workspace) || exitFocusChangedWorkspace(window));
}

std::optional<Vector2D> OverviewController::visiblePointForWindowOnMonitor(const PHLWINDOW& window, const PHLMONITOR& monitor, bool preferGoal) const {
    if (!window || !monitor || !window->m_isMapped)
        return std::nullopt;

    if (preferGoal) {
        const auto goalPoint = visiblePointForRectOnMonitor(goalGlobalRectForWindow(window), monitor);
        if (goalPoint)
            return goalPoint;
    }

    return visiblePointForRectOnMonitor(liveGlobalRectForWindow(window), monitor);
}

bool OverviewController::shouldClearWorkspaceFullscreenForExitTarget(const PHLWINDOW& window) const {
    if (!window || !window->m_isMapped)
        return false;

    const auto* backup = fullscreenBackupForWorkspace(window->m_workspace);
    if (!backup || !backup->workspace || !backup->hadFullscreenWindow)
        return false;

    if (window->m_workspace != backup->workspace || window->m_fullscreenState.internal != FSMODE_NONE)
        return false;

    PHLWINDOW fullscreenWindow;
    for (const auto& candidate : g_pCompositor->m_windows) {
        if (!candidate || !candidate->m_isMapped || candidate->m_workspace != backup->workspace)
            continue;

        if (candidate->m_fullscreenState.internal != FSMODE_NONE) {
            fullscreenWindow = candidate;
            break;
        }
    }

    return fullscreenWindow && fullscreenWindow != window;
}

bool OverviewController::clearWorkspaceFullscreenForExitTarget(const PHLWINDOW& window) {
    if (!shouldClearWorkspaceFullscreenForExitTarget(window))
        return false;

    auto backupIt = std::find_if(m_state.fullscreenBackups.begin(), m_state.fullscreenBackups.end(),
                                 [&](const FullscreenWorkspaceBackup& backup) { return backup.workspace == window->m_workspace; });
    if (backupIt == m_state.fullscreenBackups.end() || !backupIt->workspace)
        return false;

    PHLWINDOW fullscreenWindow;
    for (const auto& candidate : g_pCompositor->m_windows) {
        if (!candidate || !candidate->m_isMapped || candidate->m_workspace != backupIt->workspace)
            continue;

        if (candidate->m_fullscreenState.internal != FSMODE_NONE) {
            fullscreenWindow = candidate;
            break;
        }
    }

    if (!fullscreenWindow || fullscreenWindow == window)
        return false;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] clear workspace fullscreen source=" << debugWindowLabel(fullscreenWindow) << " target=" << debugWindowLabel(window);
        debugLog(out.str());
    }

    g_pCompositor->setWindowFullscreenInternal(fullscreenWindow, FSMODE_NONE);

    if (backupIt->workspace) {
        backupIt->workspace->m_hasFullscreenWindow = false;
        backupIt->workspace->m_fullscreenMode = FSMODE_NONE;
    }
    if (const auto workspaceMonitor = backupIt->workspace->m_monitor.lock())
        workspaceMonitor->m_solitaryClient.reset();

    backupIt->hadFullscreenWindow = false;
    backupIt->fullscreenMode = FSMODE_NONE;

    return true;
}

bool OverviewController::activateWindowWorkspaceForFocus(const PHLWINDOW& window) const {
    if (!window || !window->m_isMapped || window->m_pinned || !window->m_workspace)
        return false;

    const auto workspace = window->m_workspace;
    auto       monitor = workspace->m_monitor.lock();
    if (!monitor)
        monitor = window->m_monitor.lock();
    if (!monitor)
        return false;

    if (workspace->m_isSpecialWorkspace) {
        if (monitor->m_activeSpecialWorkspace == workspace)
            return false;

        workspace->m_lastFocusedWindow = window;
        monitor->changeWorkspace(workspace, true, true, true);
        return true;
    }

    if (monitor->m_activeWorkspace == workspace)
        return false;

    workspace->m_lastFocusedWindow = window;
    monitor->changeWorkspace(workspace, true, true, true);
    return true;
}

bool OverviewController::activateWorkspaceForExit(const PHLWORKSPACE& workspace) const {
    if (!workspace || workspace->m_isSpecialWorkspace)
        return false;

    auto monitor = workspace->m_monitor.lock();
    if (!monitor)
        monitor = m_state.ownerMonitor;
    if (!monitor)
        return false;

    workspace->m_lastFocusedWindow = PHLWINDOW{};
    clearWindowFocusCompat(monitor);

    const bool alreadyActive = monitor->m_activeWorkspace == workspace;
    if (!alreadyActive)
        monitor->changeWorkspace(workspace, true, true, false);

    clearWindowFocusCompat(monitor);
    workspace->m_lastFocusedWindow = PHLWINDOW{};
    if (g_pAnimationManager)
        g_pAnimationManager->frameTick();

    return !alreadyActive;
}

void OverviewController::normalizeDirectNiriWorkspaceActivation(const PHLWORKSPACE& workspace) const {
    if (!workspace || !m_state.collectionPolicy.onlyActiveWorkspace || !niriModeAppliesToState(m_state) || !isScrollingWorkspace(workspace))
        return;

    const auto monitor = workspace->m_monitor.lock();
    if (!monitor)
        return;

    for (const auto& candidate : g_pCompositor->getWorkspacesCopy()) {
        if (!candidate || candidate->m_isSpecialWorkspace || candidate->m_monitor.lock() != monitor)
            continue;

        const bool active = candidate == monitor->m_activeWorkspace;
        candidate->m_visible = active;
        candidate->m_forceRendering = false;
    }

    workspace->m_renderOffset->setValueAndWarp(Vector2D{});
    workspace->m_alpha->setValueAndWarp(1.F);
    g_layoutManager->recalculateMonitor(monitor);
}

void OverviewController::commitOverviewExitFocus(const PHLWINDOW& window) {
    if (!window || !window->m_isMapped)
        return;

    const bool alreadyFocused = Desktop::focusState()->window() == window;
    const bool activatedWorkspace = activateWindowWorkspaceForFocus(window);
    normalizeDirectNiriWorkspaceActivation(window->m_workspace);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] commit exit focus target=" << debugWindowLabel(window);
        const auto activeBefore = Desktop::focusState()->window();
        if (activeBefore)
            out << " activeBefore=" << debugWindowLabel(activeBefore);
        else
            out << " activeBefore=<null>";
        out << " alreadyFocused=" << (alreadyFocused ? 1 : 0);
        out << " activatedWorkspace=" << (activatedWorkspace ? 1 : 0);
        debugLog(out.str());
    }

    if (!alreadyFocused || activatedWorkspace)
        focusWindowCompat(window, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

    if (window->m_isFloating)
        g_pCompositor->changeWindowZOrder(window, true);

    recordWindowActivation(window, true);
    (void)syncScrollingWorkspaceSpotOnWindow(window);

    if (m_animationsEnabledOverridden && g_pAnimationManager) {
        // Live focus can switch the real workspace before close starts. Even when the
        // target is already active, force one animation tick so Hyprland flushes the
        // workspace scene that overview was previously masking.
        g_pAnimationManager->frameTick();
        if (debugLogsEnabled())
            debugLog("[hymission] commit exit focus forced animation frameTick");
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] commit exit focus result=";
        const auto activeAfter = Desktop::focusState()->window();
        if (activeAfter)
            out << debugWindowLabel(activeAfter);
        else
            out << "<null>";
        debugLog(out.str());
    }
}

PHLWINDOW OverviewController::focusCandidateForWorkspace(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return {};

    if (const auto focused = Desktop::focusState()->window(); focused && focused->m_workspace == workspace && focused->m_isMapped)
        return focused;

    if (const auto lastFocused = workspace->getLastFocusedWindow(); lastFocused && lastFocused->m_isMapped)
        return lastFocused;

    if (const auto candidate = workspace->getFocusCandidate(); candidate && candidate->m_isMapped)
        return candidate;

    return workspace->getFirstWindow();
}


















void OverviewController::queueSelectionRetargetDuringOverview(const PHLWINDOW& window, bool syncScrollingSpot, const char* source, bool centerCursor) {
    if (!window || !window->m_isMapped || !hasManagedWindow(window))
        return;

    m_queuedOverviewSelectionTarget = window;
    m_queuedOverviewSelectionSyncScrollingSpot = syncScrollingSpot;
    m_queuedOverviewSelectionCenterCursor = centerCursor;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] queue selection retarget during overview target=" << debugWindowLabel(window)
            << " source=" << (source ? source : "?")
            << " syncScrollingSpot=" << (syncScrollingSpot ? 1 : 0)
            << " centerCursor=" << (centerCursor ? 1 : 0);
        debugLog(out.str());
    }
}

void OverviewController::flushQueuedSelectionRetargetDuringOverview() {
    const auto queuedTarget = m_queuedOverviewSelectionTarget.lock();
    if (!queuedTarget)
        return;

    if (!isVisible() || m_state.phase != Phase::Active || m_gestureSession.active || m_workspaceTransition.active || m_beginCloseInProgress)
        return;

    m_queuedOverviewSelectionTarget.reset();
    const bool syncScrollingSpot = m_queuedOverviewSelectionSyncScrollingSpot;
    const bool centerCursor = m_queuedOverviewSelectionCenterCursor;
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewSelectionCenterCursor = false;

    if (!queuedTarget->m_isMapped || !hasManagedWindow(queuedTarget))
        return;

    const auto queuedIt =
        std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == queuedTarget; });
    if (queuedIt == m_state.windows.end())
        return;

    const auto previousSelectedWindow = m_lastLayoutSelectedWindow.lock();
    m_state.selectedIndex = static_cast<std::size_t>(std::distance(m_state.windows.begin(), queuedIt));
    m_state.focusDuringOverview = queuedTarget;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] flush queued selection retarget during overview target=" << debugWindowLabel(queuedTarget)
            << " syncScrollingSpot=" << (syncScrollingSpot ? 1 : 0)
            << " centerCursor=" << (centerCursor ? 1 : 0)
            << " previousLayoutSelected=" << debugWindowLabel(previousSelectedWindow);
        debugLog(out.str());
    }

    updateSelectedWindowLayout(previousSelectedWindow);
    queueRealFocusDuringOverview(queuedTarget, syncScrollingSpot, "frame-coalesced", centerCursor);
}

void OverviewController::queueRealFocusDuringOverview(const PHLWINDOW& window, bool syncScrollingSpot, const char* source, bool centerCursor) {
    if (!window || !window->m_isMapped || !hasManagedWindow(window))
        return;

    m_queuedOverviewLiveFocusTarget = window;
    m_queuedOverviewLiveFocusSyncScrollingSpot = syncScrollingSpot;
    m_queuedOverviewLiveFocusCenterCursor = centerCursor;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] queue real focus during overview target=" << debugWindowLabel(window)
            << " source=" << (source ? source : "?")
            << " syncScrollingSpot=" << (syncScrollingSpot ? 1 : 0)
            << " centerCursor=" << (centerCursor ? 1 : 0);
        debugLog(out.str());
    }
}

void OverviewController::flushQueuedRealFocusDuringOverview() {
    const auto queuedTarget = m_queuedOverviewLiveFocusTarget.lock();
    if (!queuedTarget)
        return;

    if (!isVisible() || m_state.phase != Phase::Active || m_gestureSession.active || m_workspaceTransition.active || m_beginCloseInProgress)
        return;

    const bool syncScrollingSpot = m_queuedOverviewLiveFocusSyncScrollingSpot;
    const bool centerCursor = m_queuedOverviewLiveFocusCenterCursor;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusCenterCursor = false;

    if (!queuedTarget->m_isMapped || !hasManagedWindow(queuedTarget))
        return;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] flush queued real focus during overview target=" << debugWindowLabel(queuedTarget)
            << " syncScrollingSpot=" << (syncScrollingSpot ? 1 : 0)
            << " centerCursor=" << (centerCursor ? 1 : 0);
        debugLog(out.str());
    }

    syncRealFocusDuringOverview(queuedTarget, syncScrollingSpot);
    if (centerCursor)
        centerCursorOnOverviewWindow(queuedTarget, "frame-coalesced");
}


void OverviewController::updateSelectedWindowLayout(const PHLWINDOW& previousSelectedWindow) {
    if (!expandSelectedWindowEnabled() || !isVisible() || m_state.phase != Phase::Active || m_gestureSession.active || m_workspaceTransition.active)
        return;

    if (m_state.collectionPolicy.onlyActiveWorkspace && usesDirectNiriScrollingOverview(m_state))
        return;

    const bool multiWorkspaceOverview = !m_state.collectionPolicy.onlyActiveWorkspace;
    if (multiWorkspaceOverview && !multiWorkspaceExpandSelectedWindowEnabled())
        return;

    const auto currentSelection = selectedWindow();
    const auto currentSelectedWindow = currentSelection ? currentSelection : Desktop::focusState()->window();
    if (currentSelectedWindow == previousSelectedWindow)
        return;
    m_lastLayoutSelectedWindow = currentSelectedWindow;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] expand-selected relayout previous=" << debugWindowLabel(previousSelectedWindow)
            << " current=" << debugWindowLabel(currentSelectedWindow);
        if (m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size())
            out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        else
            out << " hovered=<null>";
        if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
            out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        else
            out << " selected=<null>";
        debugLog(out.str());
        logOverviewLayoutState("before expand-selected relayout", m_state);
    }

    m_hoverSelectionRetargetBlockedUntil = std::chrono::steady_clock::now() + HOVER_SELECTION_RETARGET_COOLDOWN;
    m_hoverSelectionRetargetCandidateIndex.reset();
    m_hoverSelectionRetargetCandidateSince = {};
    m_hoverSelectionRetargetCandidatePrimed = false;

    const auto managedForWindow = [&](const PHLWINDOW& window) -> ManagedWindow* {
        const auto it = std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == window; });
        return it == m_state.windows.end() ? nullptr : &*it;
    };

    auto* currentManaged = managedForWindow(currentSelectedWindow);
    if (!currentManaged) {
        rebuildVisibleState(currentSelectedWindow, true);
        return;
    }

    const std::size_t currentManagedIndex = static_cast<std::size_t>(currentManaged - m_state.windows.data());
    bool              shouldAnimateRelayout = false;
    std::vector<Rect> baseTargets;
    baseTargets.reserve(m_state.windows.size());
    for (auto& managed : m_state.windows) {
        managed.relayoutFromGlobal = currentPreviewRect(managed);
        if (managed.targetMonitor) {
            managed.targetGlobal =
                makeRect(managed.targetMonitor->m_position.x + managed.slot.target.x, managed.targetMonitor->m_position.y + managed.slot.target.y,
                         managed.slot.target.width, managed.slot.target.height);
        } else {
            managed.targetGlobal = managed.relayoutFromGlobal;
        }
        baseTargets.push_back(managed.targetGlobal);
    }

    if (!currentManaged->targetMonitor) {
        rebuildVisibleState(currentSelectedWindow, true);
        return;
    }

    const Rect boundsLocal = overviewContentRectForMonitor(currentManaged->targetMonitor, m_state);
    const Rect boundsGlobal =
        makeRect(currentManaged->targetMonitor->m_position.x + boundsLocal.x, currentManaged->targetMonitor->m_position.y + boundsLocal.y, boundsLocal.width, boundsLocal.height);
    if (boundsGlobal.width <= 1.0 || boundsGlobal.height <= 1.0) {
        rebuildVisibleState(currentSelectedWindow, true);
        return;
    }

    const Rect selectedBase = baseTargets[currentManagedIndex];
    const LayoutConfig layoutConfig = loadLayoutConfig();
    const double minGapX = std::max(0.0, layoutConfig.columnSpacing * 0.25);
    const double minGapY = std::max(0.0, layoutConfig.rowSpacing * 0.25);
    const double maxGrowthXPerSide = std::max(0.0, layoutConfig.columnSpacing * 2.0);
    const double maxGrowthYPerSide = std::max(0.0, layoutConfig.rowSpacing * 2.0);
    const double scaleCapByGrowth = maxCenteredScaleForPerSideGrowth(selectedBase, maxGrowthXPerSide, maxGrowthYPerSide);
    const double scaleCapByBounds = maxCenteredScaleForBounds(selectedBase, boundsGlobal);
    const double preferredScale = m_state.windows.size() <= 1 ? 1.0 : SELECTED_WINDOW_LAYOUT_EMPHASIS;
    const double scaleCap = std::max(1.0, std::min({preferredScale, scaleCapByGrowth, scaleCapByBounds}));

    struct RipplePeer {
        std::size_t index = 0;
        Rect        base;
        double      distance = 0.0;
    };

    const double rippleRadius =
        std::max(std::hypot(selectedBase.width, selectedBase.height) * 2.5, std::hypot(boundsGlobal.width, boundsGlobal.height) * 0.55);
    const double pressureCenterX = selectedBase.centerX();
    const double pressureCenterY = selectedBase.centerY();

    std::vector<RipplePeer> peers;
    peers.reserve(m_state.windows.size());
    for (std::size_t index = 0; index < m_state.windows.size(); ++index) {
        const auto& managed = m_state.windows[index];
        if (index == currentManagedIndex || managed.targetMonitor != currentManaged->targetMonitor)
            continue;

        const Rect base = baseTargets[index];
        const double distance = std::hypot(base.centerX() - pressureCenterX, base.centerY() - pressureCenterY);
        peers.push_back({.index = index, .base = base, .distance = distance});
    }

    std::stable_sort(peers.begin(), peers.end(), [](const RipplePeer& lhs, const RipplePeer& rhs) { return lhs.distance < rhs.distance; });

    const auto applyTargets = [&](const std::vector<Rect>& targets) {
        for (std::size_t index = 0; index < m_state.windows.size() && index < targets.size(); ++index)
            m_state.windows[index].targetGlobal = targets[index];
    };

    const auto tryApplyScale = [&](double scale, std::vector<Rect>& outTargets, double& outMaxShift) -> bool {
        outTargets = baseTargets;
        outMaxShift = 0.0;

        const Rect selectedTarget = scaleRectAroundCenter(selectedBase, scale);
        if (!rectFitsInsideBounds(selectedTarget, boundsGlobal))
            return false;
        outTargets[currentManagedIndex] = selectedTarget;

        const double pressureGap = std::max({1.0, minGapX, minGapY});
        const double radialPressure =
            std::max(pressureGap, std::hypot(selectedTarget.width - selectedBase.width, selectedTarget.height - selectedBase.height) * 0.5 + pressureGap);

        std::vector<std::size_t> placed;
        placed.reserve(peers.size() + 1);
        placed.push_back(currentManagedIndex);

        const auto overlapsPlaced = [&](const Rect& target) {
            return std::ranges::any_of(placed, [&](std::size_t obstacleIndex) {
                return rectsOverlap(target, inflateRect(outTargets[obstacleIndex], minGapX, minGapY));
            });
        };

        const auto resolveAlongBearing = [&](Rect target, double dirX, double dirY) -> std::optional<Rect> {
            for (std::size_t pass = 0; pass < 6; ++pass) {
                bool changed = false;
                for (const auto obstacleIndex : placed) {
                    const Rect obstacle = inflateRect(outTargets[obstacleIndex], minGapX, minGapY);
                    if (!rectsOverlap(target, obstacle))
                        continue;

                    if (std::abs(dirX) < 0.001 && std::abs(dirY) < 0.001) {
                        dirX = target.centerX() >= pressureCenterX ? 1.0 : -1.0;
                        dirY = target.centerY() >= pressureCenterY ? 1.0 : -1.0;
                        const double length = std::hypot(dirX, dirY);
                        if (length <= 0.001)
                            return std::nullopt;
                        dirX /= length;
                        dirY /= length;
                    }

                    const auto exitDistance = overlapExitDistanceAlongDirection(target, obstacle, dirX, dirY);
                    if (!exitDistance)
                        continue;

                    target = translateRect(target, dirX * *exitDistance, dirY * *exitDistance);
                    target = clampRectInside(target, boundsGlobal);
                    changed = true;
                }

                if (!changed)
                    break;
            }

            if (!rectFitsInsideBounds(target, boundsGlobal) || overlapsPlaced(target))
                return std::nullopt;

            return target;
        };

        const auto motionDirectionsForPeer = [&](const Rect& base, double radialX, double radialY) {
            std::vector<std::pair<double, double>> directions;
            directions.reserve(10);

            const auto appendDirection = [&](double dx, double dy) {
                const double length = std::hypot(dx, dy);
                if (length <= 0.001)
                    return;
                dx /= length;
                dy /= length;
                const auto duplicate = std::ranges::any_of(directions, [&](const auto& existing) {
                    return std::abs(existing.first - dx) <= 0.001 && std::abs(existing.second - dy) <= 0.001;
                });
                if (!duplicate)
                    directions.push_back({dx, dy});
            };

            const EdgeMotionAffinity affinity = edgeMotionAffinityForRect(base, boundsGlobal);
            const double             awayX = std::abs(radialX) > 0.001 ? radialX : (base.centerX() >= pressureCenterX ? 1.0 : -1.0);
            const double             awayY = std::abs(radialY) > 0.001 ? radialY : (base.centerY() >= pressureCenterY ? 1.0 : -1.0);

            if (affinity.verticalEdge > 0.15) {
                appendDirection(0.0, awayY);
                appendDirection(0.0, 1.0);
                appendDirection(0.0, -1.0);
            }
            if (affinity.horizontalEdge > 0.15) {
                appendDirection(awayX, 0.0);
                appendDirection(1.0, 0.0);
                appendDirection(-1.0, 0.0);
            }

            appendDirection(radialX, radialY);
            appendDirection(radialX, 0.0);
            appendDirection(0.0, radialY);
            appendDirection(1.0, 0.0);
            appendDirection(-1.0, 0.0);
            appendDirection(0.0, 1.0);
            appendDirection(0.0, -1.0);
            return directions;
        };

        for (const auto& peer : peers) {
            const double deltaX = peer.base.centerX() - pressureCenterX;
            const double deltaY = peer.base.centerY() - pressureCenterY;
            double directionX = deltaX;
            double directionY = deltaY;
            const double directionLength = std::hypot(directionX, directionY);
            if (directionLength > 0.001) {
                directionX /= directionLength;
                directionY /= directionLength;
            } else {
                directionX = peer.base.centerX() >= pressureCenterX ? 1.0 : -1.0;
                directionY = peer.base.centerY() >= pressureCenterY ? 1.0 : -1.0;
                const double fallbackLength = std::hypot(directionX, directionY);
                if (fallbackLength <= 0.001)
                    return false;
                directionX /= fallbackLength;
                directionY /= fallbackLength;
            }

            const double influence = clampUnit(1.0 - peer.distance / std::max(1.0, rippleRadius));
            const double easedInfluence = influence * influence;
            Rect target = translateRect(peer.base, directionX * radialPressure * easedInfluence, directionY * radialPressure * easedInfluence);
            target = clampRectInside(target, boundsGlobal);

            std::optional<Rect> resolved;
            double              resolvedScore = std::numeric_limits<double>::max();
            for (const auto& [candidateDirX, candidateDirY] : motionDirectionsForPeer(peer.base, directionX, directionY)) {
                auto candidate = resolveAlongBearing(target, candidateDirX, candidateDirY);
                if (!candidate)
                    candidate = resolveAlongBearing(clampRectInside(peer.base, boundsGlobal), candidateDirX, candidateDirY);
                if (!candidate)
                    continue;

                const double score = edgeAwareMotionDistance2(peer.base, *candidate, boundsGlobal);
                if (!resolved || score < resolvedScore) {
                    resolved = candidate;
                    resolvedScore = score;
                }
            }
            if (!resolved)
                return false;

            outTargets[peer.index] = *resolved;
            placed.push_back(peer.index);
            outMaxShift = std::max(outMaxShift, std::hypot(resolved->centerX() - peer.base.centerX(), resolved->centerY() - peer.base.centerY()));
        }

        return true;
    };

    std::vector<Rect> bestTargets = baseTargets;
    std::vector<Rect> candidateTargets;
    double            bestMaxShift = 0.0;
    double            maxShift = 0.0;
    double            appliedScale = 1.0;

    if (tryApplyScale(scaleCap, bestTargets, bestMaxShift)) {
        appliedScale = scaleCap;
        maxShift = bestMaxShift;
    } else {
        const bool baseResolved = tryApplyScale(1.0, bestTargets, bestMaxShift);
        maxShift = baseResolved ? bestMaxShift : 0.0;

        double low = 1.0;
        double high = scaleCap;
        for (std::size_t iteration = 0; iteration < 12 && high - low > 0.001; ++iteration) {
            const double mid = (low + high) * 0.5;
            double       candidateMaxShift = 0.0;
            if (tryApplyScale(mid, candidateTargets, candidateMaxShift)) {
                appliedScale = mid;
                maxShift = candidateMaxShift;
                bestTargets = candidateTargets;
                low = mid;
            } else {
                high = mid;
            }
        }
    }

    applyTargets(bestTargets);

    for (auto& managed : m_state.windows) {
        if (!rectApproxEqual(managed.relayoutFromGlobal, managed.targetGlobal, 0.5))
            shouldAnimateRelayout = true;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] expand-selected ripple push selected=" << debugWindowLabel(currentSelectedWindow)
            << " peers=" << peers.size()
            << " radius=" << rippleRadius
            << " scale=" << appliedScale
            << " scaleCap=" << scaleCap
            << " growthCap=" << scaleCapByGrowth
            << " boundsCap=" << scaleCapByBounds
            << " minGap=" << minGapX << 'x' << minGapY
            << " maxShift=" << maxShift
            << " selectedTarget=" << rectToString(m_state.windows[currentManagedIndex].targetGlobal);
        debugLog(out.str());
    }

    if (!shouldAnimateRelayout) {
        if (debugLogsEnabled())
            debugLog("[hymission] expand-selected relayout skipped (in-place target unchanged)");
        return;
    }

    m_state.relayoutActive = true;
    m_state.relayoutProgress = 0.0;
    m_state.relayoutStart = {};
    damageOwnedMonitors();
}

void OverviewController::clearPendingWindowGeometryRetry() {
    m_pendingWindowGeometryRetryTarget.reset();
    m_pendingWindowGeometryRetryRemaining = 0;
    m_pendingWindowGeometryRetryScheduled = false;
    ++m_pendingWindowGeometryRetryGeneration;
}

void OverviewController::scheduleVisibleStateRebuild() {
    if (m_visibleStateRebuildScheduled)
        return;

    const bool transitionActiveWhenScheduled = m_workspaceTransition.active;
    if (!g_pEventLoopManager) {
        processScheduledVisibleStateRebuild(transitionActiveWhenScheduled);
        return;
    }

    m_visibleStateRebuildScheduled = true;
    const auto generation = ++m_visibleStateRebuildGeneration;
    g_pEventLoopManager->doLater([this, generation, transitionActiveWhenScheduled] {
        if (g_controller != this || generation != m_visibleStateRebuildGeneration)
            return;

        m_visibleStateRebuildScheduled = false;
        if (!isVisible() || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
            return;

        processScheduledVisibleStateRebuild(transitionActiveWhenScheduled);
    });
}

void OverviewController::processScheduledVisibleStateRebuild(bool transitionActiveWhenScheduled) {
    if (m_workspaceTransition.active) {
        if (activeDirectNiriSingleWorkspaceOverview() && !transitionActiveWhenScheduled) {
            if (debugLogsEnabled())
                debugLog("[hymission] discard stale direct niri rebuild during workspace transition");
            return;
        }

        rebuildVisibleState();
        return;
    }

    if (activeDirectNiriSingleWorkspaceOverview()) {
        const auto liveFocus = Desktop::focusState()->window();
        if (m_state.phase == Phase::Active && m_state.relayoutActive && directNiriEdgeCameraActive() &&
            (!liveFocus || !liveFocus->m_isMapped || !hasManagedWindow(liveFocus))) {
            // A delayed generic rebuild during native leaf -> scroll-past can
            // replace the edge animation target with the barely advanced live box.
            // Let the dedicated direct-Niri edge relayout finish instead.
            damageOwnedMonitors();
            return;
        }

        refreshVisibleStateMetadata();
    } else
        rebuildVisibleState();
}

void OverviewController::scheduleWorkspaceChangeHandling(const PHLWORKSPACE& workspace, OverviewWorkspaceChangeAction action, bool allowExternalTransition) {
    m_pendingWorkspaceChange = workspace;
    m_pendingWorkspaceChangeAction = action;
    m_pendingWorkspaceChangeAllowExternalTransition = allowExternalTransition;

    if (m_workspaceChangeHandlingScheduled)
        return;

    if (!g_pEventLoopManager) {
        if (!insideRenderLifecycle()) {
            if (action == OverviewWorkspaceChangeAction::Rebuild) {
                if (allowExternalTransition && beginExternalOverviewWorkspaceTransition(workspace)) {
                    m_pendingWorkspaceChange.reset();
                    m_pendingWorkspaceChangeAction.reset();
                    m_pendingWorkspaceChangeAllowExternalTransition = false;
                    return;
                }

                if (m_workspaceTransition.active)
                    clearOverviewWorkspaceTransition();
                rebuildVisibleState();
            } else {
                beginClose(CloseMode::Abort);
            }
            m_pendingWorkspaceChange.reset();
            m_pendingWorkspaceChangeAction.reset();
            m_pendingWorkspaceChangeAllowExternalTransition = false;
        }
        return;
    }

    m_workspaceChangeHandlingScheduled = true;
    const auto generation = ++m_workspaceChangeHandlingGeneration;
    g_pEventLoopManager->doLater([this, generation] {
        if (g_controller != this || generation != m_workspaceChangeHandlingGeneration)
            return;

        m_workspaceChangeHandlingScheduled = false;
        const auto workspace = m_pendingWorkspaceChange.lock();
        if (!workspace || !m_pendingWorkspaceChangeAction.has_value()) {
            m_pendingWorkspaceChangeAction.reset();
            m_pendingWorkspaceChangeAllowExternalTransition = false;
            return;
        }
        const auto action = *m_pendingWorkspaceChangeAction;
        const bool allowExternalTransition = m_pendingWorkspaceChangeAllowExternalTransition;
        m_pendingWorkspaceChangeAction.reset();
        m_pendingWorkspaceChangeAllowExternalTransition = false;

        if (insideRenderLifecycle()) {
            scheduleWorkspaceChangeHandling(workspace, action, allowExternalTransition);
            return;
        }

        if (action == OverviewWorkspaceChangeAction::Rebuild) {
            if (allowExternalTransition && beginExternalOverviewWorkspaceTransition(workspace))
                return;

            if (m_workspaceTransition.active)
                clearOverviewWorkspaceTransition();
            rebuildVisibleState();
            return;
        }

        beginClose(CloseMode::Abort);
    });
}

void OverviewController::schedulePendingWindowGeometryRetry(const PHLWINDOW& window) {
    if (!window || !g_pEventLoopManager || !isVisible() || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return;

    const auto pendingTarget = m_pendingWindowGeometryRetryTarget.lock();
    if (pendingTarget != window)
        m_pendingWindowGeometryRetryRemaining = 2;
    else
        m_pendingWindowGeometryRetryRemaining = std::max<std::size_t>(m_pendingWindowGeometryRetryRemaining, 2);
    m_pendingWindowGeometryRetryTarget = window;

    if (m_pendingWindowGeometryRetryScheduled)
        return;

    m_pendingWindowGeometryRetryScheduled = true;
    const auto generation = ++m_pendingWindowGeometryRetryGeneration;
    g_pEventLoopManager->doLater([this, generation] {
        if (g_controller != this || generation != m_pendingWindowGeometryRetryGeneration)
            return;

        m_pendingWindowGeometryRetryScheduled = false;

        if (!isVisible() || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle) {
            clearPendingWindowGeometryRetry();
            return;
        }

        const auto window = m_pendingWindowGeometryRetryTarget.lock();
        if (!window || !windowMatchesOverviewScope(window, m_state, false)) {
            clearPendingWindowGeometryRetry();
            return;
        }

        if (hasManagedWindow(window)) {
            clearPendingWindowGeometryRetry();
            return;
        }

        if (m_pendingWindowGeometryRetryRemaining == 0) {
            clearPendingWindowGeometryRetry();
            return;
        }

        --m_pendingWindowGeometryRetryRemaining;
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] retry overview rebuild for pending window geometry target=" << debugWindowLabel(window)
                << " retriesLeft=" << m_pendingWindowGeometryRetryRemaining;
            debugLog(out.str());
        }

        rebuildVisibleState();
        if (!isVisible()) {
            clearPendingWindowGeometryRetry();
            return;
        }

        if (!windowMatchesOverviewScope(window, m_state, false) || hasManagedWindow(window) || windowHasUsableStateGeometry(window) ||
            m_pendingWindowGeometryRetryRemaining == 0) {
            clearPendingWindowGeometryRetry();
            return;
        }

        schedulePendingWindowGeometryRetry(window);
    });
}

void OverviewController::updatePendingWindowGeometryRetry(const PHLWINDOW& window) {
    if (!window)
        return;

    const auto pendingTarget = m_pendingWindowGeometryRetryTarget.lock();
    if (!isVisible() || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle) {
        if (pendingTarget == window)
            clearPendingWindowGeometryRetry();
        return;
    }

    if (!windowMatchesOverviewScope(window, m_state, false) || hasManagedWindow(window) || windowHasUsableStateGeometry(window)) {
        if (pendingTarget == window)
            clearPendingWindowGeometryRetry();
        return;
    }

    schedulePendingWindowGeometryRetry(window);
}

bool OverviewController::matchesPendingLiveFocusWorkspaceChange(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return false;

    const auto pendingTarget = m_pendingLiveFocusWorkspaceChangeTarget.lock();
    return pendingTarget && pendingTarget->m_workspace && pendingTarget->m_workspace == workspace;
}

void OverviewController::clearPendingStripWorkspaceChange() {
    m_pendingStripWorkspaceChangeTarget.reset();
}

bool OverviewController::matchesPendingStripWorkspaceChange(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return false;

    const auto pendingTarget = m_pendingStripWorkspaceChangeTarget.lock();
    return pendingTarget && pendingTarget == workspace;
}

void OverviewController::clearPostCloseForcedFocus() {
    const auto forcedTarget = m_postCloseForcedFocus.lock();
    if (forcedTarget && g_pInputManager->m_forcedFocus.lock() == forcedTarget)
        g_pInputManager->m_forcedFocus.reset();

    m_postCloseForcedFocus.reset();
    m_postCloseForcedFocusLatched = false;
    m_ignorePostCloseMouseMoveCount = 0;
}

void OverviewController::clearPostCloseDispatcher() {
    m_postCloseDispatcher = PostCloseDispatcher::None;
    m_postCloseDispatcherArgs.clear();
}

void OverviewController::queuePostCloseDispatcher(PostCloseDispatcher dispatcher, std::string args) {
    m_postCloseDispatcher = dispatcher;
    m_postCloseDispatcherArgs = std::move(args);
}

SDispatchResult OverviewController::runHookedDispatcher(PostCloseDispatcher dispatcher, std::string args) {
    DispatcherHandler original;
    const char*       label = nullptr;
    switch (dispatcher) {
        case PostCloseDispatcher::Fullscreen:
            original = m_fullscreenActiveOriginal;
            label = "fullscreen";
            break;
        case PostCloseDispatcher::FullscreenState:
            original = m_fullscreenStateActiveOriginal;
            label = "fullscreenstate";
            break;
        case PostCloseDispatcher::None:
            return {};
    }

    if (!original)
        return {.success = false, .error = "fullscreen dispatcher hook unavailable"};

    if (!isVisible())
        return original(std::move(args));

    if (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return {};

    if (!selectedWindow())
        return {.success = false, .error = "no selected window in overview"};

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] queue post-close dispatcher " << label << " args=" << args;
        debugLog(out.str());
    }

    queuePostCloseDispatcher(dispatcher, std::move(args));
    beginClose(CloseMode::ActivateSelection);
    return {};
}

void OverviewController::setFullscreenRenderOverride(bool suppress) {
    if (suppress) {
        if (!m_state.fullscreenOverrideActive)
            m_state.fullscreenOverrideActive = true;

        for (const auto& backup : m_state.fullscreenBackups) {
            if (!backup.workspace)
                continue;

            backup.workspace->m_hasFullscreenWindow = false;
            backup.workspace->m_fullscreenMode = FSMODE_NONE;
            if (const auto workspaceMonitor = backup.workspace->m_monitor.lock())
                workspaceMonitor->m_solitaryClient.reset();
        }

        return;
    }

    if (!m_state.fullscreenOverrideActive)
        return;

    for (const auto& backup : m_state.fullscreenBackups) {
        if (!backup.workspace)
            continue;

        backup.workspace->m_hasFullscreenWindow = backup.hadFullscreenWindow;
        backup.workspace->m_fullscreenMode = backup.fullscreenMode;
        if (const auto workspaceMonitor = backup.workspace->m_monitor.lock())
            workspaceMonitor->m_solitaryClient.reset();
    }

    m_state.fullscreenOverrideActive = false;
}

bool OverviewController::transformBoxForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor, CBox& box, bool scaled) const {
    const auto transform = windowTransformFor(window, monitor);
    if (!transform)
        return false;

    const double monitorScale = scaled ? monitor->m_scale : 1.0;
    const Rect actual = scaled ? makeRect((transform->actualGlobal.x - monitor->m_position.x) * monitorScale, (transform->actualGlobal.y - monitor->m_position.y) * monitorScale,
                                          transform->actualGlobal.width * monitorScale, transform->actualGlobal.height * monitorScale)
                               : transform->actualGlobal;
    const Rect target = scaled ? makeRect((transform->targetGlobal.x - monitor->m_position.x) * monitorScale, (transform->targetGlobal.y - monitor->m_position.y) * monitorScale,
                                          transform->targetGlobal.width * monitorScale, transform->targetGlobal.height * monitorScale)
                               : transform->targetGlobal;

    box.x = target.x + (box.x - actual.x) * transform->scaleX;
    box.y = target.y + (box.y - actual.y) * transform->scaleY;
    box.width = std::max(1.0, box.width * transform->scaleX);
    box.height = std::max(1.0, box.height * transform->scaleY);
    return true;
}

CRegion OverviewController::transformRegionForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor, const CRegion& region, bool scaled) const {
    CRegion transformed;
    bool    changed = false;

    region.forEachRect([&](const pixman_box32_t& rect) {
        CBox box{
            static_cast<double>(rect.x1),
            static_cast<double>(rect.y1),
            static_cast<double>(rect.x2 - rect.x1),
            static_cast<double>(rect.y2 - rect.y1),
        };

        if (transformBoxForWindow(window, monitor, box, scaled))
            changed = true;

        transformed.add(box);
    });

    return changed ? transformed : region.copy();
}

void OverviewController::beginOpen(const PHLMONITOR& monitor, ScopeOverride requestedScope) {
    clearDirectNiriCloseFinalRenderFrames(this);
    setDamageTrackingOverride(true);
    setAnimationsEnabledOverride(false);
    const bool freshOpen = !isVisible();
    const double fromVisual = freshOpen ? 0.0 : visualProgress();
    m_overviewVisibilityAnimation.reset();
    m_overviewVisibilityAnimationConfig.reset();
    clearToggleSwitchSession();
    clearPostCloseCursorShapeResetTimer();
    clearPendingDeferredOpen();
    m_postCloseOpenDebounceScope.reset();
    m_postCloseOpenDebounceUntil = {};

    const auto buildStart = std::chrono::steady_clock::now();
    if (freshOpen)
        resetDirectNiriWorkspaceLanes();
    clearOverviewWorkspaceTransition();
    m_workspaceSwipeGesture = {};
    m_pendingSwapColumnRelayoutCommitWorkspace.reset();
    m_swapColumnBackendPreviewFreezeWorkspace.reset();
    m_swapColumnBackendPreviewFreezeUntil = {};
    m_swapColumnBackendPreviewFrozenLayout.clear();
    recordWindowActivation(Desktop::focusState()->window());
    closeActiveSpecialWorkspaces();
    const auto preferredSelectedWindow = expandSelectedWindowEnabled() ? Desktop::focusState()->window() : PHLWINDOW{};
    State next = buildState(monitor, requestedScope, {}, false, false, preferredSelectedWindow);
    if (next.windows.empty() && next.stripEntries.empty() && !next.ownerMonitor) {
        setDamageTrackingOverride(false);
        return;
    }

    if (!activateHooks()) {
        setDamageTrackingOverride(false);
        return;
    }

    restoreOverviewRenderState();
    clearPendingWindowGeometryRetry();
    m_visibleStateRebuildScheduled = false;
    ++m_visibleStateRebuildGeneration;
    clearPostCloseForcedFocus();
    clearPostCloseDispatcher();
    m_lastLayoutSelectedWindow.reset();
    m_queuedOverviewSelectionTarget.reset();
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewSelectionCenterCursor = false;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusCenterCursor = false;
    m_pendingLiveFocusWorkspaceChangeTarget.reset();
    m_pendingWorkspaceChange.reset();
    m_pendingWorkspaceChangeAction.reset();
    m_workspaceChangeHandlingScheduled = false;
    ++m_workspaceChangeHandlingGeneration;
    clearPendingStripWorkspaceChange();
    clearStripWindowDragState();
    m_primaryButtonPressed = false;
    next.phase = Phase::Opening;
    next.animationProgress = 0.0;
    next.animationFromVisual = fromVisual;
    next.animationToVisual = 1.0;
    next.animationStart = {};
    m_deactivatePending = false;
    carryOverWorkspaceStripSnapshots(next, m_state);
    m_state = std::move(next);
    g_openOverviewLayoutConfigSignatures[this] = layoutAffectingConfigSignature(m_handle);
    if (m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state)) {
        armOverviewOpenInputBarrier(DIRECT_NIRI_OPEN_INPUT_BLOCK_FALLBACK);
        armOverviewHeavyEditInputBarrier(DIRECT_NIRI_OPEN_INPUT_BLOCK_FALLBACK + DIRECT_NIRI_HEAVY_EDIT_EXTRA_DELAY);
        const auto openDispatcherBlockUntil = std::chrono::steady_clock::now() + DIRECT_NIRI_OPEN_DISPATCHER_BLOCK_DURATION;
        if (niri_scrolling_detail::workspaceSwitchDispatcherBlockUntil == std::chrono::steady_clock::time_point{} ||
            openDispatcherBlockUntil > niri_scrolling_detail::workspaceSwitchDispatcherBlockUntil)
            niri_scrolling_detail::workspaceSwitchDispatcherBlockUntil = openDispatcherBlockUntil;
        niri_scrolling_detail::workspaceSwitchDispatcherBlockRelayout = false;
    }
    if (const auto* placeholder = directNiriEdgeCameraOpenPlaceholder(m_state)) {
        (void)applyNiriScrollingCameraOpenGeometry(*placeholder);
    } else if (const auto selected = selectedWindow(); selected) {
        (void)applyNiriScrollingCameraOpenGeometry(selected);
    } else if (const auto* placeholder = centeredEmptyWorkspacePlaceholder(m_state)) {
        (void)applyNiriScrollingCameraOpenGeometry(*placeholder);
    }
    armOverviewRenderState(m_state);
    m_hoverSelectionAnchorValid = false;
    m_hoverSelectionRetargetBlockedUntil = {};
    m_hoverSelectionRetargetCandidateIndex.reset();
    m_hoverSelectionRetargetCandidateSince = {};
    m_hoverSelectionRetargetCandidatePrimed = false;
    if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
        latchHoverSelectionAnchor(g_pInputManager->getMouseCoordsInternal());
    applyWorkspaceNameOverrides(m_state);
    clearHiddenStripLayerProxies();
    syncNiriWallpaperSnapshots();
    const bool emptyDirectNiriOpen = niriModeAppliesToState(m_state) && m_state.collectionPolicy.onlyActiveWorkspace && m_state.windows.empty();
    if (emptyDirectNiriOpen) {
        // Do not synchronously snapshot layers while opening from a layer
        // surface on an empty scrolling workspace; focus can still belong to a
        // different monitor for that event.  Start the normal wallpaper-layout
        // refresh timer instead so Waybar/hypr-dock stay live until a safe
        // delayed capture exists, then render as zoomed Niri layout proxies.
        clearHiddenStripLayerProxies();
        startNiriWallpaperLayoutLayerRefresh();
    } else {
        syncHiddenStripLayerProxies();
        startNiriWallpaperLayoutLayerRefresh();
    }
    m_cursorShapeResetFrames = WAYBAR_CURSOR_SHAPE_RESET_FRAMES;
    resetStaleClientCursorShape();
    setInputFollowMouseOverride(true);
    setScrollingFollowFocusOverride(true);
    setFullscreenRenderOverride(true);
    refreshWorkspaceStripSnapshots();
    g_pHyprRenderer->m_directScanoutBlocked = true;
    m_postOpenRefreshFrames = 3;
    if (m_pendingOverviewSurfaceFeedbackFrames > 0) {
        m_overviewSurfaceFeedbackFrames = std::max(m_overviewSurfaceFeedbackFrames, m_pendingOverviewSurfaceFeedbackFrames);
        m_postOpenRefreshFrames = std::max(m_postOpenRefreshFrames, m_pendingOverviewSurfaceFeedbackFrames);
        if (workspaceStripEnabled(m_state)) {
            m_stripSnapshotSurfaceFeedbackFrames = std::max(m_stripSnapshotSurfaceFeedbackFrames, m_pendingOverviewSurfaceFeedbackFrames);
            m_stripSnapshotsDirty = true;
            scheduleWorkspaceStripSnapshotRefresh();
        }
        m_pendingOverviewSurfaceFeedbackFrames = 0;
    }
    if (!m_suppressInitialHoverUpdate)
        updateHoveredFromPointer(false, false, false, false, "opening-complete");

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] beginOpen monitor=" << m_state.ownerMonitor->m_name << " windows=" << m_state.windows.size() << " fromVisual=" << fromVisual
            << " buildMs=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count()
            << " scope=";
        switch (m_state.collectionPolicy.requestedScope) {
            case ScopeOverride::Default:
                out << "default";
                break;
            case ScopeOverride::OnlyCurrentWorkspace:
                out << "onlycurrentworkspace";
                break;
            case ScopeOverride::ForceAll:
                out << "forceall";
                break;
        }
        out << " monitors=" << m_state.participatingMonitors.size() << " workspaces=" << m_state.managedWorkspaces.size() << " fullscreenBackups="
            << m_state.fullscreenBackups.size();
        debugLog(out.str());
        logOverviewLayoutState("beginOpen", m_state);
        if (const auto selected = selectedWindow(); selected && selected->m_workspace && isScrollingWorkspace(selected->m_workspace))
            logScrollingWorkspaceSpotState("beginOpen", selected->m_workspace, selected);
    }

    damageOwnedMonitors();
}

bool OverviewController::retargetGestureScope(ScopeOverride requestedScope) {
    PHLMONITOR monitor = m_state.ownerMonitor;
    if (!monitor)
        monitor = g_pCompositor->getMonitorFromCursor();
    if (!monitor)
        return false;

    beginOpen(monitor, requestedScope);
    return isVisible() && m_state.collectionPolicy.requestedScope == requestedScope;
}

void OverviewController::beginClose(CloseMode mode, std::optional<double> fromVisualOverride, bool deferFullscreenMutations) {
    if (!isVisible())
        return;

    clearDirectNiriCloseFinalRenderFrames(this);

    if (mode != CloseMode::Abort && (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle))
        return;

    if (mode == CloseMode::Abort && m_state.phase == Phase::Closing)
        return;

    const ScopedFlag beginCloseGuard(m_beginCloseInProgress);
    if (mode != CloseMode::Abort) {
        m_pendingWorkspaceTransitionRequests.clear();
        // Don't clear m_pendingEditDispatchers here if a workspace transition is active
        // and will be committed - let the commit process handle them.
        if (!m_workspaceTransition.active)
            m_pendingEditDispatchers.clear();
    }

    const WORKSPACEID authoritativeCloseWorkspaceId = authoritativeOverviewWorkspaceId(
        m_workspaceTransition.active,
        m_workspaceTransition.targetWorkspaceId,
        m_state.ownerWorkspace ? m_state.ownerWorkspace->m_id : WORKSPACE_INVALID);
    std::vector<std::pair<PHLWINDOW, Rect>> transitionClosePreviewRects;
    if (mode != CloseMode::Abort && m_workspaceTransition.active) {
        transitionClosePreviewRects.reserve(m_workspaceTransition.targetState.windows.size());
        for (const auto& managed : m_workspaceTransition.targetState.windows) {
            if (!managed.window)
                continue;
            if (const auto rect = workspaceTransitionRectForWindow(managed.window); rect)
                transitionClosePreviewRects.emplace_back(managed.window, *rect);
        }
    }

    if (m_workspaceTransition.active) {
        if (mode == CloseMode::Abort) {
            clearOverviewWorkspaceTransition();
        } else {
            // Exiting while a niri single-workspace overview workspace switch is still
            // animating must land on the destination workspace, not the workspace that
            // was active when the overview opened. The old close() path discarded the
            // pending transition before resolving pendingExitFocus/pendingExitWorkspace,
            // so toggle/Escape could close against the stale source state. Commit the
            // transition first and let the normal exit resolution use the rebuilt target
            // state. Use forceSync=true to ensure the commit completes before we resolve
            // exit focus/workspace below.
            commitOverviewWorkspaceTransition(false, true);
            if (!isVisible() || m_state.phase == Phase::Inactive)
                return;
        }
    }

    if (mode != CloseMode::Abort && authoritativeCloseWorkspaceId != WORKSPACE_INVALID) {
        const auto committedWorkspace = g_pCompositor->getWorkspaceByID(authoritativeCloseWorkspaceId);
        if (committedWorkspace)
            m_state.ownerWorkspace = committedWorkspace;
    }

    const double fromVisual = fromVisualOverride.value_or(visualProgress());
    m_overviewVisibilityAnimation.reset();
    m_overviewVisibilityAnimationConfig.reset();
    clearToggleSwitchSession();
    if (mode != CloseMode::Abort)
        enforceDirectNiriExitFocusGuard();
    if (mode != CloseMode::Abort)
        reconcileNiriCenteredSelectionForExit();
    if (mode != CloseMode::Abort)
        freezeDirectNiriTwoColumnExitPreviewTargets();

    clearPendingWindowGeometryRetry();
    m_visibleStateRebuildScheduled = false;
    ++m_visibleStateRebuildGeneration;

    if (mode == CloseMode::Abort)
        clearPostCloseDispatcher();

    m_queuedOverviewSelectionTarget.reset();
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewSelectionCenterCursor = false;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusCenterCursor = false;
    m_pendingSwapColumnRelayoutCommitWorkspace.reset();
    m_swapColumnBackendPreviewFreezeWorkspace.reset();
    m_swapColumnBackendPreviewFreezeUntil = {};
    m_swapColumnBackendPreviewFrozenLayout.clear();

    if (!transitionClosePreviewRects.empty()) {
        for (auto& managed : m_state.windows) {
            const auto previous = std::find_if(transitionClosePreviewRects.begin(), transitionClosePreviewRects.end(),
                                               [&](const auto& candidate) { return candidate.first == managed.window; });
            if (previous == transitionClosePreviewRects.end())
                continue;
            managed.targetGlobal = previous->second;
            managed.relayoutFromGlobal = previous->second;
        }
        m_state.relayoutActive = false;
        m_state.relayoutProgress = 1.0;
        m_state.relayoutStart = {};
    } else if (m_state.phase == Phase::Active && m_state.collectionPolicy.onlyActiveWorkspace && usesDirectNiriScrollingOverview(m_state)) {
        for (auto& managed : m_state.windows) {
            if (const auto liveRect = livePreviewRectForManagedWindow(managed); liveRect) {
                managed.targetGlobal = *liveRect;
                managed.relayoutFromGlobal = *liveRect;
            }
        }
        m_state.relayoutActive = false;
        m_state.relayoutProgress = 1.0;
        m_state.relayoutStart = {};
    }

    if (m_state.phase == Phase::Active && m_state.relayoutActive) {
        for (auto& managed : m_state.windows) {
            managed.targetGlobal = currentPreviewRect(managed);
            managed.relayoutFromGlobal = managed.targetGlobal;
        }
        m_state.relayoutActive = false;
        m_state.relayoutProgress = 1.0;
        m_state.relayoutStart = {};
    }

    m_state.pendingExitWorkspace = resolveExitWorkspace(mode);
    m_state.pendingExitFocus = resolveExitFocus(mode);
    m_state.closeMode = mode;
    m_state.settleStableFrames = 0;
    m_state.settleHasSample = false;
    m_state.settleStart = {};
    m_state.exitFullscreenReapplied = false;
    m_state.deferredFullscreenWorkspaceClear = false;
    m_state.deferredHiddenFullscreenReapply = false;
    m_deactivatePending = false;
    m_cursorShapeResetFrames = 0;
    resetStaleClientCursorShape();
    clearNiriWallpaperLayoutLayerRefresh();
    const bool directNiriWallpaperClose = niriWallpaperZoomAppliesToState(m_state) && niriModeAppliesToState(m_state) &&
        m_state.collectionPolicy.onlyActiveWorkspace;
    if (directNiriWallpaperClose) {
        // Use the same proxy lifetime as the stable no-window path.  Window-backed
        // workspaces still have a wallpaper viewport; clearing its layout-layer
        // proxy at close exposes the real full-size layer while the viewport is
        // still zooming, which reads as a final 5–10% snap.
        syncNiriWallpaperLayoutLayerProxies();
        std::erase_if(m_hiddenStripLayerProxies, [](const HiddenStripLayerProxy& proxy) { return !proxy.niriWallpaperLayoutLayer; });
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] beginClose retain niri wallpaper viewport proxies windows=" << m_state.windows.size()
                << " proxies=" << m_hiddenStripLayerProxies.size();
            debugLog(out.str());
        }
    } else {
        clearHiddenStripLayerProxies();
        syncHiddenStripLayerProxies();
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] beginClose monitor=" << (m_state.ownerMonitor ? m_state.ownerMonitor->m_name : "?") << " fromVisual=" << fromVisual << " mode=";
        switch (mode) {
            case CloseMode::Normal:
                out << "normal";
                break;
            case CloseMode::ActivateSelection:
                out << "activate";
                break;
            case CloseMode::Abort:
                out << "abort";
                break;
        }
        if (m_state.pendingExitFocus)
            out << " pendingExitFocus=" << debugWindowLabel(m_state.pendingExitFocus);
        else
            out << " pendingExitFocus=<null>";
        if (m_state.pendingExitWorkspace)
            out << " pendingExitWorkspace=" << debugWorkspaceLabel(m_state.pendingExitWorkspace);
        else
            out << " pendingExitWorkspace=<null>";
        if (m_state.focusDuringOverview)
            out << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview);
        else
            out << " focusDuringOverview=<null>";
        if (m_state.focusBeforeOpen)
            out << " focusBeforeOpen=" << debugWindowLabel(m_state.focusBeforeOpen);
        else
            out << " focusBeforeOpen=<null>";
        if (m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size())
            out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        else
            out << " hovered=<null>";
        if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
            out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        else
            out << " selected=<null>";
        const auto activeWindow = Desktop::focusState()->window();
        if (activeWindow)
            out << " activeBeforeClose=" << debugWindowLabel(activeWindow);
        else
            out << " activeBeforeClose=<null>";
        if (m_state.pendingExitFocus && m_state.pendingExitFocus->m_workspace)
            out << " pendingExitFocusWorkspace=" << debugWorkspaceLabel(m_state.pendingExitFocus->m_workspace);
        else
            out << " pendingExitFocusWorkspace=<null>";
        if (m_state.focusBeforeOpen && m_state.focusBeforeOpen->m_workspace)
            out << " focusBeforeOpenWorkspace=" << debugWorkspaceLabel(m_state.focusBeforeOpen->m_workspace);
        else
            out << " focusBeforeOpenWorkspace=<null>";
        out << " ownerWorkspace=" << debugWorkspaceLabel(m_state.ownerWorkspace);
        debugLog(out.str());
    }

    if (mode != CloseMode::Abort && !(m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state)))
        setAnimationsEnabledOverride(true);
    else
        setAnimationsEnabledOverride(false);

    const bool needsDeferredFullscreenClear =
        mode != CloseMode::Abort && deferFullscreenMutations && shouldClearWorkspaceFullscreenForExitTarget(m_state.pendingExitFocus);
    const bool clearedFullscreen =
        mode != CloseMode::Abort && !deferFullscreenMutations && clearWorkspaceFullscreenForExitTarget(m_state.pendingExitFocus);
    const auto* pendingFullscreenBackup = fullscreenBackupForWindow(m_state.pendingExitFocus);
    const bool shouldReapplyOriginalFullscreen = mode != CloseMode::Abort && m_state.pendingExitFocus && pendingFullscreenBackup &&
        m_state.pendingExitFocus == pendingFullscreenBackup->originalFullscreenWindow && pendingFullscreenBackup->originalFullscreenMode != FSMODE_NONE;
    const bool needsDeferredFullscreenReapply = shouldReapplyOriginalFullscreen && deferFullscreenMutations;
    if (needsDeferredFullscreenClear)
        m_state.deferredFullscreenWorkspaceClear = true;
    if (needsDeferredFullscreenReapply)
        m_state.deferredHiddenFullscreenReapply = true;
    if (shouldReapplyOriginalFullscreen && !deferFullscreenMutations) {
        commitOverviewExitFocus(m_state.pendingExitFocus);
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] beginClose hidden fullscreen reapply " << debugWindowLabel(m_state.pendingExitFocus) << " mode="
                << static_cast<int>(pendingFullscreenBackup->originalFullscreenMode);
            debugLog(out.str());
        }
        if (m_state.pendingExitFocus->m_fullscreenState.internal != FSMODE_NONE)
            g_pCompositor->setWindowFullscreenInternal(m_state.pendingExitFocus, FSMODE_NONE);
        g_pCompositor->setWindowFullscreenInternal(m_state.pendingExitFocus, pendingFullscreenBackup->originalFullscreenMode);
        m_state.exitFullscreenReapplied = true;
    } else if ((needsDeferredFullscreenClear || needsDeferredFullscreenReapply) && debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] beginClose defer fullscreen mutation clear=" << (needsDeferredFullscreenClear ? 1 : 0)
            << " reapply=" << (needsDeferredFullscreenReapply ? 1 : 0);
        debugLog(out.str());
    }
    const bool preferGoalGeometry = shouldPreferGoalExitGeometry(m_state.pendingExitFocus);
    const bool shouldSettle = mode != CloseMode::Abort && m_state.pendingExitFocus &&
        (preferGoalGeometry || clearedFullscreen || m_state.exitFullscreenReapplied || m_state.deferredFullscreenWorkspaceClear ||
         m_state.deferredHiddenFullscreenReapply);
    if (debugLogsEnabled() && m_state.pendingExitFocus) {
        std::ostringstream out;
        out << "[hymission] beginClose geometry preferGoal=" << (preferGoalGeometry ? 1 : 0) << " shouldSettle=" << (shouldSettle ? 1 : 0)
            << " exitFocusChangedWorkspace=" << (exitFocusChangedWorkspace(m_state.pendingExitFocus) ? 1 : 0);
        if (const auto exitMonitor = m_state.pendingExitFocus->m_monitor.lock(); exitMonitor) {
            out << " exitMonitor=" << exitMonitor->m_name;
            out << " activeWorkspaceOnExitMonitor=" << debugWorkspaceLabel(exitMonitor->m_activeWorkspace);
        } else {
            out << " exitMonitor=<null>";
        }
        out << " live=" << rectToString(liveGlobalRectForWindow(m_state.pendingExitFocus));
        out << " goal=" << rectToString(goalGlobalRectForWindow(m_state.pendingExitFocus));
        if (const auto* pendingManaged = managedWindowFor(m_state.pendingExitFocus); pendingManaged)
            out << " preview=" << rectToString(currentPreviewRect(*pendingManaged));
        debugLog(out.str());
    }
    if (shouldSettle) {
        setScrollingFollowFocusOverride(false);
        if (!m_state.exitFullscreenReapplied)
            commitOverviewExitFocus(m_state.pendingExitFocus);
        if (preferGoalGeometry)
            refreshExitLayoutForFocus(m_state.pendingExitFocus);
        const bool appliedNiriCameraExit = preferGoalGeometry && applyNiriScrollingCameraExitGeometry(m_state.pendingExitFocus);
        if (!appliedNiriCameraExit) {
            for (auto& managed : m_state.windows) {
                if (!managed.window || !managed.window->m_isMapped)
                    continue;

                managed.exitGlobal = preferGoalGeometry ? goalGlobalRectForWindow(managed.window) : liveGlobalRectForWindow(managed.window);
            }
        }
        if (debugLogsEnabled() && m_state.pendingExitFocus) {
            if (const auto* pendingManaged = managedWindowFor(m_state.pendingExitFocus)) {
                std::ostringstream out;
                out << "[hymission] beginClose settle target=" << debugWindowLabel(m_state.pendingExitFocus)
                    << " exitGlobal=" << rectToString(pendingManaged->exitGlobal)
                    << " preview=" << rectToString(currentPreviewRect(*pendingManaged));
                debugLog(out.str());
            }
        }
        m_state.phase = Phase::ClosingSettle;
        m_state.animationProgress = 0.0;
        m_state.animationFromVisual = fromVisual;
        m_state.animationToVisual = 0.0;
        m_state.animationStart = {};
        if (debugLogsEnabled())
            debugLog("[hymission] beginClose settle start");
    } else {
        bool appliedPlaceholderCameraExit = false;
        if (auto* placeholder = pendingExitWorkspacePlaceholder())
            appliedPlaceholderCameraExit = applyNiriScrollingCameraExitGeometry(*placeholder);

        if (mode != CloseMode::Abort) {
            if (m_state.pendingExitWorkspace)
                (void)activateWorkspaceForExit(m_state.pendingExitWorkspace);
            else
                commitOverviewExitFocus(m_state.pendingExitFocus);
        }
        m_state.phase = Phase::Closing;
        m_state.animationProgress = 0.0;
        m_state.animationFromVisual = fromVisual;
        m_state.animationToVisual = 0.0;
        m_state.animationStart = {};
        if (!appliedPlaceholderCameraExit) {
            for (auto& managed : m_state.windows)
                managed.exitGlobal = liveGlobalRectForWindow(managed.window);
        }
        if (debugLogsEnabled() && m_state.pendingExitFocus) {
            if (const auto* pendingManaged = managedWindowFor(m_state.pendingExitFocus)) {
                std::ostringstream out;
                out << "[hymission] beginClose immediate target=" << debugWindowLabel(m_state.pendingExitFocus)
                    << " exitGlobal=" << rectToString(pendingManaged->exitGlobal)
                    << " preview=" << rectToString(currentPreviewRect(*pendingManaged));
                debugLog(out.str());
            }
        }
    }

    damageOwnedMonitors();
}

void OverviewController::deactivate() {
    clearDirectNiriCloseFinalRenderFrames(this);
    setDamageTrackingOverride(false);
    const auto monitor = m_state.ownerMonitor;
    const auto ownedMonitors = m_state.participatingMonitors;
    const bool refreshDirectNiriCompositing =
        m_state.collectionPolicy.onlyActiveWorkspace && (usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state));
    const auto fullscreenActiveOriginal = m_fullscreenActiveOriginal;
    const auto fullscreenStateActiveOriginal = m_fullscreenStateActiveOriginal;
    const auto* desiredFullscreenBackup = fullscreenBackupForWindow(m_state.pendingExitFocus);
    const auto originalFullscreenWindow = desiredFullscreenBackup ? desiredFullscreenBackup->originalFullscreenWindow : PHLWINDOW{};
    const auto originalFullscreenMode = desiredFullscreenBackup ? desiredFullscreenBackup->originalFullscreenMode : FSMODE_NONE;
    const auto desiredFocus = m_state.closeMode != CloseMode::Abort && m_state.pendingExitFocus && m_state.pendingExitFocus->m_isMapped ? m_state.pendingExitFocus : PHLWINDOW{};
    const auto desiredWorkspace = m_state.closeMode != CloseMode::Abort && m_state.pendingExitWorkspace ? m_state.pendingExitWorkspace : PHLWORKSPACE{};
    const auto closedScope = m_state.collectionPolicy.requestedScope;
    const auto postCloseDispatcher = desiredFocus ? m_postCloseDispatcher : PostCloseDispatcher::None;
    const auto postCloseDispatcherArgs = desiredFocus ? m_postCloseDispatcherArgs : std::string{};
    const bool shouldDelayRestoreNativeAnimations = m_animationsEnabledOverridden && m_state.closeMode != CloseMode::Abort;
    const bool shouldPreserveExitFocus = desiredFocus && m_inputFollowMouseOverridden && m_inputFollowMouseBackup != 0;
    const bool preferGoalVisiblePoint = shouldPreserveExitFocus && shouldPreferGoalExitGeometry(desiredFocus);
    const auto focusMonitor = desiredFocus ? (previewMonitorForWindow(desiredFocus) ? previewMonitorForWindow(desiredFocus) : desiredFocus->m_monitor.lock()) : PHLMONITOR{};
    const auto visiblePoint = shouldPreserveExitFocus ? visiblePointForWindowOnMonitor(desiredFocus, focusMonitor, preferGoalVisiblePoint) : std::nullopt;
    const bool shouldWarpCursorForExitFocus = visiblePoint && desiredFocus != m_state.focusBeforeOpen;
    clearToggleSwitchSession();
    m_primaryButtonPressed = false;
    m_cursorShapeResetFrames = 0;
    resetStaleClientCursorShape();
    m_hoverSelectionAnchorValid = false;
    m_hoverSelectionRetargetBlockedUntil = {};
    m_hoverSelectionRetargetCandidateIndex.reset();
    m_hoverSelectionRetargetCandidateSince = {};
    m_hoverSelectionRetargetCandidatePrimed = false;
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] deactivate monitor=" << (monitor ? monitor->m_name : "?");
        if (m_state.pendingExitFocus)
            out << " pendingExitFocus=" << debugWindowLabel(m_state.pendingExitFocus);
        else
            out << " pendingExitFocus=<null>";
        out << " closeMode=";
        switch (m_state.closeMode) {
            case CloseMode::Normal:
                out << "normal";
                break;
            case CloseMode::ActivateSelection:
                out << "activate";
                break;
            case CloseMode::Abort:
                out << "abort";
                break;
        }
        if (desiredFocus)
            out << " desiredFocus=" << debugWindowLabel(desiredFocus);
        else
            out << " desiredFocus=<null>";
        if (desiredWorkspace)
            out << " desiredExitWorkspace=" << debugWorkspaceLabel(desiredWorkspace);
        else
            out << " desiredExitWorkspace=<null>";
        if (desiredFocus && desiredFocus->m_workspace)
            out << " desiredWorkspace=" << debugWorkspaceLabel(desiredFocus->m_workspace);
        else
            out << " desiredWorkspace=<null>";
        if (originalFullscreenWindow)
            out << " originalFullscreen=" << debugWindowLabel(originalFullscreenWindow) << " mode=" << static_cast<int>(originalFullscreenMode);
        else
            out << " originalFullscreen=<null>";
        out << " exitFullscreenReapplied=" << (m_state.exitFullscreenReapplied ? 1 : 0);
        out << " shouldPreserveFocus=" << (shouldPreserveExitFocus ? 1 : 0);
        out << " preferGoalVisiblePoint=" << (preferGoalVisiblePoint ? 1 : 0);
        out << " shouldWarpCursor=" << (shouldWarpCursorForExitFocus ? 1 : 0);
        if (desiredFocus) {
            out << " desiredLive=" << rectToString(liveGlobalRectForWindow(desiredFocus));
            out << " desiredGoal=" << rectToString(goalGlobalRectForWindow(desiredFocus));
        }
        const auto activeBefore = Desktop::focusState()->window();
        if (activeBefore)
            out << " activeBeforeDeactivate=" << debugWindowLabel(activeBefore);
        else
            out << " activeBeforeDeactivate=<null>";
        debugLog(out.str());
    }

    clearPostCloseForcedFocus();
    m_lastLayoutSelectedWindow.reset();
    m_queuedOverviewSelectionTarget.reset();
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewSelectionCenterCursor = false;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusCenterCursor = false;
    m_pendingLiveFocusWorkspaceChangeTarget.reset();
    m_pendingSwapColumnRelayoutCommitWorkspace.reset();
    m_swapColumnBackendPreviewFreezeWorkspace.reset();
    m_swapColumnBackendPreviewFreezeUntil = {};
    m_swapColumnBackendPreviewFrozenLayout.clear();
    m_pendingWorkspaceChange.reset();
    m_pendingWorkspaceChangeAction.reset();
    m_workspaceChangeHandlingScheduled = false;
    ++m_workspaceChangeHandlingGeneration;
    clearPendingStripWorkspaceChange();
    clearStripWindowDragState();
    clearHiddenStripLayerProxies();
    clearNiriWallpaperSnapshots();
    clearNiriWallpaperLayoutLayerRefresh();
    restoreOverviewRenderState();
    deactivateHooks();
    setFullscreenRenderOverride(false);
    restoreWorkspaceNameOverrides();
    setScrollingFollowFocusOverride(false);
    g_pHyprRenderer->m_directScanoutBlocked = false;

    if (shouldPreserveExitFocus) {
        g_pInputManager->m_forcedFocus = desiredFocus;
        m_postCloseForcedFocus = desiredFocus;
        m_postCloseForcedFocusLatched = true;
        if (shouldWarpCursorForExitFocus) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] warp cursor to visible exit focus " << debugWindowLabel(desiredFocus) << " point=" << vectorToString(*visiblePoint);
                debugLog(out.str());
            }
            g_pCompositor->warpCursorTo(*visiblePoint);
        }
    }

    if (!shouldPreserveExitFocus) {
        setInputFollowMouseOverride(false);
        m_restoreInputFollowMouseAfterPostClose = false;
    } else {
        m_restoreInputFollowMouseAfterPostClose = true;
    }
    if (desiredFocus && Desktop::focusState()->window() != desiredFocus)
        focusWindowCompat(desiredFocus);
    if (!desiredFocus && desiredWorkspace)
        (void)activateWorkspaceForExit(desiredWorkspace);
    if (!m_state.exitFullscreenReapplied && desiredFocus && desiredFocus == originalFullscreenWindow && originalFullscreenMode != FSMODE_NONE && desiredFocus->m_isMapped) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] fullscreen restore check " << debugWindowLabel(desiredFocus) << " mode=" << static_cast<int>(originalFullscreenMode)
                << " internal=" << static_cast<int>(desiredFocus->m_fullscreenState.internal) << " needsRestore=1";
            debugLog(out.str());
        }
        if (desiredFocus->m_fullscreenState.internal != FSMODE_NONE)
            g_pCompositor->setWindowFullscreenInternal(desiredFocus, FSMODE_NONE);
        g_pCompositor->setWindowFullscreenInternal(desiredFocus, originalFullscreenMode);
    }
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] deactivate result active=";
        const auto activeAfter = Desktop::focusState()->window();
        if (activeAfter)
            out << debugWindowLabel(activeAfter);
        else
            out << "<null>";
        switch (postCloseDispatcher) {
            case PostCloseDispatcher::None:
                out << " postCloseDispatcher=<none>";
                break;
            case PostCloseDispatcher::Fullscreen:
                out << " postCloseDispatcher=fullscreen";
                break;
            case PostCloseDispatcher::FullscreenState:
                out << " postCloseDispatcher=fullscreenstate";
                break;
        }
        debugLog(out.str());
    }
    clearPostCloseDispatcher();
    m_deactivatePending = false;
    m_deactivateScheduled = false;
    m_cursorShapeResetFrames = 0;
    m_visibleStateRebuildScheduled = false;
    ++m_visibleStateRebuildGeneration;
    m_postOpenRefreshFrames = 0;
    niri_scrolling_detail::overviewOpenInputBlockUntil = {};
    niri_scrolling_detail::overviewHeavyEditInputBlockUntil = {};
    clearPendingWindowGeometryRetry();
    clearOverviewWorkspaceTransition();
    if (desiredFocus && desiredFocus->m_workspace)
        normalizeDirectNiriWorkspaceActivation(desiredFocus->m_workspace);
    else if (desiredWorkspace)
        normalizeDirectNiriWorkspaceActivation(desiredWorkspace);
    m_workspaceSwipeGesture = {};
    m_stripSnapshotsDirty = false;
    m_stripSnapshotRefreshScheduled = false;
    clearWorkspaceStripSnapshotRefreshTimer();
    m_stripSnapshotSurfaceFeedbackFrames = 0;
    m_overviewSurfaceFeedbackFrames = 0;
    if (m_surfaceFeedbackOverrideActive) {
        g_pHyprRenderer->m_bBlockSurfaceFeedback = m_surfaceFeedbackOverrideBackup;
        m_surfaceFeedbackOverrideActive = false;
    }
    m_lastStripThemeColorValid = false;
    resetDirectNiriWorkspaceLanes();
    m_state = {};
    for (const auto& ownedMonitor : ownedMonitors) {
        if (!ownedMonitor)
            continue;
        if (refreshDirectNiriCompositing) {
            ownedMonitor->m_blurFBDirty = true;
            ownedMonitor->m_forceFullFrames = std::max(ownedMonitor->m_forceFullFrames, 3);
        }
        g_pHyprRenderer->damageMonitor(ownedMonitor);
        g_pCompositor->scheduleFrameForMonitor(ownedMonitor);
    }
    if (monitor && std::ranges::find(ownedMonitors, monitor) == ownedMonitors.end()) {
        if (refreshDirectNiriCompositing) {
            monitor->m_blurFBDirty = true;
            monitor->m_forceFullFrames = std::max(monitor->m_forceFullFrames, 3);
        }
        g_pHyprRenderer->damageMonitor(monitor);
        g_pCompositor->scheduleFrameForMonitor(monitor);
    }

    switch (postCloseDispatcher) {
        case PostCloseDispatcher::Fullscreen:
            if (fullscreenActiveOriginal)
                fullscreenActiveOriginal(postCloseDispatcherArgs);
            break;
        case PostCloseDispatcher::FullscreenState:
            if (fullscreenStateActiveOriginal)
                fullscreenStateActiveOriginal(postCloseDispatcherArgs);
            break;
        case PostCloseDispatcher::None:
            break;
    }

    if (shouldDelayRestoreNativeAnimations)
        setAnimationsEnabledOverride(true, NATIVE_ANIMATION_DISABLE_DURATION);
    else
        setAnimationsEnabledOverride(false);

    schedulePostCloseCursorShapeReset();
    armPostCloseOpenDebounce(closedScope);
}

void OverviewController::scheduleDeactivate() {
    if (m_deactivateScheduled || !g_pEventLoopManager)
        return;

    m_deactivateScheduled = true;
    g_pEventLoopManager->doLater([this] {
        if (g_controller != this)
            return;

        m_deactivateScheduled = false;
        if (!m_deactivatePending || !isVisible())
            return;

        if (g_pHyprOpenGL && g_pHyprRenderer->m_renderData.pMonitor) {
            scheduleDeactivate();
            return;
        }

        if (debugLogsEnabled())
            debugLog("[hymission] deferred deactivate");
        deactivate();
    });
}

void OverviewController::damageOwnedMonitors() const {
    for (const auto& monitor : ownedMonitors()) {
        g_pHyprRenderer->damageMonitor(monitor);
        g_pCompositor->scheduleFrameForMonitor(monitor);
    }
}

void OverviewController::updateAnimation() {
    if (m_state.phase == Phase::ClosingSettle) {
        const auto now = std::chrono::steady_clock::now();
        if (m_state.settleStart == std::chrono::steady_clock::time_point{}) {
            m_state.settleStart = now;
            if (debugLogsEnabled())
                debugLog("[hymission] close settle start");
        }

        if (m_state.deferredFullscreenWorkspaceClear || m_state.deferredHiddenFullscreenReapply) {
            bool appliedDeferredFullscreenMutation = false;

            if (m_state.deferredFullscreenWorkspaceClear) {
                m_state.deferredFullscreenWorkspaceClear = false;
                if (clearWorkspaceFullscreenForExitTarget(m_state.pendingExitFocus)) {
                    appliedDeferredFullscreenMutation = true;
                    if (debugLogsEnabled())
                        debugLog("[hymission] close settle applied deferred fullscreen clear");
                }
            }

            if (m_state.deferredHiddenFullscreenReapply) {
                m_state.deferredHiddenFullscreenReapply = false;
                const auto* pendingFullscreenBackup = fullscreenBackupForWindow(m_state.pendingExitFocus);
                const bool shouldReapplyOriginalFullscreen = m_state.pendingExitFocus && m_state.pendingExitFocus->m_isMapped && pendingFullscreenBackup &&
                    m_state.pendingExitFocus == pendingFullscreenBackup->originalFullscreenWindow && pendingFullscreenBackup->originalFullscreenMode != FSMODE_NONE;
                if (shouldReapplyOriginalFullscreen) {
                    if (debugLogsEnabled()) {
                        std::ostringstream out;
                        out << "[hymission] close settle apply deferred fullscreen reapply " << debugWindowLabel(m_state.pendingExitFocus) << " mode="
                            << static_cast<int>(pendingFullscreenBackup->originalFullscreenMode);
                        debugLog(out.str());
                    }
                    if (m_state.pendingExitFocus->m_fullscreenState.internal != FSMODE_NONE)
                        g_pCompositor->setWindowFullscreenInternal(m_state.pendingExitFocus, FSMODE_NONE);
                    g_pCompositor->setWindowFullscreenInternal(m_state.pendingExitFocus, pendingFullscreenBackup->originalFullscreenMode);
                    m_state.exitFullscreenReapplied = true;
                    appliedDeferredFullscreenMutation = true;
                }
            }

            if (appliedDeferredFullscreenMutation) {
                m_state.settleHasSample = false;
                m_state.settleStableFrames = 0;
                damageOwnedMonitors();
                return;
            }
        }

        const bool preferGoalGeometry = shouldPreferGoalExitGeometry(m_state.pendingExitFocus);
        bool stable = m_state.settleHasSample;
        if (preferGoalGeometry && applyNiriScrollingCameraExitGeometry(m_state.pendingExitFocus)) {
            if (m_state.settleHasSample) {
                for (const auto& managed : m_state.windows) {
                    if (!managed.window || !managed.window->m_isMapped) {
                        beginClose(CloseMode::Abort);
                        return;
                    }
                }
            }
        } else {
            for (auto& managed : m_state.windows) {
                if (!managed.window || !managed.window->m_isMapped) {
                    beginClose(CloseMode::Abort);
                    return;
                }

                const Rect sampledGlobal = preferGoalGeometry ? goalGlobalRectForWindow(managed.window) : liveGlobalRectForWindow(managed.window);
                if (m_state.settleHasSample && !rectApproxEqual(managed.exitGlobal, sampledGlobal, CLOSE_SETTLE_EPSILON))
                    stable = false;
                managed.exitGlobal = sampledGlobal;
            }
        }

        if (debugLogsEnabled() && m_state.pendingExitFocus) {
            const auto* pendingManaged = managedWindowFor(m_state.pendingExitFocus);
            std::ostringstream out;
            out << "[hymission] close settle sample preferGoal=" << (preferGoalGeometry ? 1 : 0);
            if (pendingManaged) {
                const Rect sampledGlobal = preferGoalGeometry ? goalGlobalRectForWindow(m_state.pendingExitFocus) : liveGlobalRectForWindow(m_state.pendingExitFocus);
                out << " target=" << debugWindowLabel(m_state.pendingExitFocus)
                    << " storedExit=" << rectToString(pendingManaged->exitGlobal)
                    << " sampled=" << rectToString(sampledGlobal)
                    << " preview=" << rectToString(currentPreviewRect(*pendingManaged));
            }
            if (m_state.pendingExitFocus->m_workspace) {
                out << " pendingWorkspace=" << debugWorkspaceLabel(m_state.pendingExitFocus->m_workspace)
                    << " wsRender=" << vectorToString(m_state.pendingExitFocus->m_workspace->m_renderOffset->value())
                    << " wsGoal=" << vectorToString(m_state.pendingExitFocus->m_workspace->m_renderOffset->goal());
            }
            if (const auto monitor = m_state.pendingExitFocus->m_monitor.lock(); monitor)
                out << " activeWorkspaceOnMonitor=" << debugWorkspaceLabel(monitor->m_activeWorkspace);
            debugLog(out.str());
        }

        if (!m_state.settleHasSample) {
            m_state.settleHasSample = true;
            m_state.settleStableFrames = 0;
            return;
        }

        m_state.settleStableFrames = stable ? (m_state.settleStableFrames + 1) : 0;
        const double settleElapsedMs = std::chrono::duration<double, std::milli>(now - m_state.settleStart).count();
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] close settle stable=" << (stable ? 1 : 0) << " frames=" << m_state.settleStableFrames << " elapsedMs=" << settleElapsedMs;
            debugLog(out.str());
        }

        if (m_state.settleStableFrames >= CLOSE_SETTLE_STABLE_FRAMES || settleElapsedMs >= CLOSE_SETTLE_TIMEOUT_MS) {
            m_state.phase = Phase::Closing;
            m_state.animationProgress = 0.0;
            m_state.animationFromVisual = clampUnit(m_state.animationFromVisual);
            m_state.animationToVisual = 0.0;
            m_state.animationStart = {};
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] close settle complete frames=" << m_state.settleStableFrames << " elapsedMs=" << settleElapsedMs;
                debugLog(out.str());
            }
        }
        return;
    }

    if (m_gestureSession.active)
        return;

    if (m_state.phase == Phase::Active && m_state.relayoutActive) {
        if (m_state.relayoutStart == std::chrono::steady_clock::time_point{}) {
            beginOverviewRelayoutAnimation("update");
            return;
        }

        if (m_relayoutProgressAnimation)
            m_state.relayoutProgress = clampUnit(m_relayoutProgressAnimation->value());
        else
            m_state.relayoutProgress = 1.0;

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] relayout anim t=" << m_state.relayoutProgress;
            debugLog(out.str());
        }

        if (!m_relayoutProgressAnimation || !m_relayoutProgressAnimation->isBeingAnimated() || m_state.relayoutProgress >= 1.0) {
            finishOverviewRelayoutAnimation();
            if (!m_pendingSwapColumnRelayoutCommitWorkspace.lock()) {
                m_swapColumnBackendPreviewFreezeWorkspace.reset();
                m_swapColumnBackendPreviewFreezeUntil = {};
                m_swapColumnBackendPreviewFrozenLayout.clear();
            } else if (debugLogsEnabled()) {
                debugLog("[hymission] keep pending swapcol relayout after animation complete");
            }
            latchHoverSelectionAnchor(g_pInputManager->getMouseCoordsInternal());
            if (debugLogsEnabled())
                debugLog("[hymission] relayout anim complete");
        }
        return;
    }

    if (!isAnimating())
        return;

    if (m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state)) {
        if (m_state.phase == Phase::Closing) {
            if (const auto remainingFinalFrames = consumeDirectNiriCloseFinalRenderFrame(this); remainingFinalFrames) {
                m_state.animationProgress = 1.0;
                m_state.animationFromVisual = 0.0;
                m_state.animationToVisual = 0.0;
                if (*remainingFinalFrames > 0) {
                    if (debugLogsEnabled()) {
                        std::ostringstream out;
                        out << "[hymission] direct niri close final native-geometry frame remaining=" << *remainingFinalFrames;
                        debugLog(out.str());
                    }
                    damageOwnedMonitors();
                    return;
                }

                if (!m_deactivatePending) {
                    m_deactivatePending = true;
                    damageOwnedMonitors();
                    scheduleDeactivate();
                }
                return;
            }
        }

        if (m_state.animationStart == std::chrono::steady_clock::time_point{}) {
            m_state.animationStart = std::chrono::steady_clock::now();
            beginOverviewVisibilityAnimation(m_state.phase == Phase::Opening ? "open" : "close");
        }

        m_state.animationProgress = visualProgress();
        if (m_overviewVisibilityAnimation && m_overviewVisibilityAnimation->isBeingAnimated())
            return;

        finishOverviewVisibilityAnimation();
        if (m_state.phase == Phase::Opening) {
            clearDirectNiriCloseFinalRenderFrames(this);
            m_state.phase = Phase::Active;
            m_state.animationFromVisual = 1.0;
            m_state.animationToVisual = 1.0;
            m_postOpenRefreshFrames = std::max<std::size_t>(m_postOpenRefreshFrames, 3);
            settleOverviewOpenInputBarrier();
            settleOverviewHeavyEditInputBarrier();
            refreshNiriScrollingOverviewAfterFocusDispatcher("opening-complete");
            updateSelectedWindowLayout({});
            updateHoveredFromPointer(false, false, false, false, "begin-open");
            damageOwnedMonitors();
        } else if (m_state.phase == Phase::Closing && !m_deactivatePending) {
            m_state.animationFromVisual = 0.0;
            m_state.animationToVisual = 0.0;
            armDirectNiriCloseFinalRenderFrames(this);
            if (debugLogsEnabled())
                debugLog("[hymission] direct niri close reached native geometry; render final frame before handoff");
            damageOwnedMonitors();
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_state.animationStart == std::chrono::steady_clock::time_point{}) {
        m_state.animationStart = now;
        m_state.animationProgress = 0.0;
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] anim start phase=" << (m_state.phase == Phase::Opening ? "opening" : "closing") << " visual=" << visualProgress();
            debugLog(out.str());
        }
        return;
    }

    const auto elapsed = std::chrono::duration<double, std::milli>(now - m_state.animationStart).count();
    const double duration = m_state.phase == Phase::Opening ? OPEN_DURATION_MS : CLOSE_DURATION_MS;

    m_state.animationProgress = clampUnit(elapsed / duration);
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] anim phase=" << (m_state.phase == Phase::Opening ? "opening" : "closing") << " t=" << m_state.animationProgress
            << " visual=" << visualProgress();
        debugLog(out.str());
    }

    if (m_state.animationProgress < 1.0)
        return;

    if (m_state.phase == Phase::Opening) {
        m_state.phase = Phase::Active;
        m_state.animationProgress = 1.0;
        m_state.animationFromVisual = 1.0;
        m_state.animationToVisual = 1.0;
        m_postOpenRefreshFrames = std::max<std::size_t>(m_postOpenRefreshFrames, 3);
        settleOverviewOpenInputBarrier();
        settleOverviewHeavyEditInputBarrier();
        if (debugLogsEnabled())
            debugLog("[hymission] anim opening complete");
        refreshNiriScrollingOverviewAfterFocusDispatcher("opening-complete");
        updateSelectedWindowLayout({});
        updateHoveredFromPointer(false, false, false, false, "begin-open");
        damageOwnedMonitors();
    } else if (m_state.phase == Phase::Closing) {
        m_state.animationProgress = 1.0;
        m_state.animationFromVisual = 0.0;
        m_state.animationToVisual = 0.0;
        if (!m_deactivatePending) {
            if (debugLogsEnabled())
                debugLog("[hymission] anim closing complete, queue deferred deactivate");
            m_deactivatePending = true;
            scheduleDeactivate();
        }
    }
}

void OverviewController::updateHoveredFromPointer(bool syncSelection, bool syncRealFocus, bool syncScrollingSpot, bool allowSelectionRetarget, const char* source) {
    if (!isVisible())
        return;

    const Vector2D pointer = g_pInputManager->getMouseCoordsInternal();
    const bool draggingWindow = m_draggedWindowIndex.has_value();
    const auto previousHoveredStrip = m_state.hoveredStripIndex;
    const auto previousHovered = m_state.hoveredIndex;
    const auto previousSelected = m_state.selectedIndex;
    const auto previousFocus = m_state.focusDuringOverview;
    const auto now = std::chrono::steady_clock::now();

    m_state.hoveredStripIndex = hitTestStripTarget(pointer.x, pointer.y);
    m_state.hoveredIndex = (draggingWindow || m_state.hoveredStripIndex) ? std::optional<std::size_t>{} : hitTestTarget(pointer.x, pointer.y);

    const bool disableHoverSelection = niriModeAppliesToState(m_state);
    const bool wantsSelectionRetarget =
        !disableHoverSelection && !draggingWindow && syncSelection && m_state.hoveredIndex && focusFollowsMouseEnabled() && allowSelectionRetarget &&
        (!m_state.selectedIndex || *m_state.hoveredIndex != *m_state.selectedIndex);
    const bool wantsImmediateRetarget = wantsSelectionRetarget && syncRealFocus;
    const bool retargetBlockedByRelayout = expandSelectedWindowEnabled() && m_state.relayoutActive && !wantsImmediateRetarget;
    const bool retargetBlockedByCooldown = expandSelectedWindowEnabled() && now < m_hoverSelectionRetargetBlockedUntil && !wantsImmediateRetarget;
    bool retargetLocked = false;
    if (wantsSelectionRetarget && !retargetBlockedByRelayout && !retargetBlockedByCooldown)
        retargetLocked = hoverSelectionRetargetLocked(pointer, m_state.hoveredIndex);

    const bool retargetBlocked = retargetBlockedByRelayout || retargetBlockedByCooldown || retargetLocked;
    const bool canRetargetSelection = wantsSelectionRetarget && !retargetBlocked;
    const bool immediateRetarget = canRetargetSelection && wantsImmediateRetarget;

    if (!wantsSelectionRetarget || immediateRetarget) {
        m_hoverSelectionRetargetCandidateIndex.reset();
        m_hoverSelectionRetargetCandidateSince = {};
        m_hoverSelectionRetargetCandidatePrimed = false;
    } else if (retargetBlocked) {
        m_hoverSelectionRetargetCandidateIndex = m_state.hoveredIndex;
        m_hoverSelectionRetargetCandidateSince = {};
        m_hoverSelectionRetargetCandidatePrimed = false;
    }

    if (immediateRetarget && m_state.hoveredIndex) {
        m_state.selectedIndex = m_state.hoveredIndex;
        const auto nextSelectedWindow = selectedWindow();
        if (nextSelectedWindow) {
            m_state.focusDuringOverview = nextSelectedWindow;
            queueSelectionRetargetDuringOverview(nextSelectedWindow, syncScrollingSpot, source, true);
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] hover retarget immediate queued pointer=" << pointer.x << ',' << pointer.y;
                out << " source=" << (source ? source : "?");
                out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(nextSelectedWindow);
                const auto activeWindow = Desktop::focusState()->window();
                if (activeWindow)
                    out << " active=" << debugWindowLabel(activeWindow);
                else
                    out << " active=<null>";
                debugLog(out.str());
            }
        }
    } else if (canRetargetSelection) {
        const bool candidateNeedsPriming = m_hoverSelectionRetargetCandidateIndex != m_state.hoveredIndex || !m_hoverSelectionRetargetCandidatePrimed ||
            m_hoverSelectionRetargetCandidateSince == std::chrono::steady_clock::time_point{};
        if (candidateNeedsPriming) {
            m_hoverSelectionRetargetCandidateIndex = m_state.hoveredIndex;
            m_hoverSelectionRetargetCandidateSince = now;
            m_hoverSelectionRetargetCandidatePrimed = true;
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] hover retarget dwell start pointer=" << pointer.x << ',' << pointer.y;
                out << " source=" << (source ? source : "?");
                out << " candidate=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
                if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
                    out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
                else
                    out << " selected=<null>";
                out << " dwellMs=" << HOVER_SELECTION_RETARGET_DWELL.count();
                debugLog(out.str());
            }
        } else if (now - m_hoverSelectionRetargetCandidateSince >= HOVER_SELECTION_RETARGET_DWELL) {
            const bool selectionChanged = m_state.selectedIndex != m_state.hoveredIndex;
            m_state.selectedIndex = m_state.hoveredIndex;
            m_hoverSelectionRetargetCandidateIndex.reset();
            m_hoverSelectionRetargetCandidateSince = {};
            m_hoverSelectionRetargetCandidatePrimed = false;
            if (syncRealFocus)
                syncFocusDuringOverviewFromSelection(syncScrollingSpot, source, true);
            else
                m_state.focusDuringOverview = m_state.windows[*m_state.hoveredIndex].window;
            if (selectionChanged && !syncRealFocus)
                latchHoverSelectionAnchor(pointer);
        } else if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] hover retarget dwelling pointer=" << pointer.x << ',' << pointer.y;
            out << " source=" << (source ? source : "?");
            out << " candidate=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
            if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
                out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
            else
                out << " selected=<null>";
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_hoverSelectionRetargetCandidateSince).count();
            out << " elapsedMs=" << elapsed;
            out << " dwellMs=" << HOVER_SELECTION_RETARGET_DWELL.count();
            debugLog(out.str());
        }
    } else if (retargetBlockedByCooldown && allowSelectionRetarget && debugLogsEnabled() && m_state.hoveredIndex && m_state.selectedIndex &&
               *m_state.hoveredIndex != *m_state.selectedIndex) {
        std::ostringstream out;
        out << "[hymission] hover retarget cooling down pointer=" << pointer.x << ',' << pointer.y;
        out << " source=" << (source ? source : "?");
        out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(m_hoverSelectionRetargetBlockedUntil - now).count();
        out << " remainingMs=" << std::max<long long>(remaining, 0);
        debugLog(out.str());
    } else if (retargetBlockedByRelayout && allowSelectionRetarget && debugLogsEnabled() && m_state.hoveredIndex && m_state.selectedIndex &&
               *m_state.hoveredIndex != *m_state.selectedIndex) {
        std::ostringstream out;
        out << "[hymission] hover retarget deferred during relayout pointer=" << pointer.x << ',' << pointer.y;
        out << " source=" << (source ? source : "?");
        out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        out << " relayoutT=" << m_state.relayoutProgress;
        debugLog(out.str());
    } else if (retargetLocked && debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] hover retarget locked pointer=" << pointer.x << ',' << pointer.y
            << " source=" << (source ? source : "?")
            << " anchor=" << vectorToString(m_hoverSelectionAnchorPointer)
            << " threshold=" << HOVER_SELECTION_RETARGET_DISTANCE;
        if (m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size())
            out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        else
            out << " hovered=<null>";
        if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
            out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        else
            out << " selected=<null>";
        debugLog(out.str());
    }

    if (previousHoveredStrip != m_state.hoveredStripIndex || previousHovered != m_state.hoveredIndex || previousSelected != m_state.selectedIndex ||
        previousFocus != m_state.focusDuringOverview) {
        if (previousHoveredStrip != m_state.hoveredStripIndex)
            applyWorkspaceStripCursorShape();

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] hover pointer=" << pointer.x << ',' << pointer.y;
            out << " source=" << (source ? source : "?");
            if (m_state.hoveredStripIndex && *m_state.hoveredStripIndex < m_state.stripEntries.size()) {
                const auto& entry = m_state.stripEntries[*m_state.hoveredStripIndex];
                out << " strip=" << *m_state.hoveredStripIndex << ':' << entry.workspaceId;
                if (entry.workspace)
                    out << ':' << entry.workspace->m_name;
                else if (!entry.workspaceName.empty())
                    out << ':' << entry.workspaceName;
                if (entry.newWorkspaceSlot)
                    out << ":new";
            } else {
                out << " strip=<null>";
            }
            if (m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size())
                out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
            else
                out << " hovered=<null>";
            if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
                out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
            else
                out << " selected=<null>";
            if (m_state.focusDuringOverview)
                out << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview);
            else
                out << " focusDuringOverview=<null>";
            const auto activeWindow = Desktop::focusState()->window();
            if (activeWindow)
                out << " active=" << debugWindowLabel(activeWindow);
            else
                out << " active=<null>";
            debugLog(out.str());
        }
        damageOwnedMonitors();
    }
}

void OverviewController::refreshVisibleStateMetadata(PHLWINDOW preferredSelectedWindow, const PreviewRectSnapshot* relayoutOrigins, const char* relayoutSource) {
    if (!isVisible() || !m_state.ownerMonitor || !m_state.ownerWorkspace || m_workspaceTransition.active)
        return;

    const auto previousState = m_state;
    State next = buildState(m_state.ownerMonitor, m_state.collectionPolicy.requestedScope, {}, false, m_state.suppressWorkspaceStrip, preferredSelectedWindow, false);
    if (next.windows.empty() && next.stripEntries.empty() && next.participatingMonitors.empty()) {
        beginClose(CloseMode::Abort);
        return;
    }

    next.phase = previousState.phase;
    next.animationProgress = previousState.animationProgress;
    next.animationFromVisual = previousState.animationFromVisual;
    next.animationToVisual = previousState.animationToVisual;
    next.animationStart = previousState.animationStart;
    next.settleStableFrames = previousState.settleStableFrames;
    next.settleHasSample = previousState.settleHasSample;
    next.settleStart = previousState.settleStart;
    next.focusBeforeOpen = previousState.focusBeforeOpen;
    next.closeMode = previousState.closeMode;
    next.exitFullscreenReapplied = previousState.exitFullscreenReapplied;
    next.fullscreenOverrideActive = previousState.fullscreenOverrideActive;
    next.pendingExitFocus = windowMatchesOverviewScope(previousState.pendingExitFocus, next, false) ? previousState.pendingExitFocus : PHLWINDOW{};
    next.pendingExitWorkspace = containsHandle(next.managedWorkspaces, previousState.pendingExitWorkspace) ? previousState.pendingExitWorkspace : PHLWORKSPACE{};
    const bool retargetDirectNiriRelayout = usesDirectNiriScrollingOverview(previousState) && (previousState.relayoutActive || relayoutOrigins);
    const auto capturedRelayoutOrigins = retargetDirectNiriRelayout && !relayoutOrigins ? captureCurrentPreviewRects() : PreviewRectSnapshot{};
    const auto* directNiriRetargetOrigins = relayoutOrigins ? relayoutOrigins : &capturedRelayoutOrigins;
    next.relayoutActive = retargetDirectNiriRelayout;
    next.relayoutProgress = retargetDirectNiriRelayout ? previousState.relayoutProgress : 1.0;
    next.relayoutStart = retargetDirectNiriRelayout ? previousState.relayoutStart : std::chrono::steady_clock::time_point{};
    for (auto& backup : next.fullscreenBackups) {
        const auto previous = std::find_if(previousState.fullscreenBackups.begin(), previousState.fullscreenBackups.end(),
                                           [&](const FullscreenWorkspaceBackup& candidate) { return candidate.workspace == backup.workspace; });
        if (previous != previousState.fullscreenBackups.end())
            backup = *previous;
    }

    const auto previousManagedFor = [&](const PHLWINDOW& window) -> const ManagedWindow* {
        const auto it = std::find_if(previousState.windows.begin(), previousState.windows.end(),
                                     [&](const ManagedWindow& candidate) { return candidate.window == window; });
        return it == previousState.windows.end() ? nullptr : &*it;
    };

    next.slots.clear();
    next.slots.reserve(next.windows.size());
    for (auto& managed : next.windows) {
        if (const auto* previous = previousManagedFor(managed.window); previous) {
            managed.targetMonitor = previous->targetMonitor;
            managed.naturalGlobal = previous->naturalGlobal;
            managed.exitGlobal = previous->exitGlobal;
            managed.relayoutFromGlobal = previous->relayoutFromGlobal;
            managed.targetGlobal = previous->targetGlobal;
            managed.slot = previous->slot;
        }
        next.slots.push_back(managed.slot);
    }

    for (auto& placeholder : next.emptyWorkspacePlaceholders) {
        const auto previous = std::find_if(previousState.emptyWorkspacePlaceholders.begin(), previousState.emptyWorkspacePlaceholders.end(),
                                           [&](const EmptyWorkspacePlaceholder& candidate) {
                                               return candidate.monitor == placeholder.monitor && candidate.workspaceId == placeholder.workspaceId &&
                                                   candidate.backingOnly == placeholder.backingOnly;
                                           });
        if (previous == previousState.emptyWorkspacePlaceholders.end())
            continue;

        placeholder.naturalGlobal = previous->naturalGlobal;
        placeholder.exitGlobal = previous->exitGlobal;
        placeholder.targetGlobal = previous->targetGlobal;
        placeholder.relayoutFromGlobal = retargetDirectNiriRelayout ? previous->relayoutFromGlobal : previous->targetGlobal;
    }

    for (const auto& previous : previousState.windows) {
        if (!previous.window || !previous.window->m_fadingOut ||
            std::any_of(next.windows.begin(), next.windows.end(), [&](const ManagedWindow& candidate) { return candidate.window == previous.window; }))
            continue;

        ManagedWindow transient = previous;
        transient.targetGlobal = currentPreviewRect(previous);
        transient.relayoutFromGlobal = transient.targetGlobal;
        transient.exitGlobal = transient.targetGlobal;
        next.transientClosingWindows.push_back(std::move(transient));
    }
    for (const auto& previous : previousState.transientClosingWindows) {
        if (!previous.window || !previous.window->m_fadingOut ||
            std::any_of(next.transientClosingWindows.begin(), next.transientClosingWindows.end(),
                        [&](const ManagedWindow& candidate) { return candidate.window == previous.window; }))
            continue;
        next.transientClosingWindows.push_back(previous);
    }

    const bool nextDirectNiriEdgeCamera = directNiriOwnerEdgeCameraActive(next);
    const bool previousDirectNiriEdgeCamera = directNiriOwnerEdgeCameraActive(previousState);
    const std::string_view relayoutSourceView = relayoutSource ? std::string_view{relayoutSource} : std::string_view{};
    const bool forcedDirectNiriEdgeRelease = usesDirectNiriScrollingOverview(previousState) &&
        relayoutSourceView.find("movecol-edge-release") != std::string_view::npos;
    const auto validRestoredDirectNiriFocus = [&](const PHLWINDOW& window) {
        return window && window->m_isMapped && managedWindowFor(next, window, true);
    };

    PHLWINDOW restoredDirectNiriFocus = preferredSelectedWindow;
    if (!validRestoredDirectNiriFocus(restoredDirectNiriFocus) && !forcedDirectNiriEdgeRelease)
        restoredDirectNiriFocus = Desktop::focusState()->window();

    const bool preserveDirectNiriEdgeCamera = (forcedDirectNiriEdgeRelease || nextDirectNiriEdgeCamera || previousDirectNiriEdgeCamera) &&
        !validRestoredDirectNiriFocus(restoredDirectNiriFocus);
    if (preserveDirectNiriEdgeCamera) {
        // Native scrolling layout has deliberately released window focus while
        // the camera is panning past the strip edge. Do not resurrect the
        // previous overview focus during metadata rebuilds. Once native focus
        // comes back while leaving the edge camera, accept that live focus again.
        next.selectedIndex.reset();
        next.focusDuringOverview.reset();
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] metadata refresh preserved direct niri edge release"
                << " source=" << (relayoutSource ? relayoutSource : "?")
                << " forced=" << (forcedDirectNiriEdgeRelease ? 1 : 0)
                << " nextEdge=" << (nextDirectNiriEdgeCamera ? 1 : 0)
                << " previousEdge=" << (previousDirectNiriEdgeCamera ? 1 : 0)
                << " restored=" << debugWindowLabel(restoredDirectNiriFocus);
            debugLog(out.str());
        }
    } else {
        PHLWINDOW selected = restoredDirectNiriFocus;
        if (!validRestoredDirectNiriFocus(selected) && !previousDirectNiriEdgeCamera)
            selected = previousState.focusDuringOverview;
        if (selected && selected->m_isMapped && selectWindowInState(next, selected))
            next.focusDuringOverview = selected;
    }

    carryOverWorkspaceStripSnapshots(next, previousState);
    restoreOverviewRenderState();
    m_state = std::move(next);
    if (!retargetDirectNiriRelayout)
        m_relayoutProgressAnimation.reset();
    armOverviewRenderState(m_state);
    applyWorkspaceNameOverrides(m_state);

    // Sync scrolling layout focus after state rebuild. The buildState sets
    // focusDuringOverview but the scrolling layout's lastFocusedTarget is
    // not automatically updated. Without this, the visual selection border
    // (driven by focusDuringOverview) and the viewport centering (driven by
    // lastFocusedTarget) become desynchronized.
    if (m_state.focusDuringOverview && m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state) &&
        isScrollingWorkspace(m_state.focusDuringOverview->m_workspace)) {
        if (!shouldPreserveDirectNiriEdgeCamera(m_state.focusDuringOverview)) {
            (void)syncScrollingWorkspaceSpotOnWindow(m_state.focusDuringOverview);
        } else if (debugLogsEnabled()) {
            debugLog("[hymission] metadata refresh preserved edge camera");
        }
    }

    if (retargetDirectNiriRelayout)
        refreshNiriScrollingOverviewAfterLayoutScroll(relayoutSource, directNiriRetargetOrigins);

    syncHiddenStripLayerProxies();
    refreshWorkspaceStripSnapshots();
    updateHoveredFromPointer(false, false, false, false, "metadata-refresh");
    damageOwnedMonitors();
}

void OverviewController::rebuildVisibleState(PHLWINDOW preferredSelectedWindow, bool forceRelayout) {
    if (!isVisible() || !m_state.ownerMonitor || !m_state.ownerWorkspace)
        return;

    if (m_workspaceTransition.active)
        clearOverviewWorkspaceTransition();

    const auto monitor = m_state.ownerMonitor;
    const auto previousOwnerWorkspace = m_state.ownerWorkspace;
    const auto requestedScope = m_state.collectionPolicy.requestedScope;
    const auto previousPhase = m_state.phase;
    const auto previousAnimationProgress = m_state.animationProgress;
    const auto previousAnimationFromVisual = m_state.animationFromVisual;
    const auto previousAnimationToVisual = m_state.animationToVisual;
    const auto previousAnimationStart = m_state.animationStart;
    const auto previousSettleStableFrames = m_state.settleStableFrames;
    const auto previousSettleHasSample = m_state.settleHasSample;
    const auto previousSettleStart = m_state.settleStart;
    const auto previousRelayoutProgress = m_state.relayoutProgress;
    const bool previousRelayoutActive = m_state.relayoutActive;
    const auto previousRelayoutStart = m_state.relayoutStart;
    const auto previousFocusBeforeOpen = m_state.focusBeforeOpen;
    const auto previousPendingExitFocus = m_state.pendingExitFocus;
    const auto previousPendingExitWorkspace = m_state.pendingExitWorkspace;
    const auto previousCloseMode = m_state.closeMode;
    const bool previousExitFullscreenReapplied = m_state.exitFullscreenReapplied;
    const auto previousFullscreenBackups = m_state.fullscreenBackups;
    const bool previousFullscreenOverrideActive = m_state.fullscreenOverrideActive;
    const bool hyprlandOwnedDirectRelayout = usesDirectNiriScrollingOverview(m_state) &&
        (previousPhase == Phase::Opening || previousPhase == Phase::Active);
    
    // When forceRelayout is true (e.g., after queued dispatchers or movecol/swapcol),
    // and we're in niri scrolling mode, use live window positions instead of stale
    // overview preview rects. This prevents the focus/animation from getting stuck
    // on stale positions when the actual Hyprland layout has changed.
    const bool useLiveRectsForRebuild = forceRelayout && usesDirectNiriScrollingOverview(m_state);
    std::vector<std::pair<PHLWINDOW, Rect>> previousPreviewRects;
    previousPreviewRects.reserve(m_state.windows.size() + m_state.transientClosingWindows.size());
    for (const auto& window : m_state.windows) {
        const Rect rect = useLiveRectsForRebuild ? liveGlobalRectForWindow(window.window) : currentPreviewRect(window);
        previousPreviewRects.emplace_back(window.window, rect);
    }
    for (const auto& window : m_state.transientClosingWindows) {
        const Rect rect = useLiveRectsForRebuild ? liveGlobalRectForWindow(window.window) : currentPreviewRect(window);
        previousPreviewRects.emplace_back(window.window, rect);
    }
    std::vector<std::tuple<MONITORID, WORKSPACEID, Rect>> previousPlaceholderRects;
    previousPlaceholderRects.reserve(m_state.emptyWorkspacePlaceholders.size());
    const double relayoutProgress = relayoutVisualProgress();
    for (const auto& placeholder : m_state.emptyWorkspacePlaceholders) {
        if (!placeholder.monitor)
            continue;

        const Rect currentRect = (m_state.phase == Phase::Active && m_state.relayoutActive)
            ? lerpRect(placeholder.relayoutFromGlobal, placeholder.targetGlobal, relayoutProgress) :
            placeholder.targetGlobal;
        previousPlaceholderRects.emplace_back(placeholder.monitor->m_id, placeholder.workspaceId, currentRect);
    }

    const auto closestTiledWindowForRelayoutAnchor = [&](const PHLWINDOW& window) -> PHLWINDOW {
        if (!forceRelayout || !window || !niriModeAppliesToState(m_state))
            return window;

        if (!window->m_pinned && !isFloatingOverviewWindow(window))
            return window;

        const auto workspace = window->m_pinned ? activeLayoutWorkspace() : window->m_workspace;
        if (!workspace || !isScrollingWorkspace(workspace))
            return window;

        const auto* sourceManaged = managedWindowFor(m_state, window, true);
        const Rect sourceRect = sourceManaged ? currentPreviewRect(*sourceManaged) : liveGlobalRectForWindow(window);

        PHLWINDOW best;
        double    bestDistance = std::numeric_limits<double>::infinity();
        for (const auto& candidate : m_state.windows) {
            const auto candidateWindow = candidate.window;
            if (!candidateWindow || candidateWindow == window || !candidateWindow->m_isMapped || candidateWindow->m_workspace != workspace || candidateWindow->m_pinned)
                continue;

            const auto target = candidateWindow->layoutTarget();
            if (!target || target->floating() || isFloatingOverviewWindow(candidateWindow))
                continue;

            const Rect candidateRect = currentPreviewRect(candidate);
            const double dx = candidateRect.centerX() - sourceRect.centerX();
            const double dy = candidateRect.centerY() - sourceRect.centerY();
            const double distance = dx * dx + dy * dy;
            if (!best || distance < bestDistance) {
                best = candidateWindow;
                bestDistance = distance;
            }
        }

        return best ? best : window;
    };

    const auto requestedSelectedWindow = preferredSelectedWindow ? preferredSelectedWindow : (selectedWindow() ? selectedWindow() : Desktop::focusState()->window());
    const auto relayoutSelectedWindow = closestTiledWindowForRelayoutAnchor(requestedSelectedWindow);
    const auto layoutSelectedWindow =
        expandSelectedWindowEnabled() ? (relayoutSelectedWindow ? relayoutSelectedWindow : requestedSelectedWindow) :
        (forceRelayout ? relayoutSelectedWindow : PHLWINDOW{});
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] rebuild request forceRelayout=" << (forceRelayout ? 1 : 0)
            << " preferredSelected=" << debugWindowLabel(preferredSelectedWindow)
            << " layoutSelected=" << debugWindowLabel(layoutSelectedWindow);
        if (const auto activeWindow = Desktop::focusState()->window(); activeWindow)
            out << " active=" << debugWindowLabel(activeWindow);
        else
            out << " active=<null>";
        debugLog(out.str());
    }
    State next = buildState(monitor, requestedScope, {}, false, false, layoutSelectedWindow);
    if (next.windows.empty() && next.stripEntries.empty() && next.participatingMonitors.empty()) {
        beginClose(CloseMode::Abort);
        return;
    }

    if (!m_state.collectionPolicy.onlyActiveWorkspace && previousOwnerWorkspace)
        next.ownerWorkspace = previousOwnerWorkspace;
    const bool carriedFrozenSwapColumnLayout = !hyprlandOwnedDirectRelayout &&
        carryFrozenSwapColumnBackendPreviewLayout(next, activeLayoutWorkspace(), forceRelayout ? "rebuild-visible-forced" : "rebuild-visible");

    next.phase = previousPhase;
    next.animationProgress = previousAnimationProgress;
    next.animationFromVisual = previousAnimationFromVisual;
    next.animationToVisual = previousAnimationToVisual;
    next.animationStart = previousAnimationStart;
    next.settleStableFrames = previousSettleStableFrames;
    next.settleHasSample = previousSettleHasSample;
    next.settleStart = previousSettleStart;
    next.relayoutProgress = previousRelayoutProgress;
    next.relayoutActive = previousRelayoutActive;
    next.relayoutStart = previousRelayoutStart;
    next.focusBeforeOpen = previousFocusBeforeOpen;
    next.closeMode = previousCloseMode;
    next.exitFullscreenReapplied = previousExitFullscreenReapplied;
    next.fullscreenOverrideActive = previousFullscreenOverrideActive;
    for (auto& backup : next.fullscreenBackups) {
        const auto previousIt = std::find_if(previousFullscreenBackups.begin(), previousFullscreenBackups.end(),
                                             [&](const FullscreenWorkspaceBackup& previous) { return previous.workspace == backup.workspace; });
        if (previousIt == previousFullscreenBackups.end())
            continue;

        backup.hadFullscreenWindow = previousIt->hadFullscreenWindow;
        backup.fullscreenMode = previousIt->fullscreenMode;
        backup.originalFullscreenWindow = previousIt->originalFullscreenWindow;
        backup.originalFullscreenMode = previousIt->originalFullscreenMode;
    }

    auto previousRectForWindow = [&](const PHLWINDOW& window) -> Rect {
        const auto it = std::find_if(previousPreviewRects.begin(), previousPreviewRects.end(), [&](const auto& previous) { return previous.first == window; });
        return it != previousPreviewRects.end() ? it->second : liveGlobalRectForWindow(window);
    };

    const auto previousManagedForWindow = [&](const PHLWINDOW& window) -> const ManagedWindow* {
        const auto it = std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == window; });
        return it == m_state.windows.end() ? nullptr : &*it;
    };

    if (previousPendingExitFocus && std::any_of(next.windows.begin(), next.windows.end(), [&](const ManagedWindow& managed) { return managed.window == previousPendingExitFocus; }))
        next.pendingExitFocus = previousPendingExitFocus;
    else
        next.pendingExitFocus = {};
    next.pendingExitWorkspace = containsHandle(next.managedWorkspaces, previousPendingExitWorkspace) ? previousPendingExitWorkspace : PHLWORKSPACE{};

    const bool sameWindowSet = next.windows.size() == m_state.windows.size() &&
        std::ranges::all_of(next.windows, [&](const ManagedWindow& managed) { return managed.window && previousManagedForWindow(managed.window) != nullptr; });
    const bool sameMonitorSet = next.participatingMonitors.size() == m_state.participatingMonitors.size() &&
        std::ranges::all_of(next.participatingMonitors, [&](const PHLMONITOR& monitor) { return containsHandle(m_state.participatingMonitors, monitor); });
    const bool selectionRelayoutForced = forceRelayout && expandSelectedWindowEnabled();
    const bool layoutRelayoutForced = forceRelayout;

    bool shouldAnimateRelayout = false;
    bool placeholderRelayoutChanged = false;
    if (sameWindowSet && sameMonitorSet && !layoutRelayoutForced && !selectionRelayoutForced) {
        for (auto& window : next.windows) {
            const auto* previousManaged = previousManagedForWindow(window.window);
            if (!previousManaged)
                continue;

            window.targetMonitor = previousManaged->targetMonitor;
            window.slot = previousManaged->slot;
            window.targetGlobal = previousManaged->targetGlobal;
            window.relayoutFromGlobal = previousManaged->targetGlobal;
            window.exitGlobal = previousManaged->exitGlobal;
        }

        for (auto& placeholder : next.emptyWorkspacePlaceholders) {
            if (!placeholder.monitor)
                continue;

            const auto previousIt = std::find_if(previousPlaceholderRects.begin(), previousPlaceholderRects.end(),
                                                 [&](const auto& entry) {
                                                     return std::get<0>(entry) == placeholder.monitor->m_id && std::get<1>(entry) == placeholder.workspaceId;
                                                 });
            if (previousIt == previousPlaceholderRects.end()) {
                placeholder.relayoutFromGlobal = placeholder.targetGlobal;
                continue;
            }

            placeholder.targetGlobal = std::get<2>(*previousIt);
            placeholder.relayoutFromGlobal = placeholder.targetGlobal;
        }
    } else if (previousPhase == Phase::Active || selectionRelayoutForced) {
        for (auto& window : next.windows) {
            if (const auto* previousManaged = previousManagedForWindow(window.window); previousManaged) {
                window.exitGlobal = previousManaged->exitGlobal;
            }

            const auto it = std::find_if(previousPreviewRects.begin(), previousPreviewRects.end(), [&](const auto& previous) { return previous.first == window.window; });
            if (it != previousPreviewRects.end())
                window.relayoutFromGlobal = it->second;
            else
                window.relayoutFromGlobal = window.naturalGlobal;

            if (!rectApproxEqual(window.relayoutFromGlobal, window.targetGlobal, 0.5))
                shouldAnimateRelayout = true;
        }

        for (auto& placeholder : next.emptyWorkspacePlaceholders) {
            if (!placeholder.monitor) {
                placeholder.relayoutFromGlobal = placeholder.targetGlobal;
                continue;
            }

            const auto previousIt = std::find_if(previousPlaceholderRects.begin(), previousPlaceholderRects.end(),
                                                 [&](const auto& entry) {
                                                     return std::get<0>(entry) == placeholder.monitor->m_id && std::get<1>(entry) == placeholder.workspaceId;
                                                 });
            if (previousIt == previousPlaceholderRects.end()) {
                placeholder.relayoutFromGlobal = placeholder.targetGlobal;
                continue;
            }

            placeholder.relayoutFromGlobal = std::get<2>(*previousIt);
            if (!rectApproxEqual(placeholder.relayoutFromGlobal, placeholder.targetGlobal, 0.5))
                placeholderRelayoutChanged = true;
        }
    } else {
        for (auto& placeholder : next.emptyWorkspacePlaceholders)
            placeholder.relayoutFromGlobal = placeholder.targetGlobal;
    }

    if (hyprlandOwnedDirectRelayout) {
        shouldAnimateRelayout = false;
        placeholderRelayoutChanged = false;
        for (auto& window : next.windows)
            window.relayoutFromGlobal = window.targetGlobal;
        for (auto& placeholder : next.emptyWorkspacePlaceholders)
            placeholder.relayoutFromGlobal = placeholder.targetGlobal;
    }

    auto appendTransientClosingWindow = [&](const ManagedWindow& source) {
        if (!source.window || !source.window->m_isMapped || !source.window->m_fadingOut)
            return;
        if (std::any_of(next.windows.begin(), next.windows.end(), [&](const ManagedWindow& managed) { return managed.window == source.window; }))
            return;
        if (std::any_of(next.transientClosingWindows.begin(), next.transientClosingWindows.end(), [&](const ManagedWindow& managed) { return managed.window == source.window; }))
            return;

        const Rect previewRect = previousRectForWindow(source.window);
        next.transientClosingWindows.push_back({
            .window = source.window,
            .targetMonitor = source.targetMonitor,
            .title = source.title,
            .naturalGlobal = previewRect,
            .exitGlobal = previewRect,
            .relayoutFromGlobal = previewRect,
            .targetGlobal = previewRect,
            .slot = source.slot,
            .previewAlpha = source.previewAlpha,
            .isFloating = source.isFloating,
            .isPinned = source.isPinned,
            .isNiriFloatingOverlay = source.isNiriFloatingOverlay,
        });
    };

    for (const auto& window : m_state.windows)
        appendTransientClosingWindow(window);
    for (const auto& window : m_state.transientClosingWindows)
        appendTransientClosingWindow(window);

    if (sameWindowSet && sameMonitorSet && !layoutRelayoutForced && !selectionRelayoutForced) {
        next.relayoutActive = false;
        next.relayoutProgress = 1.0;
        next.relayoutStart = {};
    } else if (shouldAnimateRelayout || placeholderRelayoutChanged) {
        next.relayoutActive = true;
        next.relayoutProgress = 0.0;
        next.relayoutStart = {};
    } else {
        next.relayoutActive = false;
        next.relayoutProgress = 1.0;
        next.relayoutStart = {};
        for (auto& window : next.windows)
            window.relayoutFromGlobal = window.targetGlobal;
        for (auto& placeholder : next.emptyWorkspacePlaceholders)
            placeholder.relayoutFromGlobal = placeholder.targetGlobal;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] rebuild overview state windows=" << next.windows.size() << " phase=";
        switch (next.phase) {
            case Phase::Inactive:
                out << "inactive";
                break;
            case Phase::Opening:
                out << "opening";
                break;
            case Phase::Active:
                out << "active";
                break;
            case Phase::ClosingSettle:
                out << "closing-settle";
                break;
            case Phase::Closing:
                out << "closing";
                break;
        }
        out << " relayout=" << (shouldAnimateRelayout ? 1 : 0);
        out << " frozenLayout=" << ((sameWindowSet && sameMonitorSet) ? 1 : 0);
        out << " forcedSelectionRelayout=" << (selectionRelayoutForced ? 1 : 0);
        out << " forcedLayoutRelayout=" << (layoutRelayoutForced ? 1 : 0);
        out << " carriedSwapcolSettle=" << (carriedFrozenSwapColumnLayout ? 1 : 0);
        debugLog(out.str());
        if (forceRelayout || selectionRelayoutForced) {
            logOverviewLayoutState("rebuild-before", m_state);
            logOverviewLayoutState("rebuild-next", next);
        }
    }

    clearStripWindowDragState();
    const bool stripRelayoutChanged = carryOverWorkspaceStripRelayout(next, m_state);
    if (!next.relayoutActive && stripRelayoutChanged) {
        next.relayoutActive = true;
        next.relayoutProgress = 0.0;
        next.relayoutStart = {};
    }
    carryOverWorkspaceStripSnapshots(next, m_state);
    restoreOverviewRenderState();
    m_state = std::move(next);
    if (!m_state.relayoutActive)
        m_relayoutProgressAnimation.reset();
    armOverviewRenderState(m_state);
    applyWorkspaceNameOverrides(m_state);

    // Sync scrolling layout focus after state rebuild. The buildState sets
    // focusDuringOverview but the scrolling layout's lastFocusedTarget is
    // not automatically updated. Without this, the visual selection border
    // (driven by focusDuringOverview) and the viewport centering (driven by
    // lastFocusedTarget) become desynchronized.
    if (m_state.focusDuringOverview && m_state.collectionPolicy.onlyActiveWorkspace && niriModeAppliesToState(m_state) &&
        isScrollingWorkspace(m_state.focusDuringOverview->m_workspace)) {
        (void)syncScrollingWorkspaceSpotOnWindow(m_state.focusDuringOverview);
    }

    syncHiddenStripLayerProxies();
    refreshWorkspaceStripSnapshots();
    if (selectionRelayoutForced) {
        m_state.hoveredIndex = m_state.selectedIndex;
    } else {
        updateHoveredFromPointer(false, false, false, false, forceRelayout ? "rebuild-forced" : "rebuild-passive");
    }
    if (!selectionRelayoutForced && m_state.phase == Phase::Active && (!sameWindowSet || !sameMonitorSet))
        updateSelectedWindowLayout({});
    if (debugLogsEnabled() && (forceRelayout || selectionRelayoutForced))
        logOverviewLayoutState("rebuild-after-hover-refresh", m_state);
    damageOwnedMonitors();
}

void OverviewController::moveSelection(Direction direction) {
    if (m_state.windows.empty()) {
        if (niriModeAppliesToState(m_state) && m_state.collectionPolicy.onlyActiveWorkspace && isScrollingWorkspace(activeLayoutWorkspace()) &&
            (direction == Direction::Up || direction == Direction::Down) && allowsWorkspaceSwitchInOverview())
            (void)switchOverviewWorkspaceByStep(direction == Direction::Up ? -1 : 1);
        return;
    }

    if (niriModeAppliesToState(m_state) && centeredEmptyWorkspacePlaceholder(m_state)) {
        if ((direction == Direction::Up || direction == Direction::Down) && allowsWorkspaceSwitchInOverview())
            (void)switchOverviewWorkspaceByStep(direction == Direction::Up ? -1 : 1);
        return;
    }

    const auto keyboardFocusableIndex = [&](std::size_t index) {
        if (index >= m_state.windows.size())
            return false;

        const auto& managed = m_state.windows[index];
        const auto  window = managed.window;
        if (!window || !window->m_isMapped)
            return false;

        // Keyboard overview navigation must not land on floating/pinned overlay
        // windows. Those previews stay clickable with the mouse, but arrow-key
        // navigation should only move through normal strip columns/windows.
        if (window->m_pinned || managed.isPinned || isFloatingOverviewWindow(window) || managed.isNiriFloatingOverlay)
            return false;

        return true;
    };

    if (!m_state.selectedIndex || *m_state.selectedIndex >= m_state.windows.size()) {
        const auto firstFocusable = std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) {
            const auto index = static_cast<std::size_t>(&managed - m_state.windows.data());
            return keyboardFocusableIndex(index);
        });
        if (firstFocusable == m_state.windows.end())
            return;

        m_state.selectedIndex = static_cast<std::size_t>(std::distance(m_state.windows.begin(), firstFocusable));
        syncFocusDuringOverviewFromSelection(true, "keyboard-init");
        damageOwnedMonitors();
        return;
    }

    const auto rects = targetRects();

    const auto chooseSameWorkspaceNeighbor = [&](Direction searchDirection) -> std::optional<std::size_t> {
        if (!m_state.selectedIndex || *m_state.selectedIndex >= m_state.windows.size() || *m_state.selectedIndex >= rects.size())
            return std::nullopt;

        const auto selectedWindow = m_state.windows[*m_state.selectedIndex].window;
        const auto selectedWorkspace = selectedWindow ? selectedWindow->m_workspace : PHLWORKSPACE{};
        if (!selectedWorkspace)
            return std::nullopt;

        const Rect& current = rects[*m_state.selectedIndex];
        std::optional<std::size_t> bestIndex;
        double bestScore = std::numeric_limits<double>::infinity();
        double bestDistance = std::numeric_limits<double>::infinity();

        for (std::size_t index = 0; index < m_state.windows.size() && index < rects.size(); ++index) {
            if (index == *m_state.selectedIndex || !keyboardFocusableIndex(index))
                continue;

            const auto candidateWindow = m_state.windows[index].window;
            if (!candidateWindow || candidateWindow->m_workspace != selectedWorkspace)
                continue;

            const Rect& candidate = rects[index];
            const double dx = candidate.centerX() - current.centerX();
            const double dy = candidate.centerY() - current.centerY();
            const bool verticalSearch = searchDirection == Direction::Up || searchDirection == Direction::Down;
            if (verticalSearch) {
                const double overlapLeft = std::max(current.x, candidate.x);
                const double overlapRight = std::min(current.x + current.width, candidate.x + candidate.width);
                const double overlapWidth = std::max(0.0, overlapRight - overlapLeft);
                if (overlapWidth < std::min(current.width, candidate.width) * 0.15)
                    continue;
            }

            double primary = 0.0;
            double secondary = 0.0;
            switch (searchDirection) {
                case Direction::Left:
                    primary = -dx;
                    secondary = std::abs(dy);
                    break;
                case Direction::Right:
                    primary = dx;
                    secondary = std::abs(dy);
                    break;
                case Direction::Up:
                    primary = -dy;
                    secondary = std::abs(dx);
                    break;
                case Direction::Down:
                    primary = dy;
                    secondary = std::abs(dx);
                    break;
            }

            if (primary <= 0.0)
                continue;

            const double score = primary * primary + std::pow(secondary * 1.5, 2.0);
            const double distance = dx * dx + dy * dy;

            if (!bestIndex || score < bestScore || (score == bestScore && distance < bestDistance) ||
                (score == bestScore && distance == bestDistance && index < *bestIndex)) {
                bestIndex = index;
                bestScore = score;
                bestDistance = distance;
            }
        }

        return bestIndex;
    };

    const auto chooseKeyboardDirectionalNeighbor = [&](Direction searchDirection) -> std::optional<std::size_t> {
        if (!m_state.selectedIndex || *m_state.selectedIndex >= rects.size())
            return std::nullopt;

        const Rect& current = rects[*m_state.selectedIndex];
        std::optional<std::size_t> bestIndex;
        double bestScore = std::numeric_limits<double>::infinity();
        double bestDistance = std::numeric_limits<double>::infinity();

        for (std::size_t index = 0; index < rects.size() && index < m_state.windows.size(); ++index) {
            if (index == *m_state.selectedIndex || !keyboardFocusableIndex(index))
                continue;

            const Rect& candidate = rects[index];
            const double dx = candidate.centerX() - current.centerX();
            const double dy = candidate.centerY() - current.centerY();

            double primary = 0.0;
            double secondary = 0.0;
            switch (searchDirection) {
                case Direction::Left:
                    primary = -dx;
                    secondary = std::abs(dy);
                    break;
                case Direction::Right:
                    primary = dx;
                    secondary = std::abs(dy);
                    break;
                case Direction::Up:
                    primary = -dy;
                    secondary = std::abs(dx);
                    break;
                case Direction::Down:
                    primary = dy;
                    secondary = std::abs(dx);
                    break;
            }

            if (primary <= 0.0)
                continue;

            const double score = primary * primary + std::pow(secondary * 1.5, 2.0);
            const double distance = dx * dx + dy * dy;

            if (!bestIndex || score < bestScore || (score == bestScore && distance < bestDistance) ||
                (score == bestScore && distance == bestDistance && index < *bestIndex)) {
                bestIndex = index;
                bestScore = score;
                bestDistance = distance;
            }
        }

        return bestIndex;
    };

    if (niriModeAppliesToState(m_state) && *m_state.selectedIndex < m_state.windows.size() && *m_state.selectedIndex < rects.size()) {
        if (const auto sameWorkspaceNeighbor = chooseSameWorkspaceNeighbor(direction)) {
            if (*sameWorkspaceNeighbor == *m_state.selectedIndex)
                return;

            m_state.selectedIndex = *sameWorkspaceNeighbor;
            syncFocusDuringOverviewFromSelection(true, "keyboard-nav");
            damageOwnedMonitors();
            return;
        }

        if (m_state.collectionPolicy.onlyActiveWorkspace && isScrollingWorkspace(activeLayoutWorkspace()) && (direction == Direction::Up || direction == Direction::Down) &&
            allowsWorkspaceSwitchInOverview()) {
            (void)switchOverviewWorkspaceByStep(direction == Direction::Up ? -1 : 1);
            return;
        }

        if (direction == Direction::Left || direction == Direction::Right)
            return;
    }

    if (const auto next = chooseKeyboardDirectionalNeighbor(direction)) {
        if (*next == *m_state.selectedIndex)
            return;
        m_state.selectedIndex = *next;
        syncFocusDuringOverviewFromSelection(true, "keyboard-nav");
        damageOwnedMonitors();
    }
}

bool OverviewController::moveSelectionCircular(int step, const char* source) {
    if (m_state.windows.size() < 2)
        return false;

    const auto keyboardFocusableIndex = [&](std::size_t index) {
        if (index >= m_state.windows.size())
            return false;

        const auto& managed = m_state.windows[index];
        const auto  window = managed.window;
        return window && window->m_isMapped && !window->m_pinned && !managed.isPinned && !isFloatingOverviewWindow(window) && !managed.isNiriFloatingOverlay;
    };

    const std::size_t count = m_state.windows.size();
    const std::size_t start = (!m_state.selectedIndex || *m_state.selectedIndex >= count) ? 0 : *m_state.selectedIndex;
    for (std::size_t offset = 1; offset <= count; ++offset) {
        const long long rawIndex = static_cast<long long>(start) + static_cast<long long>(step) * static_cast<long long>(offset);
        const auto normalized = static_cast<std::size_t>((rawIndex % static_cast<long long>(count) + static_cast<long long>(count)) % static_cast<long long>(count));
        if (normalized == start || !keyboardFocusableIndex(normalized))
            continue;

        m_state.selectedIndex = normalized;
        syncFocusDuringOverviewFromSelection(true, source);
        damageOwnedMonitors();
        return true;
    }

    return false;
}

void OverviewController::activateSelection() {
    reconcileNiriCenteredSelectionForExit();

    if (!m_state.selectedIndex || *m_state.selectedIndex >= m_state.windows.size())
        return;

    // Make the selected/centered preview authoritative before closing. Rapid
    // niri focus changes can leave Hyprland's real focused window one step
    // behind, so closing must not fall back to the previous real focus.
    if (const auto selected = selectedWindow(); selected) {
        m_state.focusDuringOverview = selected;
        m_queuedOverviewSelectionTarget.reset();
        m_queuedOverviewSelectionSyncScrollingSpot = false;
        m_queuedOverviewSelectionCenterCursor = false;
        m_queuedOverviewLiveFocusTarget.reset();
        m_queuedOverviewLiveFocusSyncScrollingSpot = false;
        m_queuedOverviewLiveFocusCenterCursor = false;

        if (niriModeAppliesToState(m_state)) {
            syncRealFocusDuringOverview(selected, true);
            refreshNiriScrollingOverviewAfterLayoutScroll("activate-selection-sync");
        }
    }

    beginClose(CloseMode::ActivateSelection);
}

void OverviewController::clearStripWindowDragState() {
    cancelDirectNiriWindowDrag();
    m_pressedStripIndex.reset();
    m_pressedWindowIndex.reset();
    m_draggedWindowIndex.reset();
    m_pressedWindowPointer = {};
    m_draggedWindowPointerOffset = {};
    m_niriDragSession = {};
}

void OverviewController::applyWorkspaceStripCursorShape() const {
    if (!g_pHyprRenderer)
        return;

    const bool hoveredStrip = m_state.hoveredStripIndex && *m_state.hoveredStripIndex < m_state.stripEntries.size();
    g_pHyprRenderer->setCursorFromName(hoveredStrip ? "pointer" : "default", true);
}

bool OverviewController::refreshWorkspaceStripActivity(State& state, const PHLMONITOR& overrideMonitor, WORKSPACEID overrideWorkspaceId) const {
    bool changed = false;
    for (auto& entry : state.stripEntries) {
        bool active = false;
        if (entry.monitor && overrideMonitor && entry.monitor == overrideMonitor && overrideWorkspaceId != WORKSPACE_INVALID)
            active = entry.workspaceId == overrideWorkspaceId;
        else
            active = entry.monitor && entry.workspace && entry.monitor->m_activeWorkspace == entry.workspace;

        if (entry.active == active)
            continue;

        entry.active = active;
        changed = true;
    }
    return changed;
}

void OverviewController::resetStaleClientCursorShape() const {
    if (!g_pHyprRenderer)
        return;

    // Waybar can leave a pointer cursor active when Hymission opens from a bar
    // button and hides or replaces that layer before the client sees leave.
    g_pHyprRenderer->setCursorFromName("default", true);
}

void OverviewController::refreshPostCloseCursorShape() const {
    resetStaleClientCursorShape();

    if (g_pSeatManager)
        g_pSeatManager->setPointerFocus(nullptr, {});

    if (!g_pInputManager)
        return;

    g_pInputManager->refocus(g_pInputManager->getMouseCoordsInternal());
    g_pInputManager->sendMotionEventsToFocused();

    if (g_pSeatManager)
        g_pSeatManager->resendEnterEvents();
}

void OverviewController::activateStripTarget(std::size_t index) {
    if (index >= m_state.stripEntries.size())
        return;

    clearStripWindowDragState();

    const auto& entry = m_state.stripEntries[index];
    if (!entry.monitor || entry.workspaceId == WORKSPACE_INVALID)
        return;

    auto targetWorkspace = entry.workspace ? entry.workspace : g_pCompositor->getWorkspaceByID(entry.workspaceId);
    if (targetWorkspace && targetWorkspace->m_isSpecialWorkspace)
        return;

    if (targetWorkspace && entry.monitor->m_activeWorkspace == targetWorkspace) {
        m_state.hoveredStripIndex = index;
        if (const auto targetFocus = focusCandidateForWorkspace(targetWorkspace)) {
            m_state.focusDuringOverview = targetFocus;
            selectWindowInState(m_state, targetFocus);
            m_stripSnapshotsDirty = true;
            scheduleWorkspaceStripSnapshotRefresh();
        }
        damageOwnedMonitors();
        return;
    }

    const std::string targetName = entry.workspaceName.empty() ? std::to_string(entry.workspaceId) : entry.workspaceName;
    const bool        syntheticTarget = !targetWorkspace && (entry.syntheticEmpty || entry.newWorkspaceSlot);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] activate strip target monitor=" << entry.monitor->m_name;
        if (targetWorkspace)
            out << " workspace=" << debugWorkspaceLabel(targetWorkspace);
        else
            out << " workspace=<synthetic:" << entry.workspaceId << '>';
        out << " phase=" << static_cast<int>(m_state.phase)
            << " synthetic=" << (entry.syntheticEmpty ? 1 : 0) << " new=" << (entry.newWorkspaceSlot ? 1 : 0);
        debugLog(out.str());
    }

    if (!beginOverviewWorkspaceTransition(entry.monitor, entry.workspaceId, targetName, targetWorkspace, syntheticTarget, WorkspaceTransitionMode::TimedCommit)) {
        if (debugLogsEnabled())
            debugLog("[hymission] strip target transition begin failed");
        damageOwnedMonitors();
    }
}

void OverviewController::notify(const std::string& message, const CHyprColor& color, float durationMs) const {
    HyprlandAPI::addNotification(m_handle, message, color, durationMs);
}

void OverviewController::debugLog(const std::string& message) const {
    if (!debugLogsEnabled())
        return;

    if (Log::logger)
        Log::logger->log(Log::DEBUG, message);

    std::ofstream out("/tmp/hymission-debug.log", std::ios::app);
    if (!out.is_open())
        return;

    out << message << '\n';
}

void OverviewController::debugSurfaceLog(const std::string& message) const {
    if (!debugSurfaceLogsEnabled())
        return;

    if (Log::logger)
        Log::logger->log(Log::DEBUG, message);

    std::ofstream out("/tmp/hymission-debug.log", std::ios::app);
    if (!out.is_open())
        return;

    out << message << '\n';
}

std::string OverviewController::debugWorkspaceLabel(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return "<null-workspace>";

    std::ostringstream out;
    out << workspace->m_id << ':' << workspace->m_name;
    if (workspace->m_isSpecialWorkspace)
        out << " special";
    if (const auto monitor = workspace->m_monitor.lock())
        out << "@" << monitor->m_name;
    else
        out << "@<no-monitor>";
    return out.str();
}

std::string OverviewController::debugWindowLabel(const PHLWINDOW& window) const {
    if (!window)
        return "<null-window>";

    std::ostringstream out;
    out << window->m_class << "::" << window->m_title << '@' << std::hex << reinterpret_cast<uintptr_t>(window.get()) << std::dec
        << (window->m_isX11 ? " x11" : " wayland");
    return out.str();
}


void OverviewController::latchHoverSelectionAnchor(const Vector2D& pointer) {
    m_hoverSelectionAnchorPointer = pointer;
    m_hoverSelectionAnchorValid = true;
}

bool OverviewController::hoverSelectionRetargetLocked(const Vector2D& pointer, const std::optional<std::size_t>& hoveredIndex) const {
    if (!expandSelectedWindowEnabled() || !m_hoverSelectionAnchorValid || !m_state.selectedIndex || !hoveredIndex || *hoveredIndex == *m_state.selectedIndex)
        return false;

    const double distance = std::hypot(pointer.x - m_hoverSelectionAnchorPointer.x, pointer.y - m_hoverSelectionAnchorPointer.y);
    return distance < HOVER_SELECTION_RETARGET_DISTANCE;
}

void OverviewController::logOverviewLayoutState(const char* context, const State& state) const {
    if (!debugLogsEnabled())
        return;

    std::ostringstream summary;
    summary << "[hymission] layout dump context=" << (context ? context : "?") << " phase=";
    switch (state.phase) {
        case Phase::Inactive:
            summary << "inactive";
            break;
        case Phase::Opening:
            summary << "opening";
            break;
        case Phase::Active:
            summary << "active";
            break;
        case Phase::ClosingSettle:
            summary << "closing-settle";
            break;
        case Phase::Closing:
            summary << "closing";
            break;
    }
    summary << " ownerMonitor=" << (state.ownerMonitor ? state.ownerMonitor->m_name : "?")
            << " ownerWorkspace=" << debugWorkspaceLabel(state.ownerWorkspace)
            << " windows=" << state.windows.size();
    if (state.selectedIndex && *state.selectedIndex < state.windows.size())
        summary << " selected=" << *state.selectedIndex << ":" << debugWindowLabel(state.windows[*state.selectedIndex].window);
    else
        summary << " selected=<null>";
    if (state.hoveredIndex && *state.hoveredIndex < state.windows.size())
        summary << " hovered=" << *state.hoveredIndex << ":" << debugWindowLabel(state.windows[*state.hoveredIndex].window);
    else
        summary << " hovered=<null>";
    if (state.focusDuringOverview)
        summary << " focusDuringOverview=" << debugWindowLabel(state.focusDuringOverview);
    else
        summary << " focusDuringOverview=<null>";
    debugLog(summary.str());

    if (state.windows.empty())
        return;

    std::ostringstream stateOrder;
    stateOrder << "[hymission] layout dump state-order context=" << (context ? context : "?");
    for (std::size_t index = 0; index < state.windows.size(); ++index) {
        const auto& managed = state.windows[index];
        stateOrder << " | #" << index << ' ' << debugWindowLabel(managed.window);
        if (managed.window && managed.window->m_workspace)
            stateOrder << " ws=" << debugWorkspaceLabel(managed.window->m_workspace);
        stateOrder << " mon=" << (managed.targetMonitor ? managed.targetMonitor->m_name : "?")
                   << " slot=" << rectToString(managed.slot.target)
                   << " target=" << rectToString(managed.targetGlobal);
    }
    debugLog(stateOrder.str());

    std::vector<std::size_t> visualOrder(state.windows.size());
    std::iota(visualOrder.begin(), visualOrder.end(), 0);
    std::stable_sort(visualOrder.begin(), visualOrder.end(), [&](std::size_t lhs, std::size_t rhs) {
        const auto& left = state.windows[lhs];
        const auto& right = state.windows[rhs];
        const auto leftMonitorId = left.targetMonitor ? left.targetMonitor->m_id : MONITOR_INVALID;
        const auto rightMonitorId = right.targetMonitor ? right.targetMonitor->m_id : MONITOR_INVALID;
        if (leftMonitorId != rightMonitorId)
            return leftMonitorId < rightMonitorId;
        if (std::abs(left.targetGlobal.y - right.targetGlobal.y) > 0.5)
            return left.targetGlobal.y < right.targetGlobal.y;
        if (std::abs(left.targetGlobal.x - right.targetGlobal.x) > 0.5)
            return left.targetGlobal.x < right.targetGlobal.x;
        return lhs < rhs;
    });

    std::ostringstream visual;
    visual << "[hymission] layout dump visual-order context=" << (context ? context : "?");
    for (std::size_t visualIndex = 0; visualIndex < visualOrder.size(); ++visualIndex) {
        const auto stateIndex = visualOrder[visualIndex];
        const auto& managed = state.windows[stateIndex];
        visual << " | @" << visualIndex << " #" << stateIndex << ' ' << debugWindowLabel(managed.window)
               << " target=" << rectToString(managed.targetGlobal);
    }
    debugLog(visual.str());
}


bool OverviewController::workspaceStripEntriesMatchForSnapshot(const WorkspaceStripEntry& lhs, const WorkspaceStripEntry& rhs) const {
    const auto lhsMonitorId = lhs.monitor ? lhs.monitor->m_id : MONITOR_INVALID;
    const auto rhsMonitorId = rhs.monitor ? rhs.monitor->m_id : MONITOR_INVALID;
    if (lhsMonitorId != rhsMonitorId)
        return false;

    if (lhs.newWorkspaceSlot != rhs.newWorkspaceSlot)
        return false;

    if (lhs.workspaceId != rhs.workspaceId)
        return false;

    if (lhs.newWorkspaceSlot)
        return lhs.workspaceId == rhs.workspaceId;

    if (lhs.workspace && rhs.workspace)
        return lhs.workspace == rhs.workspace;

    return true;
}

void OverviewController::carryOverWorkspaceStripSnapshots(State& next, const State& previous) const {
    if (next.stripEntries.empty() || previous.stripEntries.empty())
        return;

    // Keep the previous strip textures alive until the async refresh repaints
    // the new state. Otherwise workspace commits briefly render only the card
    // background because the replacement snapshot is deferred to doLater().
    for (auto& nextEntry : next.stripEntries) {
        if (nextEntry.snapshot || nextEntry.newWorkspaceSlot)
            continue;

        const auto previousIt = std::find_if(previous.stripEntries.begin(), previous.stripEntries.end(), [&](const WorkspaceStripEntry& previousEntry) {
            return previousEntry.snapshot && workspaceStripEntriesMatchForSnapshot(nextEntry, previousEntry);
        });
        if (previousIt == previous.stripEntries.end())
            continue;

        nextEntry.snapshot = previousIt->snapshot;
    }
}

bool OverviewController::carryOverWorkspaceStripRelayout(State& next, const State& previous) const {
    if (next.stripEntries.empty() || previous.stripEntries.empty())
        return false;

    bool stripChanged = false;
    for (auto& nextEntry : next.stripEntries) {
        const auto previousIt = std::find_if(previous.stripEntries.begin(), previous.stripEntries.end(),
                                             [&](const WorkspaceStripEntry& previousEntry) {
                                                 return workspaceStripEntriesMatchForSnapshot(nextEntry, previousEntry);
                                             });

        if (previousIt == previous.stripEntries.end()) {
            nextEntry.relayoutFromRect = nextEntry.rect;
            nextEntry.hasRelayoutFromRect = false;
            continue;
        }

        const Rect previousRect = previousIt->hasRelayoutFromRect ? currentWorkspaceStripRect(*previousIt) : previousIt->rect;
        nextEntry.relayoutFromRect = previousRect;
        nextEntry.hasRelayoutFromRect = !rectApproxEqual(previousRect, nextEntry.rect, 0.5);
        stripChanged = stripChanged || nextEntry.hasRelayoutFromRect;
    }

    return stripChanged;
}

void OverviewController::renderBackdrop() const {
    const double progress = visualProgress();
    if (progress <= 0.0)
        return;

    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!monitor)
        return;

    CHyprColor color = CHyprColor(0.05, 0.06, 0.08, BACKDROP_ALPHA * progress);
    if (niriWallpaperZoomAppliesToMonitor(m_state, monitor)) {
        color = niriModeWallpaperZoomBackgroundColor();
        color.a *= progress;
    }

    g_pHyprOpenGL->renderRect(
        CBox(
            0.0,
            0.0,
            monitor->m_transformedSize.x,
            monitor->m_transformedSize.y),
        color,
        {});
}



void OverviewController::renderSelectionChrome() const {
    const double progress = visualProgress();

    const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!renderMonitor)
        return;

    const bool directNiriCloseChrome = (usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state)) &&
        (m_beginCloseInProgress || m_state.phase == Phase::ClosingSettle || m_state.phase == Phase::Closing ||
         m_deactivatePending || directNiriNativeHandoffActive());
    if (progress <= 0.0 && !directNiriCloseChrome)
        return;

    const auto closingTargetsEmptyNiriWorkspaceOnMonitor = [&]() {
        if (!niriModeEnabled() || !m_state.collectionPolicy.onlyActiveWorkspace)
            return false;

        if (m_state.pendingExitFocus)
            return false;

        if (m_state.phase != Phase::Closing && m_state.phase != Phase::ClosingSettle && !m_beginCloseInProgress && !m_deactivatePending)
            return false;

        if (m_state.pendingExitWorkspace) {
            const auto workspaceMonitor = m_state.pendingExitWorkspace->m_monitor.lock();
            if (!workspaceMonitor || workspaceMonitor == renderMonitor)
                return true;
        }

        return std::ranges::any_of(m_state.emptyWorkspacePlaceholders, [&](const EmptyWorkspacePlaceholder& placeholder) {
            return !placeholder.backingOnly && placeholder.monitor && placeholder.monitor == renderMonitor;
        });
    };

    if (closingTargetsEmptyNiriWorkspaceOnMonitor())
        return;

    // Direct Niri workspace lanes are normal overview window previews, not the
    // optional top/side workspace-strip thumbnails.  If Hyprland's inactive
    // workspace render path gives us only the window/deco pass (border visible,
    // client area blank), overlay the current main-surface texture here, just
    // before drawing Hymission chrome.  This mirrors Niri's model: overview
    // renders windows from their current layout/window state instead of waiting
    // for a workspace switch to make the workspace visible first.
    if (!directNiriCloseChrome && g_pHyprOpenGL && m_state.collectionPolicy.onlyActiveWorkspace &&
        (usesDirectNiriScrollingOverview(m_state) || niriModeAppliesToState(m_state))) {
        std::size_t rendered = 0;
        std::size_t missing = 0;
        std::size_t skipped = 0;
        static std::size_t s_directNiriSurfaceOverlayLogBudget = 320;

        for (const auto& managed : m_state.windows) {
            const auto window = managed.window;
            if (!window || managed.targetMonitor != renderMonitor || !windowMatchesOverviewScope(window, m_state, false)) {
                ++skipped;
                continue;
            }

            if (!window->m_workspace || !isScrollingWorkspace(window->m_workspace) || window->m_pinned || managed.isPinned ||
                managed.isNiriFloatingOverlay || isFloatingOverviewWindow(window)) {
                ++skipped;
                continue;
            }

            // The live texture overlay is only a fallback for inactive/unvisited
            // workspaces whose normal fake-render path can temporarily produce
            // blank client contents.  For the currently visible workspace, the
            // transformed Hyprland surface pass is the authoritative renderer;
            // overlaying the raw main-surface texture during a spawn/resize races
            // the client's configure/commit size and can stretch or offset stale
            // buffers over the preview for a couple seconds.
            if (window->m_workspace->isVisible()) {
                ++skipped;
                continue;
            }

            const auto surface = window->wlSurface();
            const auto resource = surface ? surface->resource() : nullptr;
            const auto texture = resource ? resource->m_current.texture : nullptr;
            const auto transform = windowTransformFor(window, renderMonitor);
            const Rect targetGlobal = transform ? transform->targetGlobal : currentPreviewRect(managed);
            const Rect targetRender = rectToMonitorRenderLocal(targetGlobal, renderMonitor);
            const bool usableTarget = targetRender.width > 1.0 && targetRender.height > 1.0;

            if (!texture || !usableTarget) {
                ++missing;
                if (debugLogsEnabled() && s_directNiriSurfaceOverlayLogBudget > 0) {
                    std::ostringstream out;
                    out << "[hymission] direct niri live surface overlay missing"
                        << " window=" << debugWindowLabel(window)
                        << " workspace=" << debugWorkspaceLabel(window->m_workspace)
                        << " texture=" << (texture ? 1 : 0)
                        << " target=" << rectToString(targetGlobal)
                        << " targetRender=" << rectToString(targetRender)
                        << " hidden=" << (window->isHidden() ? 1 : 0)
                        << " mapped=" << (window->m_isMapped ? 1 : 0)
                        << " wsVisible=" << (window->m_workspace && window->m_workspace->isVisible() ? 1 : 0)
                        << " wsForceRendering=" << (window->m_workspace && window->m_workspace->m_forceRendering ? 1 : 0)
                        << " alphaTotal=" << window->alphaTotal();
                    if (resource)
                        out << " surfSize=" << vectorToString(resource->m_current.size)
                            << " buffer=" << vectorToString(resource->m_current.bufferSize);
                    else
                        out << " surface=<null>";
                    debugLog(out.str());
                    --s_directNiriSurfaceOverlayLogBudget;
                }
                continue;
            }

            const float alpha = std::clamp(managedPreviewAlphaFor(window, managed.previewAlpha), 0.0F, 1.0F) *
                static_cast<float>(std::clamp(progress, 0.0, 1.0));
            if (alpha <= 0.001F) {
                ++missing;
                if (debugLogsEnabled() && s_directNiriSurfaceOverlayLogBudget > 0) {
                    std::ostringstream out;
                    out << "[hymission] direct niri live surface overlay skipped-alpha"
                        << " window=" << debugWindowLabel(window)
                        << " workspace=" << debugWorkspaceLabel(window->m_workspace)
                        << " alpha=" << alpha
                        << " previewAlpha=" << managed.previewAlpha
                        << " alphaTotal=" << window->alphaTotal()
                        << " hidden=" << (window->isHidden() ? 1 : 0)
                        << " wsVisible=" << (window->m_workspace && window->m_workspace->isVisible() ? 1 : 0);
                    debugLog(out.str());
                    --s_directNiriSurfaceOverlayLogBudget;
                }
                continue;
            }

            g_pHyprOpenGL->renderTexture(texture, toBox(targetRender), {.a = alpha});
            ++rendered;

            if (debugLogsEnabled() && s_directNiriSurfaceOverlayLogBudget > 0) {
                std::ostringstream out;
                out << "[hymission] direct niri live surface overlay render"
                    << " window=" << debugWindowLabel(window)
                    << " workspace=" << debugWorkspaceLabel(window->m_workspace)
                    << " target=" << rectToString(targetGlobal)
                    << " targetRender=" << rectToString(targetRender)
                    << " alpha=" << alpha
                    << " hidden=" << (window->isHidden() ? 1 : 0)
                    << " mapped=" << (window->m_isMapped ? 1 : 0)
                    << " wsVisible=" << (window->m_workspace && window->m_workspace->isVisible() ? 1 : 0)
                    << " wsForceRendering=" << (window->m_workspace && window->m_workspace->m_forceRendering ? 1 : 0)
                    << " alphaTotal=" << window->alphaTotal();
                if (resource)
                    out << " surfSize=" << vectorToString(resource->m_current.size)
                        << " buffer=" << vectorToString(resource->m_current.bufferSize);
                debugLog(out.str());
                --s_directNiriSurfaceOverlayLogBudget;
            }
        }

        if (debugLogsEnabled() && s_directNiriSurfaceOverlayLogBudget > 0) {
            std::ostringstream out;
            out << "[hymission] direct niri live surface overlay summary"
                << " monitor=" << renderMonitor->m_name
                << " rendered=" << rendered
                << " missing=" << missing
                << " skipped=" << skipped
                << " windows=" << m_state.windows.size();
            debugLog(out.str());
            --s_directNiriSurfaceOverlayLogBudget;
        }
    }

    const bool showFocusIndicator = !directNiriCloseChrome && showFocusIndicatorEnabled();

    if (showFocusIndicator && m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size() &&
        m_state.windows[*m_state.hoveredIndex].targetMonitor == renderMonitor) {
        renderOutline(currentPreviewRect(m_state.windows[*m_state.hoveredIndex]), CHyprColor(0.95, 0.97, 1.0, 0.55 * progress), HOVER_THICKNESS);
    }

    if (showFocusIndicator && m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size() &&
        m_state.windows[*m_state.selectedIndex].targetMonitor == renderMonitor) {
        const auto& window = m_state.windows[*m_state.selectedIndex];
        const Rect  rectGlobal = currentPreviewRect(window);
        const Rect  rect = rectToMonitorRenderLocal(rectGlobal, renderMonitor);
        renderOutline(rectGlobal, CHyprColor(0.24, 0.78, 1.0, 0.95 * progress), OUTLINE_THICKNESS);

        auto texture =
            g_pHyprRenderer->renderText(window.title, CHyprColor(1.0, 1.0, 1.0, std::min(1.0, progress)), scaleFontSizeForRender(renderMonitor, 16), false, "",
                                        static_cast<int>(rect.width));
        if (texture) {
            const Rect titleRect =
                makeRect(rect.x, std::max(scaleLengthForRender(renderMonitor, 8.0), rect.y - texture->m_size.y - scaleLengthForRender(renderMonitor, TITLE_PADDING)),
                         texture->m_size.x, texture->m_size.y);
            g_pHyprOpenGL->renderTexture(texture, toBox(titleRect), {});
        }
    }

    const double borderProgress = directNiriCloseChrome ? 1.0 : progress;
    const bool niriWorkspaceTransitionBorders = m_workspaceTransition.active && m_workspaceTransition.monitor &&
        m_workspaceTransition.monitor == renderMonitor && m_state.collectionPolicy.onlyActiveWorkspace &&
        (niriModeAppliesToState(m_workspaceTransition.sourceState) || niriModeAppliesToState(m_workspaceTransition.targetState));

    if (niriWorkspaceTransitionBorders) {
        renderInactiveWindowBorders(m_workspaceTransition.sourceState, borderProgress, false);
        renderInactiveWindowBorders(m_workspaceTransition.targetState, borderProgress, false);
        renderFocusedWindowBorder(m_workspaceTransition.targetState, borderProgress, false);
    } else {
        renderInactiveWindowBorders(m_state, borderProgress, false);
        renderFocusedWindowBorder(m_state, borderProgress, false);
    }

    if (!m_niriDragSession.active && m_draggedWindowIndex && *m_draggedWindowIndex < m_state.windows.size() && m_state.windows[*m_draggedWindowIndex].targetMonitor == renderMonitor) {
        const auto& window = m_state.windows[*m_draggedWindowIndex];
        const Rect  preview = currentPreviewRect(window);
        const auto  pointer = g_pInputManager->getMouseCoordsInternal();
        const Rect  ghostGlobal = makeRect(pointer.x - m_draggedWindowPointerOffset.x, pointer.y - m_draggedWindowPointerOffset.y, preview.width, preview.height);
        const Rect  ghost = rectToMonitorRenderLocal(ghostGlobal, renderMonitor);
        g_pHyprOpenGL->renderRect(toBox(ghost), CHyprColor(0.16, 0.20, 0.24, 0.28 * progress), {});
        renderOutline(ghostGlobal, CHyprColor(0.95, 0.97, 1.0, 0.82 * progress), 2.0);
    }
}

const OverviewController::ManagedWindow* OverviewController::focusedManagedForBorder(const State& state, const PHLMONITOR& renderMonitor) const {
    const bool edgeCameraActive = directNiriOwnerEdgeCameraActive(state);

    auto focusedWindow = directNiriFocusedOverviewWindow(state);
    if (!focusedWindow && !edgeCameraActive)
        focusedWindow = state.focusDuringOverview;

    auto focusedManaged = managedWindowFor(state, focusedWindow, true);
    if (!focusedManaged) {
        focusedWindow = Desktop::focusState()->window();

        const bool liveFocusBelongsToOwnerWorkspace = focusedWindow && focusedWindow->m_isMapped && !focusedWindow->m_pinned && state.ownerWorkspace &&
            focusedWindow->m_workspace == state.ownerWorkspace;

        // In direct Niri scroll-past, Hyprland intentionally clears native focus.
        // While that native focus is null, do not fall back to any stale previous
        // window for the active border.  Once Hyprland restores a real leaf focus
        // while the offset is still technically in the edge-camera range, allow
        // that live focus to own the active border immediately.
        if (!edgeCameraActive || liveFocusBelongsToOwnerWorkspace)
            focusedManaged = managedWindowFor(state, focusedWindow, true);
    }

    if (!focusedManaged || !focusedManaged->window || focusedManaged->targetMonitor != renderMonitor)
        return nullptr;

    return focusedManaged;
}

bool OverviewController::borderUsesTransformedGeometry(const State& state) const {
    if (m_gestureSession.active)
        return false;

    if (state.phase == Phase::Opening || state.phase == Phase::ClosingSettle || state.phase == Phase::Closing)
        return false;

    if (state.phase == Phase::Active && state.relayoutActive)
        return false;

    return true;
}

Rect OverviewController::managedWindowBorderRect(const ManagedWindow& managed, const PHLMONITOR& renderMonitor, const State& state, bool useTargetGeometry,
                                                 bool forceTransformedGeometry) const {
    Rect rect = useTargetGeometry ? managed.targetGlobal : currentPreviewRect(managed);

    if (forceTransformedGeometry || borderUsesTransformedGeometry(state)) {
        if (const auto transform = windowTransformFor(managed.window, renderMonitor))
            rect = transform->targetGlobal;
    }

    return snapRectToRenderPixelGrid(rect, renderMonitor);
}

int OverviewController::managedWindowBorderRound(const ManagedWindow& managed, const PHLMONITOR& renderMonitor) const {
    if (!managed.window || !renderMonitor)
        return 0;

    const double baseRound = std::max(0.0, static_cast<double>(managed.window->rounding()));
    if (baseRound <= 0.0)
        return 0;

    double scale = 1.0;
    if (const auto transform = windowTransformFor(managed.window, renderMonitor))
        scale = std::max(0.0, std::min(std::abs(transform->scaleX), std::abs(transform->scaleY)));

    if (m_stripPreviewContext.active) {
        const auto   fbSize = m_stripPreviewContext.framebufferSize;
        const double monitorPixelWidth = std::max(1.0, static_cast<double>(renderMonitor->m_size.x) * renderScaleForMonitor(renderMonitor));
        const double monitorPixelHeight = std::max(1.0, static_cast<double>(renderMonitor->m_size.y) * renderScaleForMonitor(renderMonitor));
        const double fbScale = std::clamp(std::min(fbSize.x / monitorPixelWidth, fbSize.y / monitorPixelHeight), 0.0, 1.0);
        scale *= fbScale;
    }

    return std::max(0, static_cast<int>(std::lround(baseRound * scale * overviewBorderRoundingScale())));
}

float OverviewController::managedWindowBorderRoundingPower(const ManagedWindow& managed) const {
    if (!managed.window)
        return 2.0F;

    return std::max(0.01F, managed.window->roundingPower());
}

void OverviewController::renderInactiveWindowBorders(const State& state, double progress, bool useTargetGeometry) const {
    if (progress <= 0.0)
        return;

    const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!renderMonitor)
        return;

    const double thickness = inactiveBorderWidth();
    if (thickness <= 0.0)
        return;

    const auto* focusedManaged = focusedManagedForBorder(state, renderMonitor);
    const auto inactiveGradient = inactiveBorderGradient();
    for (const auto& managed : state.windows) {
        if (!managed.window || managed.targetMonitor != renderMonitor)
            continue;
        if (focusedManaged && managed.window == focusedManaged->window)
            continue;

        renderOutline(managedWindowBorderRect(managed, renderMonitor, state, useTargetGeometry, true), inactiveGradient, thickness, 0.95 * progress,
                      managedWindowBorderRound(managed, renderMonitor), managedWindowBorderRoundingPower(managed));
    }
}

void OverviewController::renderFocusedWindowBorder(const State& state, double progress, bool useTargetGeometry) const {
    if (progress <= 0.0)
        return;

    const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!renderMonitor)
        return;

    const double configuredWidth = activeBorderWidth();
    if (configuredWidth <= 0.0)
        return;

    const double thickness = std::max(1.0, configuredWidth - focusedBorderThicknessReduction());

    const auto* focusedManaged = focusedManagedForBorder(state, renderMonitor);
    if (!focusedManaged)
        return;

    renderOutline(managedWindowBorderRect(*focusedManaged, renderMonitor, state, useTargetGeometry), activeBorderGradient(), thickness, 0.95 * progress,
                  managedWindowBorderRound(*focusedManaged, renderMonitor), managedWindowBorderRoundingPower(*focusedManaged));
}

void OverviewController::renderOutline(const Rect& rect, const CHyprColor& color, double thickness) const {
    const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!renderMonitor)
        return;

    const Rect local = rectToMonitorRenderLocal(rect, renderMonitor);
    const double scaledThickness = scaleLengthForRender(renderMonitor, thickness);
    const Rect top = makeRect(local.x - scaledThickness, local.y - scaledThickness, local.width + scaledThickness * 2.0, scaledThickness);
    const Rect bottom = makeRect(local.x - scaledThickness, local.y + local.height, local.width + scaledThickness * 2.0, scaledThickness);
    const Rect left = makeRect(local.x - scaledThickness, local.y, scaledThickness, local.height);
    const Rect right = makeRect(local.x + local.width, local.y, scaledThickness, local.height);

    g_pHyprOpenGL->renderRect(toBox(top), color, {});
    g_pHyprOpenGL->renderRect(toBox(bottom), color, {});
    g_pHyprOpenGL->renderRect(toBox(left), color, {});
    g_pHyprOpenGL->renderRect(toBox(right), color, {});
}

void OverviewController::renderOutline(const Rect& rect, const Config::CGradientValueData& gradient, double thickness, double alpha, int round,
                                       float roundingPower) const {
    const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!renderMonitor || gradient.m_colors.empty())
        return;

    const Rect local = rectToMonitorRenderLocal(rect, renderMonitor);
    if (local.width <= 0.0 || local.height <= 0.0)
        return;

    const double x1 = std::floor(local.x);
    const double y1 = std::floor(local.y);
    const double x2 = std::ceil(local.x + local.width);
    const double y2 = std::ceil(local.y + local.height);
    constexpr double BORDER_INSET_PX = 1.0;
    const Rect       aligned = makeRect(x1 + BORDER_INSET_PX, y1 + BORDER_INSET_PX,
                                  std::max(0.0, (x2 - x1) - BORDER_INSET_PX * 2.0),
                                  std::max(0.0, (y2 - y1) - BORDER_INSET_PX * 2.0));
    if (aligned.width <= 0.0 || aligned.height <= 0.0)
        return;

    const int borderThickness = std::max(1, static_cast<int>(std::lround(thickness)));
    g_pHyprOpenGL->renderBorder(toBox(aligned), gradient,
                                {.round = std::max(0, round),
                                 .roundingPower = std::max(0.01F, roundingPower),
                                 .borderSize = borderThickness,
                                 .a = static_cast<float>(std::clamp(alpha, 0.0, 1.0))});
}

Rect OverviewController::workspaceStripThumbRect(const WorkspaceStripEntry& entry, const PHLMONITOR& monitor) const {
    if (!monitor)
        return {};

    const Rect outer = rectToMonitorLocal(entry.rect, monitor);
    return makeRect(outer.x, outer.y, outer.width, outer.height);
}

void OverviewController::renderWorkspaceStripSnapshot(WorkspaceStripEntry& entry) {
    // Keep the existing snapshot alive until a replacement is successfully
    // rendered.  In direct niri mode Hyprland may report only the active
    // workspace as renderable while the overview is open; clearing here makes
    // adjacent workspace thumbnails flash once and then disappear until that
    // workspace is focused again.

    if (!entry.monitor || entry.newWorkspaceSlot) {
        entry.snapshot.reset();
        return;
    }

    const Rect thumb = workspaceStripThumbRect(entry, entry.monitor);
    if (thumb.width < 4.0 || thumb.height < 4.0)
        return;

    const int fbWidth = std::max(1, static_cast<int>(std::ceil(thumb.width * entry.monitor->m_scale)));
    const int fbHeight = std::max(1, static_cast<int>(std::ceil(thumb.height * entry.monitor->m_scale)));
    using RenderWindowFn = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);

    static RenderWindowFn renderWindowFn = nullptr;
    static bool           renderWindowResolved = false;
    if (!renderWindowResolved) {
        renderWindowResolved = true;
        renderWindowFn = reinterpret_cast<RenderWindowFn>(findFunction("renderWindow", "IHyprRenderer::renderWindow"));
        if (!renderWindowFn)
            debugLog("[hymission] failed to resolve IHyprRenderer::renderWindow for strip snapshots");
    }

    const auto monitor = entry.monitor;
    auto       targetWorkspace = entry.workspace ? entry.workspace : g_pCompositor->getWorkspaceByID(entry.workspaceId);
    if (targetWorkspace) {
        const auto targetWorkspaceMonitor = targetWorkspace->m_monitor.lock();
        if (!targetWorkspaceMonitor || targetWorkspaceMonitor != monitor) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] strip snapshot skipped foreign workspace monitor=" << monitor->m_name
                    << " workspace=" << debugWorkspaceLabel(targetWorkspace)
                    << " workspaceMonitor=" << (targetWorkspaceMonitor ? targetWorkspaceMonitor->m_name : std::string{"<none>"});
                debugLog(out.str());
            }
            targetWorkspace = PHLWORKSPACE{};
        }
    }
    if (!targetWorkspace && !entry.syntheticEmpty)
        return;
    auto snapshot = std::make_shared<WorkspaceStripEntry::Snapshot>();
    snapshot->framebuffer = createFramebuffer("hymission workspace strip snapshot");
    if (!snapshot->framebuffer || !snapshot->framebuffer->alloc(fbWidth, fbHeight))
        return;
    snapshot->framebuffer->setImageDescription(monitor->workBufferImageDescription());
    setFramebufferLinearFiltering(*snapshot->framebuffer);

    const bool directNiriStripSnapshot = targetWorkspace && niriModeEnabled() && m_state.collectionPolicy.onlyActiveWorkspace && isScrollingWorkspace(targetWorkspace);

    State previewState;
    bool  renderWorkspaceContents = false;
    if (targetWorkspace) {
        const std::vector<WorkspaceOverride> workspaceOverrides = {{
            .monitorId = monitor->m_id,
            .workspace = targetWorkspace,
            .workspaceId = entry.workspaceId,
            .workspaceName = entry.workspaceName,
            .syntheticEmpty = false,
        }};
        const bool niriStripPreview = directNiriStripSnapshot;
        ScopedFlag niriStripSingleWorkspace(g_niriStripSnapshotSingleWorkspaceOnly, niriStripPreview);
        const auto preferredPreviewFocus =
            entry.active && m_state.focusDuringOverview && m_state.focusDuringOverview->m_workspace == targetWorkspace ?
            m_state.focusDuringOverview :
            focusCandidateForWorkspace(targetWorkspace);
        previewState = buildState(monitor, ScopeOverride::OnlyCurrentWorkspace, workspaceOverrides, true, false, preferredPreviewFocus);

        previewState.phase = Phase::Active;
        previewState.animationProgress = 1.0;
        previewState.animationFromVisual = 1.0;
        previewState.animationToVisual = 1.0;
        previewState.relayoutProgress = 1.0;
        previewState.relayoutActive = false;

        const Vector2D previewOffset = stripThumbnailPreviewOffset(monitor, previewState);
        if (previewOffset.x != 0.0 || previewOffset.y != 0.0) {
            for (auto& managed : previewState.windows) {
                managed.targetGlobal = translateRect(managed.targetGlobal, -previewOffset.x, -previewOffset.y);
                managed.relayoutFromGlobal = translateRect(managed.relayoutFromGlobal, -previewOffset.x, -previewOffset.y);
                managed.exitGlobal = translateRect(managed.exitGlobal, -previewOffset.x, -previewOffset.y);
                managed.slot.target =
                    makeRect(managed.slot.target.x - previewOffset.x, managed.slot.target.y - previewOffset.y, managed.slot.target.width, managed.slot.target.height);
            }
        }

        if (previewState.windows.empty()) {
            auto fallbackPreviews = entry.windows;
            fallbackPreviews.reserve(fallbackPreviews.size() + g_pCompositor->m_windows.size());

            for (const auto& window : g_pCompositor->m_windows) {
                if (!window || !window->m_isMapped || window->m_fadingOut || window->m_workspace != targetWorkspace)
                    continue;

                if (!windowHasUsableStateGeometry(window))
                    continue;

                const bool alreadyListed = std::ranges::any_of(fallbackPreviews, [&](const auto& preview) { return preview.window == window; });
                if (alreadyListed)
                    continue;

                const bool useGoalGeometry = shouldUseGoalGeometryForStateSnapshot(window);
                fallbackPreviews.push_back({
                    .window = window,
                    .naturalGlobal = stateSnapshotGlobalRectForWindow(window, useGoalGeometry),
                    .alpha = niriStripPreview ? 1.0F : std::clamp(window->alphaTotal(), 0.0F, 1.0F),
                    .focused = window == preferredPreviewFocus,
                });
            }

            if (!fallbackPreviews.empty()) {
                previewState.ownerMonitor = monitor;
                previewState.ownerWorkspace = targetWorkspace;
                previewState.collectionPolicy = loadCollectionPolicy(ScopeOverride::OnlyCurrentWorkspace);
                previewState.suppressWorkspaceStrip = true;
                previewState.participatingMonitors = {monitor};
                previewState.managedWorkspaces = {targetWorkspace};
                previewState.focusDuringOverview = preferredPreviewFocus ? preferredPreviewFocus : Desktop::focusState()->window();

                previewState.windows.reserve(fallbackPreviews.size());
                for (const auto& preview : fallbackPreviews) {
                    if (!preview.window)
                        continue;

                    const Rect local = rectToMonitorLocal(preview.naturalGlobal, monitor);
                    const auto index = previewState.windows.size();
                    previewState.windows.push_back({
                        .window = preview.window,
                        .targetMonitor = monitor,
                        .title = preview.window->m_title,
                        .naturalGlobal = preview.naturalGlobal,
                        .exitGlobal = preview.naturalGlobal,
                        .relayoutFromGlobal = preview.naturalGlobal,
                        .targetGlobal = preview.naturalGlobal,
                        .slot =
                            {
                                .index = index,
                                .natural = local,
                                .target = local,
                                .scale = 1.0,
                            },
                        .previewAlpha = preview.alpha,
                        .isFloating = preview.window->m_isFloating,
                        .isPinned = preview.window->m_pinned,
                    });
                }
            }
        }

        if (niriStripPreview && !previewState.windows.empty()) {
            for (auto& managed : previewState.windows)
                managed.previewAlpha = 1.0F;

            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] strip snapshot force opaque niri preview workspace=" << debugWorkspaceLabel(targetWorkspace)
                    << " windows=" << previewState.windows.size();
                debugLog(out.str());
            }
        }

        renderWorkspaceContents = !previewState.windows.empty();
    }

    if (!renderWorkspaceContents)
        return;

    if (renderWorkspaceContents && !renderWindowFn)
        return;

    const auto previousWorkspace = monitor->m_activeWorkspace;
    const auto previousSpecialWorkspace = monitor->m_activeSpecialWorkspace;
    const bool previousBlockSurfaceFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockScreenShader = g_pHyprRenderer->m_renderData.blockScreenShader;
    const bool previousRenderingSnapshot = g_pHyprRenderer->m_bRenderingSnapshot;
    struct WorkspaceRenderState {
        PHLWORKSPACE workspace;
        bool         visible = false;
        bool         forceRendering = false;
        Vector2D     renderOffsetValue;
        Vector2D     renderOffsetGoal;
        float        alphaValue = 1.0F;
        float        alphaGoal = 1.0F;
    };
    const auto captureWorkspaceRenderState = [](const PHLWORKSPACE& workspace) -> std::optional<WorkspaceRenderState> {
        if (!workspace)
            return std::nullopt;
        return WorkspaceRenderState{
            .workspace = workspace,
            .visible = workspace->m_visible,
            .forceRendering = workspace->m_forceRendering,
            .renderOffsetValue = workspace->m_renderOffset->value(),
            .renderOffsetGoal = workspace->m_renderOffset->goal(),
            .alphaValue = workspace->m_alpha->value(),
            .alphaGoal = workspace->m_alpha->goal(),
        };
    };
    const auto restoreWorkspaceRenderState = [](const std::optional<WorkspaceRenderState>& state) {
        if (!state || !state->workspace)
            return;

        state->workspace->m_visible = state->visible;
        state->workspace->m_forceRendering = state->forceRendering;
        state->workspace->m_renderOffset->setValueAndWarp(state->renderOffsetValue);
        if (state->renderOffsetGoal != state->renderOffsetValue)
            *state->workspace->m_renderOffset = state->renderOffsetGoal;
        state->workspace->m_alpha->setValueAndWarp(state->alphaValue);
        if (std::abs(state->alphaGoal - state->alphaValue) > 0.0001F)
            *state->workspace->m_alpha = state->alphaGoal;
    };
    const auto previousWorkspaceRenderState = previousWorkspace != targetWorkspace ? captureWorkspaceRenderState(previousWorkspace) : std::nullopt;
    const auto targetWorkspaceRenderState = targetWorkspace ? captureWorkspaceRenderState(targetWorkspace) : std::nullopt;
    bool targetVisibilityChanged = false;

    const auto applyFullscreenOverrideForState = [](State& state, bool suppress) {
        if (suppress) {
            if (!state.fullscreenOverrideActive)
                state.fullscreenOverrideActive = true;

            for (const auto& backup : state.fullscreenBackups) {
                if (!backup.workspace)
                    continue;

                backup.workspace->m_hasFullscreenWindow = false;
                backup.workspace->m_fullscreenMode = FSMODE_NONE;
                if (const auto workspaceMonitor = backup.workspace->m_monitor.lock())
                    workspaceMonitor->m_solitaryClient.reset();
            }

            return;
        }

        if (!state.fullscreenOverrideActive)
            return;

        for (const auto& backup : state.fullscreenBackups) {
            if (!backup.workspace)
                continue;

            backup.workspace->m_hasFullscreenWindow = backup.hadFullscreenWindow;
            backup.workspace->m_fullscreenMode = backup.fullscreenMode;
            if (const auto workspaceMonitor = backup.workspace->m_monitor.lock())
                workspaceMonitor->m_solitaryClient.reset();
        }

        state.fullscreenOverrideActive = false;
    };

    const auto renderBackgroundLayers = [&](const Time::steady_tp& now) {
        if (!m_renderLayerOriginal)
            return;

        for (const auto layerKind : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
            for (const auto& layer : g_pCompositor->m_layers) {
                if (!layer || !layer->m_mapped || layer->m_readyToDelete)
                    continue;

                const auto layerMonitor = layer->m_monitor.lock();
                if (!layerMonitor || layerMonitor != monitor || layer->m_layer != static_cast<int>(layerKind))
                    continue;

                m_renderLayerOriginal(g_pHyprRenderer.get(), layer, monitor, now, false, false);
            }
        }
    };

    const auto renderBackgroundLayersIntoFramebuffer = [&](const SP<Render::IFramebuffer>& targetFramebuffer, const Time::steady_tp& now) -> bool {
        if (!targetFramebuffer)
            return false;

        const bool previousBlockScreenShaderLocal = g_pHyprRenderer->m_renderData.blockScreenShader;
        CRegion     fakeDamage{0, 0, static_cast<int>(std::lround(targetFramebuffer->m_size.x)), static_cast<int>(std::lround(targetFramebuffer->m_size.y))};
        if (!g_pHyprRenderer->beginFullFakeRender(monitor, fakeDamage, targetFramebuffer)) {
            g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockScreenShaderLocal;
            return false;
        }

        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->draw(CClearPassElement::SClearData{.color = CHyprColor{0.05, 0.06, 0.08, 1.0}}, fakeDamage);
        renderBackgroundLayers(now);
        g_pHyprRenderer->endRender();
        g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockScreenShaderLocal;
        return true;
    };

    static std::size_t s_stripSnapshotProbeLogBudget = 260;
    const auto logStripSnapshotProbe = [&](const char* phase) {
        if (!debugLogsEnabled() || !targetWorkspace || !directNiriStripSnapshot || s_stripSnapshotProbeLogBudget == 0)
            return;

        const auto activeWorkspace = monitor ? monitor->m_activeWorkspace : PHLWORKSPACE{};
        {
            std::ostringstream out;
            out << "[hymission] strip snapshot probe phase=" << (phase ? phase : "?")
                << " workspace=" << debugWorkspaceLabel(targetWorkspace)
                << " activeWorkspace=" << debugWorkspaceLabel(activeWorkspace)
                << " targetVisible=" << (targetWorkspace->isVisible() ? 1 : 0)
                << " targetRawVisible=" << (targetWorkspace->m_visible ? 1 : 0)
                << " targetForceRendering=" << (targetWorkspace->m_forceRendering ? 1 : 0)
                << " entryWindows=" << entry.windows.size()
                << " previewWindows=" << previewState.windows.size()
                << " renderWorkspaceContents=" << (renderWorkspaceContents ? 1 : 0)
                << " snapshotFeedbackFrames=" << m_stripSnapshotSurfaceFeedbackFrames;
            debugLog(out.str());
            --s_stripSnapshotProbeLogBudget;
            if (s_stripSnapshotProbeLogBudget == 0)
                return;
        }

        auto* const scrolling = scrollingAlgorithmForWorkspace(targetWorkspace);
        if (scrolling && scrolling->m_scrollingData && s_stripSnapshotProbeLogBudget > 0) {
            std::ostringstream out;
            out << "[hymission] strip snapshot probe scrolling phase=" << (phase ? phase : "?")
                << " workspace=" << debugWorkspaceLabel(targetWorkspace)
                << " columns=" << scrolling->m_scrollingData->columns.size()
                << " offset=" << (scrolling->m_scrollingData->controller ? scrolling->m_scrollingData->controller->getOffset() : 0.0);
            for (std::size_t columnIndex = 0; columnIndex < scrolling->m_scrollingData->columns.size() && columnIndex < 8; ++columnIndex) {
                const auto& column = scrolling->m_scrollingData->columns[columnIndex];
                out << " | col#" << columnIndex;
                if (!column) {
                    out << "=<null>";
                    continue;
                }
                out << " width=" << column->getColumnWidth() << " targets=" << column->targetDatas.size();
                for (std::size_t targetIndex = 0; targetIndex < column->targetDatas.size() && targetIndex < 4; ++targetIndex) {
                    const auto& targetData = column->targetDatas[targetIndex];
                    const auto target = targetData ? targetData->target.lock() : SP<Layout::ITarget>{};
                    const auto window = target ? target->window() : PHLWINDOW{};
                    const CBox pos = target ? target->position() : CBox{};
                    const CBox layout = targetData ? targetData->layoutBox : CBox{};
                    out << " [" << targetIndex << "]" << debugWindowLabel(window)
                        << " pos=" << boxToString(pos)
                        << " layout=" << boxToString(layout);
                }
            }
            debugLog(out.str());
            --s_stripSnapshotProbeLogBudget;
            if (s_stripSnapshotProbeLogBudget == 0)
                return;
        }

        std::size_t loggedWindows = 0;
        for (const auto& window : g_pCompositor->m_windows) {
            if (!window || window->m_workspace != targetWorkspace || !window->m_isMapped || window->m_fadingOut)
                continue;
            if (loggedWindows >= 10 || s_stripSnapshotProbeLogBudget == 0)
                break;

            const auto target = window->layoutTarget();
            auto* const scrollingForWindow = scrollingAlgorithmForWorkspace(window->m_workspace);
            const auto targetData = scrollingForWindow && target ? scrollingForWindow->dataFor(target) : nullptr;
            const auto surface = window->wlSurface();
            const auto resource = surface ? surface->resource() : nullptr;
            const bool hasTexture = resource && static_cast<bool>(resource->m_current.texture);
            const Rect live = stateSnapshotGlobalRectForWindow(window, false);
            const Rect goal = stateSnapshotGlobalRectForWindow(window, true);
            std::ostringstream out;
            out << "[hymission] strip snapshot probe window phase=" << (phase ? phase : "?")
                << " window=" << debugWindowLabel(window)
                << " hidden=" << (window->isHidden() ? 1 : 0)
                << " mapped=" << (window->m_isMapped ? 1 : 0)
                << " wsVisible=" << (window->m_workspace && window->m_workspace->isVisible() ? 1 : 0)
                << " alpha=" << window->alphaTotal()
                << " live=" << rectToString(live)
                << " goal=" << rectToString(goal)
                << " targetPos=" << (target ? boxToString(target->position()) : std::string{"<null>"})
                << " layoutBox=" << (targetData ? boxToString(targetData->layoutBox) : std::string{"<null>"})
                << " texture=" << (hasTexture ? 1 : 0);
            if (resource)
                out << " surfSize=" << vectorToString(resource->m_current.size)
                    << " buffer=" << vectorToString(resource->m_current.bufferSize);
            else
                out << " surface=<null>";
            debugLog(out.str());
            --s_stripSnapshotProbeLogBudget;
            ++loggedWindows;
        }
    };

    struct WindowRenderGeometryBackup {
        PHLWINDOW window;
        Vector2D  positionValue;
        Vector2D  positionGoal;
        Vector2D  sizeValue;
        Vector2D  sizeGoal;
    };

    std::vector<WindowRenderGeometryBackup> windowGeometryBackups;

    const auto restoreWindowRenderGeometry = [&]() {
        for (const auto& backup : windowGeometryBackups) {
            if (!backup.window || !backup.window->m_realPosition || !backup.window->m_realSize)
                continue;

            backup.window->m_realPosition->setValueAndWarp(backup.positionValue);
            if (backup.positionGoal != backup.positionValue)
                *backup.window->m_realPosition = backup.positionGoal;

            backup.window->m_realSize->setValueAndWarp(backup.sizeValue);
            if (backup.sizeGoal != backup.sizeValue)
                *backup.window->m_realSize = backup.sizeGoal;

            backup.window->updateWindowDecos();
        }
        windowGeometryBackups.clear();
    };

    bool forcedWindowGeometryForStripSnapshot = false;
    const auto forceUsableWindowGeometryForStripSnapshot = [&]() {
        if (!renderWorkspaceContents || !targetWorkspace || !directNiriStripSnapshot || !m_stripPreviewContext.active)
            return;

        windowGeometryBackups.reserve(m_stripPreviewContext.state.windows.size());
        for (const auto& managed : m_stripPreviewContext.state.windows) {
            const auto& window = managed.window;
            if (!window || !window->m_realPosition || !window->m_realSize || window->m_pinned || window->onSpecialWorkspace())
                continue;

            Rect source = managed.naturalGlobal;
            if (source.width <= 1.0 || source.height <= 1.0)
                source = managed.exitGlobal;
            if (source.width <= 1.0 || source.height <= 1.0)
                source = managed.targetGlobal;
            if (source.width <= 1.0 || source.height <= 1.0)
                continue;

            const Vector2D currentSize = window->m_realSize->value();
            const Vector2D goalSize = window->m_realSize->goal();
            const bool unusableLive = currentSize.x <= 1.0 || currentSize.y <= 1.0;
            const bool unusableGoal = goalSize.x <= 1.0 || goalSize.y <= 1.0;
            const bool inactiveWorkspace = window->m_workspace && !window->m_workspace->isVisible();
            if (!inactiveWorkspace && !unusableLive && !unusableGoal)
                continue;

            windowGeometryBackups.push_back({
                .window = window,
                .positionValue = window->m_realPosition->value(),
                .positionGoal = window->m_realPosition->goal(),
                .sizeValue = currentSize,
                .sizeGoal = goalSize,
            });

            const Vector2D forcedPosition{source.x, source.y};
            const Vector2D forcedSize{std::max(1.0, source.width), std::max(1.0, source.height)};
            window->m_realPosition->setValueAndWarp(forcedPosition);
            *window->m_realPosition = forcedPosition;
            window->m_realSize->setValueAndWarp(forcedSize);
            *window->m_realSize = forcedSize;
            window->updateWindowDecos();
            forcedWindowGeometryForStripSnapshot = true;

            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] strip snapshot force window geometry window=" << debugWindowLabel(window)
                    << " workspace=" << debugWorkspaceLabel(window->m_workspace)
                    << " live=" << vectorToString(currentSize)
                    << " goal=" << vectorToString(goalSize)
                    << " forced=" << rectToString(source);
                debugLog(out.str());
            }
        }
    };

    logStripSnapshotProbe("before-render");

    ++m_stripSnapshotRenderDepth;
    g_pHyprOpenGL->makeEGLCurrent();
    g_pHyprRenderer->m_bBlockSurfaceFeedback = directNiriStripSnapshot ? false : (m_stripSnapshotSurfaceFeedbackFrames == 0);
    g_pHyprRenderer->m_bRenderingSnapshot = false;
    if (renderWorkspaceContents) {
        m_stripPreviewContext.active = true;
        m_stripPreviewContext.monitor = monitor;
        m_stripPreviewContext.state = std::move(previewState);
        m_stripPreviewContext.framebufferSize = Vector2D{static_cast<double>(fbWidth), static_cast<double>(fbHeight)};
        applyFullscreenOverrideForState(m_stripPreviewContext.state, true);
    }

    if (renderWorkspaceContents && targetWorkspace)
        monitor->m_activeSpecialWorkspace.reset();

    if (renderWorkspaceContents && targetWorkspace && targetWorkspace->m_monitor.lock() != monitor) {
        applyFullscreenOverrideForState(m_stripPreviewContext.state, false);
        m_stripPreviewContext.state = {};
        m_stripPreviewContext.framebufferSize = {};
        m_stripPreviewContext.monitor.reset();
        m_stripPreviewContext.active = false;
        g_pHyprRenderer->m_bRenderingSnapshot = previousRenderingSnapshot;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockSurfaceFeedback;
        --m_stripSnapshotRenderDepth;
        return;
    }

    if (renderWorkspaceContents && targetWorkspace) {
        monitor->m_activeWorkspace = targetWorkspace;
        if (!targetWorkspace->m_visible) {
            targetWorkspace->m_visible = true;
            targetVisibilityChanged = true;
        }
        targetWorkspace->m_forceRendering = true;
        targetWorkspace->m_renderOffset->setValueAndWarp(Vector2D{});
        *targetWorkspace->m_renderOffset = Vector2D{};
        targetWorkspace->m_alpha->setValueAndWarp(1.F);
        *targetWorkspace->m_alpha = 1.F;
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip snapshot force workspace render state workspace=" << debugWorkspaceLabel(targetWorkspace)
                << " visible=" << (targetWorkspaceRenderState ? (targetWorkspaceRenderState->visible ? 1 : 0) : -1)
                << " forceRendering=" << (targetWorkspaceRenderState ? (targetWorkspaceRenderState->forceRendering ? 1 : 0) : -1)
                << " alpha=" << (targetWorkspaceRenderState ? targetWorkspaceRenderState->alphaValue : -1.0F);
            debugLog(out.str());
        }
    }

    forceUsableWindowGeometryForStripSnapshot();
    if (forcedWindowGeometryForStripSnapshot)
        m_stripSnapshotSurfaceFeedbackFrames = std::max<std::size_t>(m_stripSnapshotSurfaceFeedbackFrames, 8);

    const auto renderNow = Time::steadyNow();
    bool       renderedScaledBackgroundOnly = false;
    if (!renderWorkspaceContents) {
        const int backgroundFbWidth = std::max(1, static_cast<int>(std::ceil(static_cast<double>(monitor->m_size.x) * renderScaleForMonitor(monitor))));
        const int backgroundFbHeight = std::max(1, static_cast<int>(std::ceil(static_cast<double>(monitor->m_size.y) * renderScaleForMonitor(monitor))));
        auto      backgroundFramebuffer = createFramebuffer("hymission workspace strip background");
        if (backgroundFramebuffer && backgroundFramebuffer->alloc(backgroundFbWidth, backgroundFbHeight)) {
            backgroundFramebuffer->setImageDescription(monitor->workBufferImageDescription());
            setFramebufferLinearFiltering(*backgroundFramebuffer);
            renderedScaledBackgroundOnly =
                renderBackgroundLayersIntoFramebuffer(backgroundFramebuffer, renderNow) &&
                blitFramebufferRegion(*backgroundFramebuffer, *snapshot->framebuffer, makeRect(0.0, 0.0, backgroundFramebuffer->m_size.x, backgroundFramebuffer->m_size.y),
                                     makeRect(0.0, 0.0, snapshot->framebuffer->m_size.x, snapshot->framebuffer->m_size.y));
        }
    }

    if (!renderedScaledBackgroundOnly) {
        SP<Render::IFramebuffer> renderFramebuffer = snapshot->framebuffer;
        bool                     blitRenderedFramebuffer = false;
        if (renderWorkspaceContents) {
            const int renderFbWidth = std::max(1, static_cast<int>(std::ceil(static_cast<double>(monitor->m_size.x) * renderScaleForMonitor(monitor))));
            const int renderFbHeight = std::max(1, static_cast<int>(std::ceil(static_cast<double>(monitor->m_size.y) * renderScaleForMonitor(monitor))));
            auto      fullSizeFramebuffer = createFramebuffer("hymission workspace strip full snapshot");
            if (fullSizeFramebuffer && fullSizeFramebuffer->alloc(renderFbWidth, renderFbHeight)) {
                fullSizeFramebuffer->setImageDescription(monitor->workBufferImageDescription());
                setFramebufferLinearFiltering(*fullSizeFramebuffer);
                renderFramebuffer = fullSizeFramebuffer;
                blitRenderedFramebuffer = true;
                m_stripPreviewContext.framebufferSize = Vector2D{fullSizeFramebuffer->m_size.x, fullSizeFramebuffer->m_size.y};
            }
        }

        CRegion fakeDamage{0, 0, static_cast<int>(std::lround(renderFramebuffer->m_size.x)), static_cast<int>(std::lround(renderFramebuffer->m_size.y))};
        g_pHyprRenderer->beginFullFakeRender(monitor, fakeDamage, renderFramebuffer);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{.color = CHyprColor{0.05, 0.06, 0.08, 1.0}}, fakeDamage);
        renderBackgroundLayers(renderNow);
        if (renderWorkspaceContents && targetWorkspace && renderWindowFn) {
            const auto renderPreviewWindows = [&](bool floating) {
                for (const auto& managed : m_stripPreviewContext.state.windows) {
                    if (!managed.window || managed.isFloating != floating || !windowMatchesOverviewScope(managed.window, m_stripPreviewContext.state, false))
                        continue;

                    renderWindowFn(g_pHyprRenderer.get(), managed.window, monitor, renderNow, false, Render::RENDER_PASS_ALL, false, true);
                }
            };
            renderPreviewWindows(false);
            renderPreviewWindows(true);

            // Hyprland's normal window render path can draw decos/borders for an
            // inactive scrolling workspace while still producing an empty client
            // surface when that workspace has never been made visible.  Niri does
            // not depend on hidden-workspace compositor visibility for overview:
            // it renders from the window/layout state.  For direct-Niri strip
            // snapshots, mirror that by drawing the current main surface texture
            // into the preview rect directly as a fallback layer.
            if (directNiriStripSnapshot && g_pHyprOpenGL) {
                std::size_t directSurfaceRendered = 0;
                std::size_t directSurfaceMissing = 0;
                static std::size_t s_directSurfaceLogBudget = 180;

                for (const auto& managed : m_stripPreviewContext.state.windows) {
                    if (!managed.window || !windowMatchesOverviewScope(managed.window, m_stripPreviewContext.state, false))
                        continue;

                    const auto surface = managed.window->wlSurface();
                    const auto resource = surface ? surface->resource() : nullptr;
                    const auto texture = resource ? resource->m_current.texture : nullptr;

                    const Rect targetGlobal = snapRectToRenderPixelGrid(managed.targetGlobal, monitor);
                    const Rect targetLocal = rectToMonitorRenderLocal(targetGlobal, monitor);
                    const bool usableTarget = targetLocal.width > 1.0 && targetLocal.height > 1.0;

                    if (!texture || !usableTarget) {
                        ++directSurfaceMissing;
                        if (debugLogsEnabled() && s_directSurfaceLogBudget > 0) {
                            std::ostringstream out;
                            out << "[hymission] strip snapshot direct surface missing"
                                << " window=" << debugWindowLabel(managed.window)
                                << " workspace=" << debugWorkspaceLabel(managed.window->m_workspace)
                                << " texture=" << (texture ? 1 : 0)
                                << " target=" << rectToString(targetGlobal)
                                << " targetLocal=" << rectToString(targetLocal)
                                << " hidden=" << (managed.window->isHidden() ? 1 : 0)
                                << " mapped=" << (managed.window->m_isMapped ? 1 : 0)
                                << " fading=" << (managed.window->m_fadingOut ? 1 : 0);
                            if (resource)
                                out << " surfSize=" << vectorToString(resource->m_current.size)
                                    << " buffer=" << vectorToString(resource->m_current.bufferSize);
                            else
                                out << " surface=<null>";
                            debugLog(out.str());
                            --s_directSurfaceLogBudget;
                        }
                        continue;
                    }

                    const float alpha = std::clamp(managedPreviewAlphaFor(managed.window, managed.previewAlpha), 0.0F, 1.0F);
                    g_pHyprOpenGL->renderTexture(texture, toBox(targetLocal), {.a = alpha});
                    ++directSurfaceRendered;

                    if (debugLogsEnabled() && s_directSurfaceLogBudget > 0) {
                        std::ostringstream out;
                        out << "[hymission] strip snapshot direct surface render"
                            << " window=" << debugWindowLabel(managed.window)
                            << " workspace=" << debugWorkspaceLabel(managed.window->m_workspace)
                            << " target=" << rectToString(targetGlobal)
                            << " targetLocal=" << rectToString(targetLocal)
                            << " alpha=" << alpha
                            << " hidden=" << (managed.window->isHidden() ? 1 : 0)
                            << " mapped=" << (managed.window->m_isMapped ? 1 : 0)
                            << " visibleWs=" << (managed.window->m_workspace && managed.window->m_workspace->isVisible() ? 1 : 0);
                        if (resource)
                            out << " surfSize=" << vectorToString(resource->m_current.size)
                                << " buffer=" << vectorToString(resource->m_current.bufferSize);
                        debugLog(out.str());
                        --s_directSurfaceLogBudget;
                    }
                }

                if (debugLogsEnabled() && s_directSurfaceLogBudget > 0) {
                    std::ostringstream out;
                    out << "[hymission] strip snapshot direct surface summary"
                        << " workspace=" << debugWorkspaceLabel(targetWorkspace)
                        << " rendered=" << directSurfaceRendered
                        << " missing=" << directSurfaceMissing
                        << " activeWorkspace=" << debugWorkspaceLabel(previousWorkspace)
                        << " targetWasVisible=" << (targetWorkspaceRenderState && targetWorkspaceRenderState->visible ? 1 : 0);
                    debugLog(out.str());
                    --s_directSurfaceLogBudget;
                }
            }

            renderInactiveWindowBorders(m_stripPreviewContext.state, 1.0, true);
            renderFocusedWindowBorder(m_stripPreviewContext.state, 1.0, true);
        }
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();
        g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockScreenShader;

        if (blitRenderedFramebuffer) {
            blitFramebufferRegion(*renderFramebuffer, *snapshot->framebuffer, makeRect(0.0, 0.0, renderFramebuffer->m_size.x, renderFramebuffer->m_size.y),
                                  makeRect(0.0, 0.0, snapshot->framebuffer->m_size.x, snapshot->framebuffer->m_size.y));
        }
    }
    logStripSnapshotProbe("after-render");
        restoreWindowRenderGeometry();

    if (renderWorkspaceContents) {
        applyFullscreenOverrideForState(m_stripPreviewContext.state, false);
        m_stripPreviewContext.state = {};
        m_stripPreviewContext.framebufferSize = {};
        m_stripPreviewContext.monitor.reset();
        m_stripPreviewContext.active = false;
    }

    if (targetVisibilityChanged && targetWorkspace)
        targetWorkspace->m_visible = false;

    if (renderWorkspaceContents && targetWorkspace) {
        monitor->m_activeSpecialWorkspace = previousSpecialWorkspace;
        monitor->m_activeWorkspace = previousWorkspace;
    }

    restoreWorkspaceRenderState(previousWorkspaceRenderState);
    restoreWorkspaceRenderState(targetWorkspaceRenderState);

    g_pHyprRenderer->m_bRenderingSnapshot = previousRenderingSnapshot;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockSurfaceFeedback;
    --m_stripSnapshotRenderDepth;

    entry.snapshot = std::move(snapshot);
}

void OverviewController::refreshWorkspaceStripSnapshots() {
    if (!workspaceStripEnabled(m_state) || m_state.stripEntries.empty()) {
        for (auto& entry : m_state.stripEntries)
            entry.snapshot.reset();
        m_stripSnapshotsDirty = false;
        m_stripSnapshotSurfaceFeedbackFrames = 0;
        m_lastStripThemeColorValid = false;
        return;
    }

    if (m_stripSnapshotRenderDepth > 0 || (g_pHyprOpenGL && g_pHyprRenderer->m_renderData.pMonitor)) {
        m_stripSnapshotsDirty = true;
        scheduleWorkspaceStripSnapshotRefresh();
        return;
    }

    m_stripSnapshotsDirty = false;
    for (auto& entry : m_state.stripEntries)
        renderWorkspaceStripSnapshot(entry);

    if (m_stripSnapshotSurfaceFeedbackFrames > 0)
        --m_stripSnapshotSurfaceFeedbackFrames;
}

void OverviewController::scheduleWorkspaceStripSnapshotRefresh() {
    if (m_stripSnapshotRefreshScheduled || !g_pEventLoopManager)
        return;

    m_stripSnapshotRefreshScheduled = true;
    const auto refresh = [this] {
        if (g_controller != this)
            return;

        m_stripSnapshotRefreshScheduled = false;
        if (!m_stripSnapshotsDirty)
            return;

        refreshWorkspaceStripSnapshots();
    };

    if (m_stripSnapshotSurfaceFeedbackFrames > 0) {
        if (!m_stripSnapshotRefreshTimer) {
            m_stripSnapshotRefreshTimer = makeShared<CEventLoopTimer>(
                THEME_SURFACE_FEEDBACK_INTERVAL,
                [this, refresh](SP<CEventLoopTimer> self, void*) {
                    self->updateTimeout(std::nullopt);
                    refresh();
                },
                nullptr);
            g_pEventLoopManager->addTimer(m_stripSnapshotRefreshTimer);
            return;
        }

        m_stripSnapshotRefreshTimer->updateTimeout(THEME_SURFACE_FEEDBACK_INTERVAL);
        return;
    }

    g_pEventLoopManager->doLater([this] {
        if (g_controller != this)
            return;

        m_stripSnapshotRefreshScheduled = false;
        if (!m_stripSnapshotsDirty)
            return;

        refreshWorkspaceStripSnapshots();
    });
}

void OverviewController::renderWorkspaceStrip() const {
    const double progress = visualProgress();
    if (progress <= 0.0 || m_state.stripEntries.empty())
        return;

    const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!renderMonitor)
        return;

    if (debugLogsEnabled()) {
        static int debugRenderSamples = 0;
        if (debugRenderSamples < 4) {
            std::ostringstream out;
            out << "[hymission] strip render progress=" << progress << " entries=" << m_state.stripEntries.size();
            if (!m_state.stripEntries.empty())
                out << " firstRect=" << rectToString(m_state.stripEntries.front().rect);
            debugLog(out.str());
            ++debugRenderSamples;
        }
    }

    // Check if a workspace transition is active and applies to this monitor.
    // During a workspace transition, the strip entries should slide with the transition delta.
    const bool hasWorkspaceTransition = m_workspaceTransition.active &&
        m_workspaceTransition.monitor && m_workspaceTransition.monitor == renderMonitor;
    double workspaceTransitionOffset = 0.0;
    if (hasWorkspaceTransition) {
        const double clampedDelta = std::clamp(m_workspaceTransition.delta, -m_workspaceTransition.distance, m_workspaceTransition.distance);
        const double sourceOffset = -clampedDelta;
        const int targetDirection =
            clampedDelta < -0.0001 ? -1 : clampedDelta > 0.0001 ? 1 : (m_workspaceTransition.step < 0 ? -1 : 1);
        const double targetOffset = sourceOffset + static_cast<double>(targetDirection) * m_workspaceTransition.distance;
        const double t = m_workspaceTransition.distance > 0.0 ? clampUnit(std::abs(clampedDelta) / m_workspaceTransition.distance) : 1.0;
        if (m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical)
            workspaceTransitionOffset = sourceOffset + (targetOffset - sourceOffset) * t;
        else
            workspaceTransitionOffset = sourceOffset + (targetOffset - sourceOffset) * t;
    }

    const Rect bandGlobal = workspaceStripBandRectForMonitor(renderMonitor, m_state);
    if (bandGlobal.width > 0.0 && bandGlobal.height > 0.0) {
        const Rect band = rectToMonitorRenderLocal(animatedWorkspaceStripRect(bandGlobal, renderMonitor), renderMonitor);
        g_pHyprOpenGL->renderRect(toBox(band), CHyprColor(0.03, 0.07, 0.14, 0.24 * progress), {.blur = true, .blurA = 1.0F});
    }

    for (std::size_t index = 0; index < m_state.stripEntries.size(); ++index) {
        const auto& entry = m_state.stripEntries[index];
        if (!entry.monitor || entry.monitor != renderMonitor)
            continue;

        const bool hovered = m_state.hoveredStripIndex && *m_state.hoveredStripIndex == index;
        // Apply workspace transition offset during active transition
        Rect outerGlobal = currentWorkspaceStripRect(entry);
        if (hasWorkspaceTransition) {
            if (m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical)
                outerGlobal = translateRect(outerGlobal, 0.0, workspaceTransitionOffset);
            else
                outerGlobal = translateRect(outerGlobal, workspaceTransitionOffset, 0.0);
        }
        outerGlobal = animatedWorkspaceStripRect(outerGlobal, renderMonitor);
        const Rect outer = rectToMonitorLocal(outerGlobal, renderMonitor);
        if (outer.width <= 0.0 || outer.height <= 0.0)
            continue;

        const Rect thumb = rectToMonitorLocal(outerGlobal, renderMonitor);
        const Rect thumbRender = scaleRectForRender(thumb, renderMonitor);

        const CHyprColor cardColor = entry.newWorkspaceSlot ? CHyprColor(0.11, 0.16, 0.23, 0.26 * progress) :
                                      entry.syntheticEmpty ? CHyprColor(0.06, 0.10, 0.16, 0.18 * progress) :
                                      entry.active ? CHyprColor(0.10, 0.18, 0.32, 0.24 * progress) :
                                                     CHyprColor(0.05, 0.09, 0.15, 0.18 * progress);
        const CHyprColor stateOverlayColor =
            hovered ? CHyprColor(1.0, 1.0, 1.0, 0.06 * progress) : entry.active ? CHyprColor(0.34, 0.58, 0.95, 0.10 * progress) : CHyprColor(0.0, 0.0, 0.0, 0.0);

        g_pHyprOpenGL->renderRect(toBox(thumbRender), cardColor, {.blur = true, .blurA = 1.0F});

        if (!entry.newWorkspaceSlot && entry.snapshot && entry.snapshot->framebuffer && entry.snapshot->framebuffer->isAllocated() && entry.snapshot->framebuffer->getTexture()) {
            g_pHyprOpenGL->renderTexture(entry.snapshot->framebuffer->getTexture(), toBox(thumbRender), {.a = static_cast<float>(std::clamp(progress, 0.0, 1.0))});
        }

        // Direct-Niri strip thumbnails cannot rely exclusively on cached fake-render
        // snapshots.  Inactive scrolling workspaces can have correct Hyprland
        // layout boxes while the snapshot render path still returns a stale/blank
        // client surface until a real workspace switch.  Niri avoids this by
        // rendering overview windows from its current layout/window state each
        // frame.  Mirror that here by overlaying the current wl_surface texture
        // for every managed window that belongs to this strip entry.
        if (!entry.newWorkspaceSlot && entry.workspace && niriModeEnabled() && m_state.collectionPolicy.onlyActiveWorkspace &&
            isScrollingWorkspace(entry.workspace) && !entry.workspace->isVisible() && g_pHyprOpenGL) {
            std::size_t liveSurfaceRendered = 0;
            std::size_t liveSurfaceMissing = 0;
            static std::size_t s_liveStripSurfaceLogBudget = 240;

            for (const auto& managed : m_state.windows) {
                if (!managed.window || managed.window->m_workspace != entry.workspace || !windowMatchesOverviewScope(managed.window, m_state, false))
                    continue;

                const auto surface = managed.window->wlSurface();
                const auto resource = surface ? surface->resource() : nullptr;
                const auto texture = resource ? resource->m_current.texture : nullptr;

                Rect windowGlobal = currentPreviewRect(managed);
                if (hasWorkspaceTransition) {
                    if (m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical)
                        windowGlobal = translateRect(windowGlobal, 0.0, workspaceTransitionOffset);
                    else
                        windowGlobal = translateRect(windowGlobal, workspaceTransitionOffset, 0.0);
                }

                // Keep this fallback clipped to the strip card.  The direct
                // renderer intentionally only covers the client/content region;
                // the cached snapshot below/normal border pass still provide the
                // card, decorations and workspace label.
                if (!rectsOverlap(windowGlobal, outerGlobal))
                    continue;

                const Rect targetGlobal = snapRectToRenderPixelGrid(windowGlobal, renderMonitor);
                const Rect targetRender = rectToMonitorRenderLocal(targetGlobal, renderMonitor);
                const bool usableTarget = targetRender.width > 1.0 && targetRender.height > 1.0;

                if (!texture || !usableTarget) {
                    ++liveSurfaceMissing;
                    if (debugLogsEnabled() && s_liveStripSurfaceLogBudget > 0) {
                        std::ostringstream out;
                        out << "[hymission] strip live surface missing"
                            << " window=" << debugWindowLabel(managed.window)
                            << " workspace=" << debugWorkspaceLabel(entry.workspace)
                            << " texture=" << (texture ? 1 : 0)
                            << " target=" << rectToString(targetGlobal)
                            << " targetRender=" << rectToString(targetRender)
                            << " hidden=" << (managed.window->isHidden() ? 1 : 0)
                            << " mapped=" << (managed.window->m_isMapped ? 1 : 0)
                            << " visibleWs=" << (managed.window->m_workspace && managed.window->m_workspace->isVisible() ? 1 : 0);
                        if (resource)
                            out << " surfSize=" << vectorToString(resource->m_current.size)
                                << " buffer=" << vectorToString(resource->m_current.bufferSize);
                        else
                            out << " surface=<null>";
                        debugLog(out.str());
                        --s_liveStripSurfaceLogBudget;
                    }
                    continue;
                }

                const float alpha = std::clamp(managedPreviewAlphaFor(managed.window, managed.previewAlpha), 0.0F, 1.0F) *
                    static_cast<float>(std::clamp(progress, 0.0, 1.0));
                g_pHyprOpenGL->renderTexture(texture, toBox(targetRender), {.a = alpha});
                ++liveSurfaceRendered;

                if (debugLogsEnabled() && s_liveStripSurfaceLogBudget > 0) {
                    std::ostringstream out;
                    out << "[hymission] strip live surface render"
                        << " window=" << debugWindowLabel(managed.window)
                        << " workspace=" << debugWorkspaceLabel(entry.workspace)
                        << " target=" << rectToString(targetGlobal)
                        << " targetRender=" << rectToString(targetRender)
                        << " alpha=" << alpha
                        << " hidden=" << (managed.window->isHidden() ? 1 : 0)
                        << " mapped=" << (managed.window->m_isMapped ? 1 : 0)
                        << " visibleWs=" << (managed.window->m_workspace && managed.window->m_workspace->isVisible() ? 1 : 0);
                    if (resource)
                        out << " surfSize=" << vectorToString(resource->m_current.size)
                            << " buffer=" << vectorToString(resource->m_current.bufferSize);
                    debugLog(out.str());
                    --s_liveStripSurfaceLogBudget;
                }
            }

            if (debugLogsEnabled() && s_liveStripSurfaceLogBudget > 0) {
                std::ostringstream out;
                out << "[hymission] strip live surface summary"
                    << " workspace=" << debugWorkspaceLabel(entry.workspace)
                    << " rendered=" << liveSurfaceRendered
                    << " missing=" << liveSurfaceMissing
                    << " entryActive=" << (entry.active ? 1 : 0)
                    << " snapshot=" << (entry.snapshot && entry.snapshot->framebuffer && entry.snapshot->framebuffer->isAllocated() ? 1 : 0)
                    << " windows=" << m_state.windows.size();
                debugLog(out.str());
                --s_liveStripSurfaceLogBudget;
            }
        }

        if (stateOverlayColor.a > 0.0) {
            g_pHyprOpenGL->renderRect(toBox(thumbRender), stateOverlayColor, {});
        }

        if (entry.active && !entry.newWorkspaceSlot) {
            renderOutline(outerGlobal, activeBorderGradient(), 2.0, 0.92 * progress);
        }

        if (!entry.newWorkspaceSlot) {
            const std::string label = entry.workspaceName.empty() ? std::to_string(entry.workspaceId) : entry.workspaceName;
            const double      labelOpacity = hovered ? 1.0 : workspaceStripLabelOpacity();
            const auto        labelTexture =
                g_pHyprRenderer->renderText(label, activeBorderColorWithAlpha(labelOpacity * progress), scaleFontSizeForRender(renderMonitor, workspaceStripLabelFontSize()),
                                            false, "", static_cast<int>(std::max(1.0, thumbRender.width * 0.86)));
            if (labelTexture) {
                const Rect labelRect = makeRect(thumbRender.centerX() - labelTexture->m_size.x * 0.5, thumbRender.centerY() - labelTexture->m_size.y * 0.5,
                                                labelTexture->m_size.x, labelTexture->m_size.y);
                g_pHyprOpenGL->renderTexture(labelTexture, toBox(labelRect), {});
            }
        }

        if (entry.newWorkspaceSlot) {
            const double plusArmLength = std::min(thumb.width, thumb.height) * 0.28;
            const double plusThickness = std::max(2.0, std::round(std::min(thumb.width, thumb.height) * 0.03));
            const Rect plusHorizontal =
                makeRect(thumb.centerX() - plusArmLength * 0.5, thumb.centerY() - plusThickness * 0.5, plusArmLength, plusThickness);
            const Rect plusVertical =
                makeRect(thumb.centerX() - plusThickness * 0.5, thumb.centerY() - plusArmLength * 0.5, plusThickness, plusArmLength);
            const CHyprColor plusColor = activeBorderColorWithAlpha(0.88 * progress);
            g_pHyprOpenGL->renderRect(toBox(scaleRectForRender(plusHorizontal, renderMonitor)), plusColor, {});
            g_pHyprOpenGL->renderRect(toBox(scaleRectForRender(plusVertical, renderMonitor)), plusColor, {});
        }
    }
}


bool OverviewController::selectWindowInState(State& state, const PHLWINDOW& window) const {
    if (!window)
        return false;

    const auto selectedIt = std::find_if(state.windows.begin(), state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == window; });
    if (selectedIt == state.windows.end())
        return false;

    state.selectedIndex = static_cast<std::size_t>(std::distance(state.windows.begin(), selectedIt));
    state.focusDuringOverview = window;
    return true;
}


} // namespace hymission
