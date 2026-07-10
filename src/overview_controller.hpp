#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/supplementary/WorkspaceAlgoMatcher.hpp>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "mission_layout.hpp"
#include "overview_drag_logic.hpp"
#include "overview_logic.hpp"

class CEventLoopTimer;
namespace Config {
    class CGradientValueData;
}

namespace hymission {

class OverviewOverlayPassElement;
class OverviewWallpaperPassElement;

class OverviewController {
  public:
    enum class WindowSetChangeKind {
        General,
        Open,
        MoveToWorkspace,
    };

    enum class ScopeOverride {
        Default,
        OnlyCurrentWorkspace,
        ForceAll,
    };

    explicit OverviewController(HANDLE handle);
    ~OverviewController();

    OverviewController(const OverviewController&) = delete;
    OverviewController& operator=(const OverviewController&) = delete;

    bool initialize();

    [[nodiscard]] SDispatchResult open(const std::string& args = {});
    [[nodiscard]] SDispatchResult close();
    [[nodiscard]] SDispatchResult toggle(const std::string& args = {});
    [[nodiscard]] SDispatchResult debugCurrentLayout() const;
    [[nodiscard]] bool            allowsWorkspaceSwitchInOverviewForGestures() const;
    [[nodiscard]] bool            blocksWorkspaceSwitchInOverviewForGestures() const;
    [[nodiscard]] bool            beginOverviewWorkspaceSwipeGesture(eTrackpadGestureDirection direction);
    void                          updateOverviewWorkspaceSwipeGesture(double delta);
    void                          setOverviewWorkspaceSwipeGestureDelta(double delta);
    void                          endOverviewWorkspaceSwipeGesture(bool cancelled);
    [[nodiscard]] bool            beginScrollGesture(HymissionScrollMode mode, eTrackpadGestureDirection direction,
                                                     const IPointer::SSwipeUpdateEvent& event, float deltaScale);
    void                          updateScrollGesture(const IPointer::SSwipeUpdateEvent& event);
    void                          endScrollGesture(bool cancelled);
    void                          refreshAfterOfficialScrollMove(const char* source);

    void renderStage(eRenderStage stage);
    void handleConfigReloaded();
    void handleMouseMove();
    bool handleMouseButton(const IPointer::SButtonEvent& event);
    void handleKeyboard(const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info);
    void handleWindowSetChange(PHLWINDOW window, WindowSetChangeKind kind = WindowSetChangeKind::General, bool preferDeferredRebuild = false);
    void handleWorkspaceChange(PHLWORKSPACE workspace);
    void handleMonitorChange(PHLMONITOR monitor);
    bool                shouldRenderWindowHook(const PHLWINDOW& window, const PHLMONITOR& monitor);
    void                borderDrawHook(void* borderDecorationThisptr, const PHLMONITOR& monitor, const float& alpha);
    void                shadowDrawHook(void* shadowDecorationThisptr, const PHLMONITOR& monitor, const float& alpha);
    void                calculateUVForSurfaceHook(const PHLWINDOW& window, SP<CWLSurfaceResource> surface, const PHLMONITOR& monitor, bool main, const Vector2D& projSize,
                                                  const Vector2D& projSizeUnscaled, bool fixMisalignedFSV1);
    void                renderLayerHook(void* rendererThisptr, PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& now, bool popups, bool lockscreen);
    [[nodiscard]] SDispatchResult fullscreenDispatcherHook(std::string args);
    [[nodiscard]] SDispatchResult fullscreenStateDispatcherHook(std::string args);
    [[nodiscard]] SDispatchResult changeWorkspaceDispatcherHook(std::string args);
    [[nodiscard]] SDispatchResult focusWorkspaceOnCurrentMonitorDispatcherHook(std::string args);
    [[nodiscard]] SDispatchResult layoutMessageDispatcherHook(std::string args);
    [[nodiscard]] SDispatchResult moveFocusDispatcherHook(std::string args);
    [[nodiscard]] std::optional<Config::Actions::ActionResult> layoutMessageActionHook(const std::string& msg);
    [[nodiscard]] std::optional<Config::Actions::ActionResult> floatWindowActionHook(Config::Actions::eTogglableAction action,
                                                                                    std::optional<PHLWINDOW> window);
    [[nodiscard]] Config::Actions::ActionResult moveToWorkspaceActionHook(PHLWORKSPACE workspace, bool silent, std::optional<PHLWINDOW> window);
    bool                surfaceNeedsLiveBlurHook(void* surfacePassThisptr);
    bool                surfaceNeedsPrecomputeBlurHook(void* surfacePassThisptr);
    std::vector<UP<IPassElement>> surfaceDrawHook(void* surfacePassThisptr);
    CBox                surfaceTexBoxHook(void* surfacePassThisptr);
    std::optional<CBox> surfaceBoundingBoxHook(void* surfacePassThisptr);
    CRegion             surfaceOpaqueRegionHook(void* surfacePassThisptr);
    CRegion             surfaceVisibleRegionHook(void* surfacePassThisptr, bool& cancel);
    std::optional<std::string> handleGestureConfigHook(const std::string& keyword, const std::string& value);
    [[nodiscard]] bool         beginTrackpadGesture(bool openOnly, ScopeOverride requestedScope, bool recommand, eTrackpadGestureDirection direction,
                                                    const IPointer::SSwipeUpdateEvent& event, float deltaScale);
    void                       updateTrackpadGesture(const IPointer::SSwipeUpdateEvent& event);
    void                       endTrackpadGesture(bool cancelled);
    void                       workspaceSwipeBeginHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureBegin& e);
    void                       workspaceSwipeUpdateHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureUpdate& e);
    void                       workspaceSwipeEndHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureEnd& e);
    void                       unifiedWorkspaceSwipeBeginHook(void* gestureThisptr);
    void                       unifiedWorkspaceSwipeUpdateHook(void* gestureThisptr, double delta);
    void                       unifiedWorkspaceSwipeEndHook(void* gestureThisptr);
    [[nodiscard]] bool         handleTouchDown(const ITouch::SDownEvent& event);
    [[nodiscard]] bool         handleTouchMotion(const ITouch::SMotionEvent& event);
    [[nodiscard]] bool         handleTouchUp(const ITouch::SUpEvent& event, bool cancelled = false);
    void                       scrollMoveGestureBeginHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureBegin& e);
    void                       scrollMoveGestureUpdateHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureUpdate& e);
    void                       scrollMoveGestureEndHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureEnd& e);
  private:
    friend class OverviewOverlayPassElement;
    friend class OverviewWallpaperPassElement;

    enum class Phase {
        Inactive,
        Opening,
        Active,
        ClosingSettle,
        Closing,
    };

    enum class CloseMode {
        Normal,
        ActivateSelection,
        Abort,
    };

    enum class PostCloseDispatcher {
        None,
        Fullscreen,
        FullscreenState,
    };

    struct CollectionPolicy {
        ScopeOverride requestedScope = ScopeOverride::Default;
        bool          onlyActiveWorkspace = false;
        bool          onlyActiveMonitor = true;
        bool          includeSpecial = false;
    };

    struct FullscreenWorkspaceBackup {
        PHLWORKSPACE    workspace;
        PHLWINDOW       originalFullscreenWindow;
        bool            hadFullscreenWindow = false;
        eFullscreenMode fullscreenMode = FSMODE_NONE;
        eFullscreenMode originalFullscreenMode = FSMODE_NONE;
    };

    struct ManagedWindow {
        PHLWINDOW    window;
        PHLMONITOR   targetMonitor;
        std::string  title;
        Rect         naturalGlobal;
        Rect         exitGlobal;
        Rect         relayoutFromGlobal;
        Rect         targetGlobal;
        WindowSlot   slot;
        float        previewAlpha = 1.0F;
        bool         isFloating = false;
        bool         isPinned = false;
        bool         isNiriFloatingOverlay = false;
    };

    struct WorkspaceStripEntry {
        struct Snapshot {
            SP<Render::IFramebuffer> framebuffer;
        };

        struct WindowPreview {
            PHLWINDOW window;
            Rect  naturalGlobal;
            float alpha = 1.0F;
            bool  focused = false;
        };

        PHLMONITOR               monitor;
        PHLWORKSPACE             workspace;
        WORKSPACEID              workspaceId = WORKSPACE_INVALID;
        std::string              workspaceName;
        Rect                     rect;
        Rect                     relayoutFromRect;
        bool                     hasRelayoutFromRect = false;
        std::shared_ptr<Snapshot> snapshot;
        std::vector<WindowPreview> windows;
        bool                     syntheticEmpty = false;
        bool                     newWorkspaceSlot = false;
        bool                     specialWorkspace = false;
        bool                     active = false;
    };

    struct EmptyWorkspacePlaceholder {
        PHLMONITOR  monitor;
        PHLWORKSPACE workspace;
        WORKSPACEID workspaceId = WORKSPACE_INVALID;
        Rect        naturalGlobal;
        Rect        exitGlobal;
        Rect        targetGlobal;
        Rect        relayoutFromGlobal;
        bool        backingOnly = false;
    };

    struct NiriWallpaperSnapshot {
        PHLMONITOR               monitor;
        PHLLS                    layer;
        SP<Render::IFramebuffer> framebuffer;
    };

    struct NiriDragTarget {
        PHLWORKSPACE                workspace;
        PHLMONITOR                  monitor;
        WORKSPACEID                 workspaceId = WORKSPACE_INVALID;
        overview_drag::InsertTarget insertion;
        bool                        floating = false;
    };

    struct NiriDragSession {
        bool                                  active = false;
        PHLWINDOWREF                          window;
        PHLWORKSPACEREF                       sourceWorkspace;
        Vector2D                              pointerRatio;
        Rect                                  previewRect;
        std::size_t                           sourceColumn = 0;
        std::size_t                           sourceTile = 0;
        float                                 sourceColumnWidth = 1.0F;
        bool                                  sourceFloating = false;
        bool                                  detached = false;
        std::optional<NiriDragTarget>         target;
        std::chrono::steady_clock::time_point edgeEnteredAt = {};
        std::chrono::steady_clock::time_point lastEdgeTick = {};
        double                                edgeVelocity = 0.0;
    };

    struct State {
        Phase                                  phase = Phase::Inactive;
        PHLMONITOR                             ownerMonitor;
        PHLWORKSPACE                           ownerWorkspace;
        CollectionPolicy                       collectionPolicy;
        bool                                   suppressWorkspaceStrip = false;
        std::vector<PHLMONITOR>                participatingMonitors;
        std::vector<PHLWORKSPACE>              managedWorkspaces;
        std::vector<WorkspaceStripEntry>       stripEntries;
        std::vector<EmptyWorkspacePlaceholder> emptyWorkspacePlaceholders;
        std::vector<FullscreenWorkspaceBackup> fullscreenBackups;
        PHLWINDOW                              focusBeforeOpen;
        PHLWINDOW                              focusDuringOverview;
        PHLWINDOW                              pendingExitFocus;
        PHLWORKSPACE                           pendingExitWorkspace;
        CloseMode                              closeMode = CloseMode::Normal;
        bool                                   fullscreenOverrideActive = false;
        std::vector<ManagedWindow>             windows;
        std::vector<ManagedWindow>             transientClosingWindows;
        std::vector<WindowSlot>                slots;
        std::optional<std::size_t>             hoveredStripIndex;
        std::optional<std::size_t>             hoveredIndex;
        std::optional<std::size_t>             selectedIndex;
        double                                 animationProgress = 0.0;
        double                                 animationFromVisual = 0.0;
        double                                 animationToVisual = 0.0;
        double                                 relayoutProgress = 1.0;
        bool                                   relayoutActive = false;
        std::size_t                            settleStableFrames = 0;
        bool                                   settleHasSample = false;
        bool                                   exitFullscreenReapplied = false;
        bool                                   deferredFullscreenWorkspaceClear = false;
        bool                                   deferredHiddenFullscreenReapply = false;
        std::chrono::steady_clock::time_point  animationStart = {};
        std::chrono::steady_clock::time_point  relayoutStart = {};
        std::chrono::steady_clock::time_point  settleStart = {};
    };

    using PreviewRectSnapshot = std::vector<std::pair<PHLWINDOW, Rect>>;

    enum class ScrollingSpotTargeting {
        Configured,
        Center,
    };

    enum class ScrollingSpotSyncIntent {
        PreserveNativeCamera,
        FocusChange,
    };

    struct SurfaceRenderDataSnapshot {
        Vector2D pos;
        Vector2D localPos;
        double   w = 0.0;
        double   h = 0.0;
        int      rounding = 0;
        bool     dontRound = true;
        float    roundingPower = 2.0F;
        float    alpha = 1.0F;
        float    fadeAlpha = 1.0F;
        bool     blur = false;
        bool     blockBlurOptimization = false;
        CBox     clipBox = {};
    };

    struct WindowTransform {
        Rect   actualGlobal;
        Rect   targetGlobal;
        double scaleX = 1.0;
        double scaleY = 1.0;
    };

    struct GestureRegistration {
        std::size_t               fingerCount = 0;
        eTrackpadGestureDirection direction = TRACKPAD_GESTURE_DIR_NONE;
        uint32_t                  modMask = 0;
        float                     deltaScale = 1.0F;
        bool                      disableInhibit = false;
    };

    struct GestureSession {
        bool         active = false;
        bool         recommand = false;
        bool         startedVisible = false;
        bool         opening = true;
        bool         allowRecommandTransfer = false;
        ScopeOverride requestedScope = ScopeOverride::Default;
        ScopeOverride initialScope = ScopeOverride::Default;
        ScopeOverride compactScope = ScopeOverride::Default;
        eTrackpadGestureDirection direction = TRACKPAD_GESTURE_DIR_VERTICAL;
        int          directionSign = 1;
        double       openness = 0.0;
        double       signedProgress = 0.0;
        double       hiddenGapProgress = 0.0;
        double       lastAlignedSpeed = 0.0;
        float        deltaScale = 1.0F;
    };

    enum class ScrollGestureRoute {
        None,
        Layout,
    };

    struct ScrollGestureSession {
        bool                      active = false;
        HymissionScrollMode       mode = HymissionScrollMode::Layout;
        ScrollGestureRoute        route = ScrollGestureRoute::None;
        eTrackpadGestureDirection direction = TRACKPAD_GESTURE_DIR_HORIZONTAL;
        float                     deltaScale = 1.0F;
        std::size_t               debugSamples = 0;
        bool                      skipNextUpdate = false;
        bool                      restoreScrollingFollowFocus = false;
    };

    struct WorkspaceNameBackup {
        PHLWORKSPACE workspace;
        std::string  name;
    };

    struct WorkspaceOverride {
        MONITORID    monitorId = MONITOR_INVALID;
        PHLWORKSPACE workspace;
        WORKSPACEID  workspaceId = WORKSPACE_INVALID;
        std::string  workspaceName;
        bool         syntheticEmpty = false;
    };

    enum class WorkspaceTransitionAxis {
        Horizontal,
        Vertical,
    };

    enum class WorkspaceTransitionMode {
        Gesture,
        TimedCommit,
        TimedRevert,
    };

    struct WorkspaceTransition {
        bool                                  active = false;
        PHLMONITOR                             monitor;
        eTrackpadGestureDirection              gestureDirection = TRACKPAD_GESTURE_DIR_NONE;
        WorkspaceTransitionAxis                axis = WorkspaceTransitionAxis::Horizontal;
        WorkspaceTransitionMode                mode = WorkspaceTransitionMode::Gesture;
        double                                 distance = 1.0;
        double                                 delta = 0.0;
        int                                    step = 0;
        int                                    initialDirection = 0;
        double                                 avgSpeed = 0.0;
        int                                    speedPoints = 0;
        WORKSPACEID                            targetWorkspaceId = WORKSPACE_INVALID;
        std::string                            targetWorkspaceName;
        bool                                   targetWorkspaceSyntheticEmpty = false;
        bool                                   targetActivatedEarly = false;
        bool                                   targetEdgeCameraPreserved = false;
        PHLWINDOWREF                           previewAlphaOverrideWindow;
        float                                  previewAlphaOverride = 1.0F;
        State                                  sourceState;
        State                                  targetState;
        double                                 animationFromDelta = 0.0;
        double                                 animationToDelta = 0.0;
        double                                 animationProgress = 0.0;
        std::chrono::steady_clock::time_point  animationStart = {};
    };

    struct WorkspaceTransitionRequest {
        std::string args;
        bool        currentMonitorOnly = false;
    };

    struct WorkspaceTransitionRenderStateBackup {
        PHLWORKSPACE workspace;
        bool         visible = false;
        bool         forceRendering = false;
        Vector2D     renderOffsetValue;
        Vector2D     renderOffsetGoal;
        float        alphaValue = 1.0F;
        float        alphaGoal = 1.0F;
    };

    struct WorkspaceSwipeGestureContext {
        bool                      active = false;
        PHLMONITOR                monitor;
        eTrackpadGestureDirection direction = TRACKPAD_GESTURE_DIR_NONE;
        double                    gestureDelta = 0.0;
        bool                      touchActive = false;
        int32_t                   touchId = 0;
        bool                      touchVertical = false;
        bool                      touchFromHighEdge = false;
    };

    struct StripPreviewContext {
        bool       active = false;
        PHLMONITOR monitor;
        State      state;
        Vector2D   framebufferSize;
    };

    struct ThemeWorkspaceActivationTarget {
        PHLMONITOR   monitor;
        PHLWORKSPACE workspace;
        WORKSPACEID  workspaceId = WORKSPACE_INVALID;
    };

    struct HiddenStripLayerProxy {
        PHLLS      layer;
        PHLMONITOR monitor;
        Rect       capturedRectGlobal;
        Rect       proxyRectGlobal;
        Vector2D   snapshotSize;
        SP<Render::IFramebuffer> framebuffer;
        std::array<SP<Render::IFramebuffer>, 4> blurredFramebuffers;
        bool       niriWallpaperLayoutLayer = false;
        WORKSPACEID niriWallpaperWorkspaceId = WORKSPACE_INVALID;
    };

    using SurfaceGetTexBoxFn = CBox (*)(void*);
    using SurfaceBoundingBoxFn = std::optional<CBox> (*)(void*);
    using SurfaceOpaqueRegionFn = CRegion (*)(void*);
    using SurfaceVisibleRegionFn = CRegion (*)(void*, bool&);
    using SurfaceDrawFn = std::vector<UP<IPassElement>> (*)(void*);
    using SurfaceBlurNeedsFn = bool (*)(void*);
    using ShouldRenderWindowFn = bool (*)(void*, PHLWINDOW, PHLMONITOR);
    using RenderLayerFn = void (*)(void*, PHLLS, PHLMONITOR, const Time::steady_tp&, bool, bool);
    using BorderDrawFn = void (*)(void*, PHLMONITOR, const float&);
    using CalculateUVForSurfaceFn = void (*)(void*, PHLWINDOW, SP<CWLSurfaceResource>, PHLMONITOR, bool, const Vector2D&, const Vector2D&, bool);
    using DispatcherHandler = std::function<SDispatchResult(std::string)>;
    using WorkspaceSwipeBeginFn = void (*)(void*, const ITrackpadGesture::STrackpadGestureBegin&);
    using WorkspaceSwipeUpdateFn = void (*)(void*, const ITrackpadGesture::STrackpadGestureUpdate&);
    using WorkspaceSwipeEndFn = void (*)(void*, const ITrackpadGesture::STrackpadGestureEnd&);
    using UnifiedWorkspaceSwipeBeginFn = void (*)(void*);
    using UnifiedWorkspaceSwipeUpdateFn = void (*)(void*, double);
    using UnifiedWorkspaceSwipeEndFn = void (*)(void*);
    using ScrollMoveGestureBeginFn = void (*)(void*, const ITrackpadGesture::STrackpadGestureBegin&);
    using ScrollMoveGestureUpdateFn = void (*)(void*, const ITrackpadGesture::STrackpadGestureUpdate&);
    using ScrollMoveGestureEndFn = void (*)(void*, const ITrackpadGesture::STrackpadGestureEnd&);
    using HandleGestureFn = std::optional<std::string> (*)(void*, const std::string&, const std::string&);
    [[nodiscard]] LayoutConfig loadLayoutConfig() const;
    [[nodiscard]] LayoutConfig layoutConfigForState(const State& state) const;
    [[nodiscard]] CollectionPolicy loadCollectionPolicy(ScopeOverride requestedScope) const;
    [[nodiscard]] std::optional<ScopeOverride> parseScopeOverride(const std::string& args, std::string& error) const;
    [[nodiscard]] bool         expandSelectedWindowEnabled() const;
    [[nodiscard]] bool         multiWorkspaceExpandSelectedWindowEnabled() const;
    [[nodiscard]] bool         focusFollowsMouseEnabled() const;
    [[nodiscard]] bool         refreshPreviewsOnConfigReloadEnabled() const;
    [[nodiscard]] int          stripThemeSurfaceFeedbackFrames() const;
    [[nodiscard]] bool         multiWorkspaceSortRecentFirstEnabled() const;
    [[nodiscard]] bool         toggleSwitchModeEnabled() const;
    [[nodiscard]] bool         switchToggleAutoNextEnabled() const;
    [[nodiscard]] std::string  switchReleaseKeyConfig() const;
    [[nodiscard]] bool         gestureInvertVerticalEnabled() const;
    [[nodiscard]] bool         workspaceSwipeInvertEnabled() const;
    [[nodiscard]] bool         workspaceChangeKeepsOverviewEnabled() const;
    [[nodiscard]] bool         damageTrackingOverrideEnabled() const;
    [[nodiscard]] bool         closeSpecialWorkspacesOnOpenEnabled() const;
    [[nodiscard]] std::chrono::milliseconds postCloseCrossScopeDebounce() const;
    [[nodiscard]] bool         hideBarsWhenStripShownEnabled() const;
    [[nodiscard]] std::string  hideBarNamespaces() const;
    [[nodiscard]] bool         hideOverviewLayersEnabled() const;
    [[nodiscard]] std::string  hideOverviewLayerNamespaces() const;
    [[nodiscard]] bool         hideBarAnimationEffectsEnabled() const;
    [[nodiscard]] bool         hideBarAnimationBlurEnabled() const;
    [[nodiscard]] double       hideBarAnimationMoveMultiplier() const;
    [[nodiscard]] double       hideBarAnimationScaleDivisor() const;
    [[nodiscard]] double       hideBarAnimationAlphaEnd() const;
    [[nodiscard]] bool         barSingleMissionControlEnabled() const;
    [[nodiscard]] bool         showFocusIndicatorEnabled() const;
    [[nodiscard]] double       activeBorderWidth() const;
    [[nodiscard]] double       inactiveBorderWidth() const;
    [[nodiscard]] double       focusedBorderThicknessReduction() const;
    [[nodiscard]] double       overviewBorderRoundingScale() const;
    [[nodiscard]] bool         niriModeEnabled() const;
    [[nodiscard]] bool         niriModeAppliesToState(const State& state) const;
    [[nodiscard]] double       niriScrollPixelsPerDelta() const;
    [[nodiscard]] double       niriLayoutScale() const;
    [[nodiscard]] double       niriOverviewScale() const;
    [[nodiscard]] double       niriWindowGaps() const;
    [[nodiscard]] double       niriWindowGapsForWorkspace(const PHLWORKSPACE& workspace, GestureAxis axis) const;
    [[nodiscard]] double       niriMultiWorkspaceScale() const;
    [[nodiscard]] double       niriWorkspaceGap() const;
    [[nodiscard]] double       niriWorkspaceScale() const;
    [[nodiscard]] bool         niriModeShowEmptyWorkspacesBetweenEnabled() const;
    [[nodiscard]] bool         niriModeWallpaperZoomEnabled() const;
    [[nodiscard]] CHyprColor   niriModeWallpaperZoomBackgroundColor() const;
    [[nodiscard]] std::string  niriModeWallpaperZoomLayerNamespaces() const;
    [[nodiscard]] std::chrono::milliseconds niriModeWallpaperZoomLayerRefreshInterval() const;
    [[nodiscard]] bool         niriWallpaperZoomAppliesToState(const State& state) const;
    [[nodiscard]] bool         niriWallpaperZoomAppliesToMonitor(const State& state, const PHLMONITOR& monitor) const;
    [[nodiscard]] bool         niriPreviewDisabled() const;
    [[nodiscard]] bool         niriOverviewAnimationsEnabled() const;
    [[nodiscard]] double       niriOverviewOpenCloseSpeedMultiplier() const;
    [[nodiscard]] bool         debugLogsEnabled() const;
    [[nodiscard]] bool         debugSurfaceLogsEnabled() const;
    [[nodiscard]] PHLWORKSPACE activeLayoutWorkspace() const;
    [[nodiscard]] bool         isScrollingWorkspace(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] bool         hasScrollingWorkspace() const;
    [[nodiscard]] GestureAxis  gestureAxisForDirection(eTrackpadGestureDirection direction) const;
    [[nodiscard]] ScrollingLayoutDirection scrollingLayoutDirection() const;
    [[nodiscard]] bool         canScrollActiveLayoutWithGesture(eTrackpadGestureDirection direction) const;
    [[nodiscard]] double       scrollLayoutPixelsPerGestureDelta(ScrollingLayoutDirection direction) const;
    [[nodiscard]] double       scrollLayoutPrimaryDelta(const IPointer::SSwipeUpdateEvent& event, eTrackpadGestureDirection direction, float deltaScale) const;
    [[nodiscard]] bool         scrollActiveLayoutByGestureDelta(const IPointer::SSwipeUpdateEvent& event, eTrackpadGestureDirection direction, float deltaScale);
    void                       refreshNiriScrollingOverviewAfterLayoutScroll(const char* source, const PreviewRectSnapshot* previousPreviewRects = nullptr);
    void                       refreshNiriScrollingOverviewAfterFocusDispatcher(const char* source, PHLWINDOW preferredWindow = {}, bool syncScrollingSpot = true);
    [[nodiscard]] bool         shouldSyncRealFocusDuringOverview() const;
    [[nodiscard]] bool         shouldSyncScrollingLayoutDuringOverviewFocus() const;
    [[nodiscard]] bool         handleNiriOverviewArrowKeybind(xkb_keysym_t keysym, uint32_t modifiers);
    [[nodiscard]] bool         allowsWorkspaceSwitchInOverview() const;
    [[nodiscard]] bool         shouldBlockWorkspaceSwitchInOverview() const;
    [[nodiscard]] bool         shouldOverrideWorkspaceNames(const State& state) const;
    [[nodiscard]] std::string  workspaceStripAnchor() const;
    [[nodiscard]] WorkspaceStripEmptyMode workspaceStripEmptyMode() const;
    [[nodiscard]] double       workspaceStripThickness(const PHLMONITOR& monitor) const;
    [[nodiscard]] double       workspaceStripGap() const;
    [[nodiscard]] int          workspaceStripLabelFontSize() const;
    [[nodiscard]] double       workspaceStripLabelOpacity() const;
    [[nodiscard]] bool         shouldDisableWorkspaceStripForNiriPreview(const State& state) const;
    [[nodiscard]] bool         shouldRenderEmptyOverviewPlaceholder(const State& state, const PHLMONITOR& monitor) const;
    [[nodiscard]] PHLWORKSPACE centeredEmptyPlaceholderWorkspace(const State& state, const PHLMONITOR& monitor) const;
    [[nodiscard]] bool         workspaceStripEnabled(const State& state) const;
    [[nodiscard]] bool         isStripOnlyOverviewState(const State& state) const;
    [[nodiscard]] bool         shouldContinuouslyRefreshWorkspaceStripSnapshots() const;
    [[nodiscard]] bool         isCurrentActiveWorkspaceStripEntry(const WorkspaceStripEntry& entry) const;
    [[nodiscard]] int          resolveOverviewWorkspaceSwipeStep(eTrackpadGestureDirection direction, double totalDelta, double lastDelta) const;
    [[nodiscard]] bool         switchOverviewWorkspaceByStep(int step);
    [[nodiscard]] double       gestureSwipeDistance() const;
    [[nodiscard]] double       gestureForceSpeedThreshold() const;
    [[nodiscard]] bool         gestureSwipeForeverEnabled() const;
    [[nodiscard]] bool         gestureSwipeCreateNewEnabled() const;
    [[nodiscard]] bool         gestureSwipeUseRelativeEnabled() const;
    [[nodiscard]] bool         gestureSwipeDirectionLockEnabled() const;
    [[nodiscard]] double       gestureSwipeDirectionLockThreshold() const;
    void                       setInputFollowMouseOverride(bool disable);
    void                       setScrollingFollowFocusOverride(bool disable);
    void                       setAnimationsEnabledOverride(bool disable, std::optional<std::chrono::milliseconds> restoreDelay = std::nullopt);
    void                       setDamageTrackingOverride(bool disable);
    void                       armThemeSurfaceFeedback(std::size_t frames);
    void                       pumpThemeSurfaceFeedbackFrames();
    bool                       renderThemeWorkspaceFeedbackFrame();
    void                       clearThemeSurfaceFeedbackTimer();
    [[nodiscard]] bool         stripThemeWorkspaceActivationRefreshEnabled() const;
    [[nodiscard]] std::string  stripThemeWorkspaceActivationRefreshClasses() const;
    [[nodiscard]] bool         workspaceNeedsThemeActivationRefresh(const PHLWORKSPACE& workspace) const;
    void                       armThemeWorkspaceActivationRefresh();
    void                       stepThemeWorkspaceActivationRefresh();
    void                       clearThemeWorkspaceActivationRefresh();
    void                       clearWorkspaceStripSnapshotRefreshTimer();
    void                       closeActiveSpecialWorkspaces();
    void                       applyWorkspaceNameOverrides(const State& state);
    void                       restoreWorkspaceNameOverrides();
    void                       clearRegisteredTrackpadGestures();
    void                       rememberRegisteredTrackpadGesture(const GestureRegistration& gesture);
    void                       replaceNativeWorkspaceGestures(const char* source = "?");
    [[nodiscard]] bool         installHooks();
    [[nodiscard]] bool         activateHooks();
    void                       deactivateHooks();
    [[nodiscard]] bool         hookFunction(const std::string& symbolName, const std::string& demangledNeedle, CFunctionHook*& hook, void* destination);
    [[nodiscard]] bool         hookFunction(const std::string& symbolName, const std::vector<std::string>& demangledNeedles, CFunctionHook*& hook, void* destination);
    [[nodiscard]] void*        findFunction(const std::string& symbolName, const std::string& demangledNeedle) const;
    [[nodiscard]] void*        findFunction(const std::string& symbolName, const std::vector<std::string>& demangledNeedles) const;
    [[nodiscard]] bool         wrapDispatcher(const std::string& name, DispatcherHandler& original, DispatcherHandler replacement);
    void                       prepareOverviewDispatcherTarget(PHLWINDOW preferredWindow = {}, bool allowWorkspaceOnly = false);
    [[nodiscard]] SDispatchResult runOverviewExecDispatcher(std::string args);
    [[nodiscard]] SDispatchResult runOverviewEditingDispatcher(const char* dispatcherName, DispatcherHandler* original, std::string args);
    [[nodiscard]] std::optional<SDispatchResult> tryRunDirectNiriMoveToWorkspaceDispatcher(const std::string& args, const PHLWINDOW& selectedBefore,
                                                                                          bool keepFocusOnSource);
    [[nodiscard]] SDispatchResult runDirectNiriSilentMoveToWorkspaceDispatcher(const std::string& args, const PHLWINDOW& movedWindow,
                                                                              const PHLWORKSPACE& sourceWorkspace, const PHLMONITOR& sourceMonitor,
                                                                              const PHLWORKSPACE& targetWorkspace, const PHLWINDOW& selectedBefore,
                                                                              bool explicitWindowArg, const DispatcherHandler& silentDispatcher);
    void                       processScheduledVisibleStateRebuild(bool transitionActiveWhenScheduled);
    void                       restoreWrappedDispatchers();

    [[nodiscard]] bool         isAnimating() const;
    [[nodiscard]] bool         isVisible() const;
    [[nodiscard]] bool         shouldHandleInput() const;
    [[nodiscard]] bool         insideRenderLifecycle() const;
    [[nodiscard]] std::vector<PHLMONITOR> ownedMonitors() const;
    [[nodiscard]] bool         ownsMonitor(const PHLMONITOR& monitor) const;
    [[nodiscard]] bool         ownsWorkspace(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] bool         hasManagedWindow(const PHLWINDOW& window) const;
    [[nodiscard]] bool         usesDirectNiriScrollingOverview(const State& state) const;
    [[nodiscard]] bool         activeDirectNiriSingleWorkspaceOverview() const;
    [[nodiscard]] bool         timedNiriSingleWorkspaceTransitionActive() const;
    [[nodiscard]] bool         windowHasUsableStateGeometry(const PHLWINDOW& window) const;
    [[nodiscard]] bool         windowMatchesOverviewScope(const PHLWINDOW& window, const State& state, bool requireUsableGeometry) const;
    [[nodiscard]] bool         shouldAutoCloseFor(const PHLWINDOW& window) const;
    [[nodiscard]] bool         shouldManageWindow(const PHLWINDOW& window, const State& state) const;
    [[nodiscard]] std::string  collectionSummary(const PHLMONITOR& monitor) const;
    [[nodiscard]] PreviewRectSnapshot captureCurrentPreviewRects() const;
    [[nodiscard]] PreviewRectSnapshot commitActiveNiriRelayoutForRetarget();
    [[nodiscard]] std::vector<Rect> targetRects() const;
    [[nodiscard]] Rect         workspaceStripBandRectForMonitor(const PHLMONITOR& monitor, const State& state) const;
    [[nodiscard]] Rect         overviewContentRectForMonitor(const PHLMONITOR& monitor, const State& state) const;
    [[nodiscard]] Vector2D     stripThumbnailPreviewOffset(const PHLMONITOR& monitor, const State& state) const;
    [[nodiscard]] std::vector<Rect> stripRects() const;
    [[nodiscard]] const ManagedWindow* managedWindowFor(const State& state, const PHLWINDOW& window, bool includeTransient = false) const;
    [[nodiscard]] const ManagedWindow* managedWindowForWorkspaceTransition(const PHLWINDOW& window) const;
    [[nodiscard]] const ManagedWindow* managedWindowFor(const PHLWINDOW& window) const;
    [[nodiscard]] const ManagedWindow* renderableManagedWindowFor(const PHLWINDOW& window, const PHLMONITOR& monitor) const;
    [[nodiscard]] PHLWINDOW     selectedWindow() const;
    [[nodiscard]] float        managedPreviewAlphaFor(const PHLWINDOW& window, float fallback = 1.0F) const;
    [[nodiscard]] PHLMONITOR   focusMonitorForWindow(const PHLWINDOW& window) const;
    [[nodiscard]] bool         shouldSuppressNiriFocusScrollForMonitorReturn(const PHLWINDOW& window, const PHLMONITOR& previousFocusMonitor) const;
    [[nodiscard]] PHLMONITOR   preferredMonitorForWindow(const PHLWINDOW& window, const State& state) const;
    [[nodiscard]] PHLMONITOR   previewMonitorForWindow(const PHLWINDOW& window) const;
    [[nodiscard]] const FullscreenWorkspaceBackup* fullscreenBackupForWorkspace(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] const FullscreenWorkspaceBackup* fullscreenBackupForWindow(const PHLWINDOW& window) const;
    [[nodiscard]] PHLWINDOW    hoveredWindow() const;
    [[nodiscard]] PHLWINDOW    directNiriFocusedOverviewWindow(const State& state) const;
    [[nodiscard]] bool         directNiriOverviewHasSingleColumnAnchor(const PHLWINDOW& anchor) const;
    [[nodiscard]] PHLWINDOW    directNiriOneToTwoColumnOpenAnchor(const PHLWINDOW& openedWindow) const;
    void                       stabilizeDirectNiriOneToTwoColumnOpen(const PHLWINDOW& anchor);
    [[nodiscard]] PHLWINDOW    preferredOverviewExitFocus() const;
    [[nodiscard]] bool         directNiriEdgeCameraActive() const;
    [[nodiscard]] bool         directNiriOwnerEdgeCameraActive(const State& state) const;
    [[nodiscard]] const EmptyWorkspacePlaceholder* directNiriEdgeCameraOpenPlaceholder(const State& state) const;
    [[nodiscard]] bool         shouldPreserveDirectNiriEdgeCamera(const PHLWINDOW& window) const;
    [[nodiscard]] PHLWORKSPACE directNiriTwoColumnExitWorkspace() const;
    void                       freezeDirectNiriTwoColumnExitPreviewTargets();
    void                       stabilizeDirectNiriExitSnapshot(const PHLWINDOW& target);
    void                       enforceDirectNiriExitFocusGuard();
    void                       reconcileNiriCenteredSelectionForExit();
    [[nodiscard]] Rect         liveGlobalRectForWindow(const PHLWINDOW& window) const;
    [[nodiscard]] Rect         goalGlobalRectForWindow(const PHLWINDOW& window) const;
    [[nodiscard]] std::optional<Rect> livePreviewRectForManagedWindow(const ManagedWindow& window) const;
    [[nodiscard]] bool         shouldUseGoalGeometryForStateSnapshot(const PHLWINDOW& window) const;
    [[nodiscard]] bool         inactiveDirectNiriFloatingOverlay(const ManagedWindow& managed) const;
    [[nodiscard]] EmptyWorkspacePlaceholder* directNiriWorkspaceViewportPlaceholder(WORKSPACEID workspaceId, const PHLMONITOR& monitor);
    void                       stabilizeInactiveNiriFloatingOpenGeometry(WORKSPACEID selectedWorkspaceId, const Rect& selectedStart, const Rect& selectedTarget);
    void                       refreshWorkspaceLayoutSnapshot(const PHLWORKSPACE& workspace) const;
    void                       commitNonScrollingWorkspaceLayout(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] std::optional<Vector2D> predictedScrollingExitTranslation(const PHLWINDOW& window) const;
    [[nodiscard]] bool         applyNiriScrollingCameraExitGeometry(const PHLWINDOW& window);
    [[nodiscard]] bool         applyNiriScrollingCameraExitGeometry(const EmptyWorkspacePlaceholder& placeholder);
    [[nodiscard]] bool         applyNiriScrollingCameraOpenGeometry(const PHLWINDOW& window);
    [[nodiscard]] bool         applyNiriScrollingCameraOpenGeometry(const EmptyWorkspacePlaceholder& placeholder);
    void                       prepareGestureCloseExitGeometry();
    [[nodiscard]] bool         workspaceSwipeUsesVerticalAxis(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] double       workspaceSwipeViewportDistance(const PHLMONITOR& monitor, WorkspaceTransitionAxis axis) const;
    [[nodiscard]] std::optional<Rect> workspaceTransitionRectForWindow(const PHLWINDOW& window) const;
    [[nodiscard]] bool         resolveOverviewWorkspaceTargetByStep(const PHLMONITOR& monitor, int step, WORKSPACEID& workspaceId, std::string& workspaceName,
                                                                    PHLWORKSPACE& workspace, bool& syntheticEmpty) const;
    [[nodiscard]] bool         beginOverviewWorkspaceTransition(const PHLMONITOR& monitor, WORKSPACEID workspaceId, std::string workspaceName, PHLWORKSPACE workspace,
                                                                bool syntheticEmpty, WorkspaceTransitionMode mode,
                                                                std::optional<State> sourceStateOverride = std::nullopt,
                                                                PHLWINDOW preferredTargetFocus = {});
    [[nodiscard]] bool         rebuildTimedNiriWorkspaceTransitionTarget(const PHLWINDOW& preferredTargetFocus);
    [[nodiscard]] State        captureOverviewWorkspaceTransitionSourceState() const;
    [[nodiscard]] bool         beginExternalOverviewWorkspaceTransition(const PHLWORKSPACE& workspace);
    [[nodiscard]] bool         startOverviewWorkspaceTransitionByStep(const PHLMONITOR& monitor, int step, WorkspaceTransitionMode mode);
    void                       updateOverviewWorkspaceTransition();
    void                       requestOverviewWorkspaceTransitionCommit(bool followGesture = false, bool forceSync = false);
    void                       commitOverviewWorkspaceTransition(bool followGesture = false, bool forceSync = false);
    [[nodiscard]] bool         activateTimedNiriWorkspaceTransitionTarget();
    void                       clearOverviewWorkspaceTransition(const PHLWORKSPACE& committedWorkspace = {}, bool clearPendingRequests = true);
    void                       processQueuedEditDispatchers();
    void                       commitActiveNiriWorkspaceTransitionForRetarget();
    void                       startNextQueuedOverviewWorkspaceTransition();
    void                       armWorkspaceTransitionRenderState();
    void                       restoreWorkspaceTransitionRenderState(const PHLWORKSPACE& committedWorkspace = {});
    void                       armOverviewRenderState(const State& state);
    void                       restoreOverviewRenderState();
    [[nodiscard]] SDispatchResult startOverviewWorkspaceTransitionForDispatcher(const std::string& args, bool currentMonitorOnly);
    [[nodiscard]] std::optional<WindowTransform> windowTransformFor(const PHLWINDOW& window, const PHLMONITOR& monitor) const;
    [[nodiscard]] bool                          transformSurfaceRenderDataForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor,
                                                                                   CSurfacePassElement::SRenderData& renderData) const;
    bool                                        adjustTransformedSurfaceBoxSize(const CSurfacePassElement::SRenderData& renderData, const PHLMONITOR& monitor,
                                                                               CBox& box) const;
    [[nodiscard]] double                        hiddenStripLayerProgress(const PHLLS& layer, const PHLMONITOR& monitor) const;
    void                                        clearHiddenStripLayerProxies();
    void                                        syncHiddenStripLayerProxies();
    void                                        syncNiriWallpaperLayoutLayerProxies();
    void                                        startNiriWallpaperLayoutLayerRefresh();
    void                                        clearNiriWallpaperLayoutLayerRefresh();
    void                                        clearNiriWallpaperSnapshots();
    void                                        resetDirectNiriWorkspaceLanes();
    void                                        syncNiriWallpaperSnapshots();
    [[nodiscard]] SP<Render::IFramebuffer>       captureLayerFramebuffer(const PHLLS& layer);
    [[nodiscard]] bool                          isNiriWallpaperLayer(const PHLLS& layer, const PHLMONITOR& monitor) const;
    [[nodiscard]] bool                          isNiriWallpaperLayoutLayer(const PHLLS& layer, const PHLMONITOR& monitor) const;
    [[nodiscard]] bool                          isRetainedNiriWallpaperLayoutLayer(const PHLLS& layer, const PHLMONITOR& monitor) const;
    [[nodiscard]] SP<Render::ITexture>           niriWallpaperTextureForMonitor(const PHLMONITOR& monitor) const;
    [[nodiscard]] bool                          captureHiddenStripLayerProxy(const PHLLS& layer, const PHLMONITOR& monitor);
    [[nodiscard]] HiddenStripLayerProxy*        hiddenStripLayerProxyFor(const PHLLS& layer, const PHLMONITOR& monitor);
    [[nodiscard]] const HiddenStripLayerProxy*  hiddenStripLayerProxyFor(const PHLLS& layer, const PHLMONITOR& monitor) const;
    [[nodiscard]] Rect                          hiddenStripLayerProxyRect(const HiddenStripLayerProxy& proxy) const;
    [[nodiscard]] bool                          shouldRenderHiddenStripLayerProxy(const PHLLS& layer, const PHLMONITOR& monitor) const;
    void                                        renderHiddenStripLayerProxies() const;
    [[nodiscard]] const ManagedWindow*          focusedManagedForBorder(const State& state, const PHLMONITOR& renderMonitor) const;
    [[nodiscard]] bool                          borderUsesTransformedGeometry(const State& state) const;
    [[nodiscard]] Rect                          managedWindowBorderRect(const ManagedWindow& managed, const PHLMONITOR& renderMonitor, const State& state,
                                                                        bool useTargetGeometry, bool forceTransformedGeometry = false) const;
    [[nodiscard]] int                           managedWindowBorderRound(const ManagedWindow& managed, const PHLMONITOR& renderMonitor) const;
    [[nodiscard]] float                         managedWindowBorderRoundingPower(const ManagedWindow& managed) const;
    void                                        renderInactiveWindowBorders(const State& state, double progress, bool useTargetGeometry) const;
    void                                        renderFocusedWindowBorder(const State& state, double progress, bool useTargetGeometry) const;
    [[nodiscard]] bool                          suppressSurfaceBlur(void* surfacePassThisptr) const;
    [[nodiscard]] bool                          prepareSurfaceRenderData(void* surfacePassThisptr, const char* context, CSurfacePassElement::SRenderData*& renderData,
                                                                         PHLMONITOR& monitor, SurfaceRenderDataSnapshot& snapshot) const;
    void                                        restoreSurfaceRenderData(CSurfacePassElement::SRenderData* renderData, const SurfaceRenderDataSnapshot& snapshot) const;
    [[nodiscard]] std::size_t                   renderDirectNiriSurfaceTreeOverlay(const ManagedWindow& managed, const PHLMONITOR& monitor,
                                                                                  const Time::steady_tp& now, float alpha, const char* context,
                                                                                  std::optional<Rect> targetOverride = std::nullopt) const;
    [[nodiscard]] bool                          removeOccupiedWorkspacePlaceholder(State& state, const PHLWINDOW& window) const;
    [[nodiscard]] std::optional<std::size_t> hitTestStripTarget(double x, double y) const;
    [[nodiscard]] std::optional<std::size_t> hitTestTarget(double x, double y) const;
    [[nodiscard]] Rect         stablePreviewOrderRect(const ManagedWindow& window) const;
    [[nodiscard]] Rect         currentPreviewRect(const ManagedWindow& window) const;
    [[nodiscard]] bool         directNiriNativeFloatingGeometryPreviewActive(const ManagedWindow& window) const;
    [[nodiscard]] double       visualProgress() const;
    [[nodiscard]] double       relayoutVisualProgress() const;
    void                       beginOverviewRelayoutAnimation(const char* source = "?");
    void                       finishOverviewRelayoutAnimation();
    void                       beginOverviewVisibilityAnimation(const char* source = "?");
    void                       finishOverviewVisibilityAnimation();
    void                       beginWorkspaceTransitionAnimation(const char* source = "?");
    void                       finishWorkspaceTransitionAnimation();
    [[nodiscard]] double       workspaceStripEnterProgress() const;
    [[nodiscard]] Vector2D     workspaceStripEnterOffset(const PHLMONITOR& monitor) const;
    [[nodiscard]] Rect         currentWorkspaceStripRect(const WorkspaceStripEntry& entry) const;
    [[nodiscard]] Rect         animatedWorkspaceStripRect(const Rect& rect, const PHLMONITOR& monitor) const;
    [[nodiscard]] bool         transformBoxForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor, CBox& box, bool scaled) const;
    [[nodiscard]] CRegion      transformRegionForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor, const CRegion& region, bool scaled) const;
    [[nodiscard]] PHLWINDOW    resolveExitFocus(CloseMode mode) const;
    [[nodiscard]] PHLWORKSPACE resolveExitWorkspace(CloseMode mode) const;
    [[nodiscard]] const EmptyWorkspacePlaceholder* centeredEmptyWorkspacePlaceholder(const State& state) const;
    [[nodiscard]] EmptyWorkspacePlaceholder*       pendingExitWorkspacePlaceholder();
    [[nodiscard]] bool         exitFocusChangedWorkspace(const PHLWINDOW& window) const;
    [[nodiscard]] bool         shouldPreferGoalExitGeometry(const PHLWINDOW& window) const;
    [[nodiscard]] std::optional<Vector2D> visiblePointForWindowOnMonitor(const PHLWINDOW& window, const PHLMONITOR& monitor, bool preferGoal = false) const;
    [[nodiscard]] bool         clearWorkspaceFullscreenForExitTarget(const PHLWINDOW& window);
    [[nodiscard]] bool         shouldClearWorkspaceFullscreenForExitTarget(const PHLWINDOW& window) const;
    [[nodiscard]] bool         activateWindowWorkspaceForFocus(const PHLWINDOW& window) const;
    [[nodiscard]] bool         activateWorkspaceForExit(const PHLWORKSPACE& workspace) const;
    void                       normalizeDirectNiriWorkspaceActivation(const PHLWORKSPACE& workspace) const;
    void                       commitOverviewExitFocus(const PHLWINDOW& window);
    [[nodiscard]] PHLWINDOW    focusCandidateForWorkspace(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] bool         syncScrollingWorkspaceSpotOnWindow(
        const PHLWINDOW& window, ScrollingSpotTargeting targeting = ScrollingSpotTargeting::Configured,
        ScrollingSpotSyncIntent intent = ScrollingSpotSyncIntent::PreserveNativeCamera) const;
    [[nodiscard]] PHLWINDOW directNiriFloatActionTarget(const std::optional<PHLWINDOW>& window) const;
    [[nodiscard]] PHLWINDOW closestTiledDirectNiriGeometryAnchor(const PHLWINDOW& window) const;
    void                    prepareDirectNiriFloatActionTarget(const PHLWINDOW& window, bool syncScrollingSpot = true);
    void                    restoreDirectNiriFloatingActionTarget(const PHLWINDOW& window, const char* source);
    void                    armDirectNiriNativeFloatingGeometryPreview(const PHLWINDOW& window);
    void                    clearDirectNiriNativeFloatingGeometryPreview(const PHLWINDOW& window = {});
    [[nodiscard]] bool      hardRecalculateDirectNiriGeometryAnchor(const PHLWINDOW& anchor, const char* source);
    [[nodiscard]] bool      focusDirectNiriGeometryAnchor(const PHLWINDOW& anchor, const char* source);
    void                    refreshDirectNiriFloatingGeometryActionTarget(const PHLWINDOW& window, const char* source,
                                                                         const PreviewRectSnapshot* previousPreviewRects = nullptr);
    void                    refreshDirectNiriSetFloatingActionTarget(const PHLWINDOW& window, const char* source,
                                                                     const PreviewRectSnapshot* previousPreviewRects = nullptr);
    void                    refreshDirectNiriFloatActionTarget(const PHLWINDOW& window, bool tiledNow, const char* source,
                                                               const PreviewRectSnapshot* previousPreviewRects = nullptr);
    void                       refreshExitLayoutForFocus(const PHLWINDOW& window) const;
    void                       syncRealFocusDuringOverview(const PHLWINDOW& window, bool syncScrollingSpot = true,
                                                          const PreviewRectSnapshot* previousPreviewRects = nullptr, bool forceRealFocus = false);
    void                       syncFocusDuringOverviewFromSelection(bool syncScrollingSpot = true, const char* source = "?", bool centerCursor = false);
    [[nodiscard]] bool         refocusDirectNiriSelectionWithoutScroll(const char* source = "?");
    void                       armPendingSwapColumnRelayoutCommit(const PHLWORKSPACE& workspace);
    [[nodiscard]] bool         hasPendingSwapColumnRelayoutCommit(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] bool         commitPendingSwapColumnRelayout(const char* source = "?");
    void                       freezeSwapColumnBackendPreview(const PHLWORKSPACE& workspace, const char* source = "?");
    [[nodiscard]] bool         swapColumnBackendPreviewFreezeActiveFor(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] bool         swapColumnBackendPreviewFrozenFor(const ManagedWindow& window) const;
    [[nodiscard]] bool         pendingSwapColumnRelayoutOwnsPreviewFor(const ManagedWindow& window) const;
    [[nodiscard]] const ManagedWindow* frozenSwapColumnBackendPreviewManagedFor(const PHLWINDOW& window) const;
    [[nodiscard]] bool         shouldCarryFrozenSwapColumnBackendPreview(const ManagedWindow& managed, const PHLWORKSPACE& workspace) const;
    [[nodiscard]] bool         shouldSuppressSwapColumnFollowupFocusScroll(const PHLWINDOW& window) const;
    bool                       carryFrozenSwapColumnBackendPreviewLayout(ManagedWindow& managed, std::size_t index, const PHLWORKSPACE& workspace) const;
    bool                       carryFrozenSwapColumnBackendPreviewLayout(State& state, const PHLWORKSPACE& workspace, const char* source) const;
    void                       recordWindowActivation(const PHLWINDOW& window, bool allowWhileVisible = false);
    void                       pruneWindowActivationHistory(const PHLWINDOW& removedWindow = {});
    [[nodiscard]] bool         shouldUseRecentWindowOrdering(const State& state) const;
    void                       queueSelectionRetargetDuringOverview(const PHLWINDOW& window, bool syncScrollingSpot = true, const char* source = "?", bool centerCursor = false);
    void                       flushQueuedSelectionRetargetDuringOverview();
    void                       queueRealFocusDuringOverview(const PHLWINDOW& window, bool syncScrollingSpot = true, const char* source = "?", bool centerCursor = false);
    void                       flushQueuedRealFocusDuringOverview();
    void                       centerCursorOnOverviewWindow(const PHLWINDOW& window, const char* source = "?");
    void                       updateSelectedWindowLayout(const PHLWINDOW& previousSelectedWindow);
    void                       clearPendingWindowGeometryRetry();
    void                       scheduleVisibleStateRebuild();
    void                       scheduleDeferredNiriLayoutRefresh(const char* source = "?", std::size_t passes = 1);
    void                       scheduleWorkspaceChangeHandling(const PHLWORKSPACE& workspace, OverviewWorkspaceChangeAction action, bool allowExternalTransition = false);
    void                       updateOverviewWorkspaceSwipeGestureAdjusted(double delta, bool absolute);
    void                       schedulePendingWindowGeometryRetry(const PHLWINDOW& window);
    void                       updatePendingWindowGeometryRetry(const PHLWINDOW& window);
    [[nodiscard]] bool         matchesPendingLiveFocusWorkspaceChange(const PHLWORKSPACE& workspace) const;
    void                       clearPostCloseForcedFocus();
    void                       clearPostCloseDispatcher();
    void                       queuePostCloseDispatcher(PostCloseDispatcher dispatcher, std::string args);
    [[nodiscard]] SDispatchResult runHookedDispatcher(PostCloseDispatcher dispatcher, std::string args);
    void                       setFullscreenRenderOverride(bool suppress);

    void beginOpen(const PHLMONITOR& monitor, ScopeOverride requestedScope);
    void beginClose(CloseMode mode = CloseMode::Normal, std::optional<double> fromVisualOverride = std::nullopt, bool deferFullscreenMutations = false);
    [[nodiscard]] bool retargetGestureScope(ScopeOverride requestedScope);
    void deactivate();
    void scheduleDeactivate();
    void damageOwnedMonitors() const;
    void updateAnimation();
    void updateHoveredFromPointer(bool syncSelection = true, bool syncRealFocus = true, bool syncScrollingSpot = true, bool allowSelectionRetarget = false,
                                  const char* source = "?");
    void refreshVisibleStateMetadata(PHLWINDOW preferredSelectedWindow = {}, const PreviewRectSnapshot* relayoutOrigins = nullptr,
                                     const char* relayoutSource = "metadata-retarget");
    void rebuildVisibleState(PHLWINDOW preferredSelectedWindow = {}, bool forceRelayout = false);
    void moveSelection(Direction direction);
    [[nodiscard]] bool moveSelectionCircular(int step = 1, const char* source = "?");
    void activateSelection();
    void notify(const std::string& message, const CHyprColor& color, float durationMs) const;
    void debugLog(const std::string& message) const;
    void debugSurfaceLog(const std::string& message) const;
    [[nodiscard]] std::string debugWorkspaceLabel(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] std::string debugWindowLabel(const PHLWINDOW& window) const;
    void logOverviewLayoutState(const char* context, const State& state) const;
    void logScrollingWorkspaceSpotState(const char* context, const PHLWORKSPACE& workspace, const PHLWINDOW& focusWindow = {}) const;
    void logSwapColumnFollowupState(const char* context, const PHLWORKSPACE& workspace, const char* source, const PHLWINDOW& focusWindow = {}) const;
    [[nodiscard]] SP<IKeyboard> inputKeyboardWithState() const;
    [[nodiscard]] bool          switchReleaseKeyHeld() const;
    [[nodiscard]] bool          isSwitchReleaseEvent(const IKeyboard::SKeyEvent& event, const SP<IKeyboard>& keyboard) const;
    void                        updateToggleSwitchSessionReleaseTracking(const char* source = "?");
    void                        scheduleToggleSwitchReleasePoll();
    void                        clearToggleSwitchReleasePollTimer();
    void                        clearToggleSwitchSession();
    void                        schedulePostCloseCursorShapeReset();
    void                        clearPostCloseCursorShapeResetTimer();
    void                        scheduleDeferredOpen(ScopeOverride requestedScope);
    void                        clearPendingDeferredOpen();
    void                        armPostCloseOpenDebounce(ScopeOverride closedScope);
    [[nodiscard]] bool          shouldSuppressPostCloseOpen(ScopeOverride requestedScope) const;
    void latchHoverSelectionAnchor(const Vector2D& pointer);
    [[nodiscard]] bool hoverSelectionRetargetLocked(const Vector2D& pointer, const std::optional<std::size_t>& hoveredIndex) const;
    [[nodiscard]] bool workspaceStripEntriesMatchForSnapshot(const WorkspaceStripEntry& lhs, const WorkspaceStripEntry& rhs) const;
    void carryOverWorkspaceStripSnapshots(State& next, const State& previous) const;
    [[nodiscard]] bool carryOverWorkspaceStripRelayout(State& next, const State& previous) const;
    void renderWorkspaceStrip() const;
    [[nodiscard]] Rect workspaceStripThumbRect(const WorkspaceStripEntry& entry, const PHLMONITOR& monitor) const;
    void refreshWorkspaceStripSnapshots();
    void scheduleWorkspaceStripSnapshotRefresh();
    void renderWorkspaceStripSnapshot(WorkspaceStripEntry& entry);
    [[nodiscard]] bool shouldHideLayerSurface(const PHLLS& layer, const PHLMONITOR& monitor) const;
    [[nodiscard]] bool shouldHideLayerSurfaceNamespace(const PHLLS& layer, const std::string& namespaces) const;
    void renderBackdrop() const;
    [[nodiscard]] Rect emptyOverviewPlaceholderLocalRect(const PHLMONITOR& monitor, const PHLWORKSPACE& workspace, const Rect& content, const State& state) const;
    [[nodiscard]] Rect currentEmptyWorkspacePlaceholderRect(const EmptyWorkspacePlaceholder& placeholder) const;
    [[nodiscard]] PHLWORKSPACE niriWorkspaceForBackground(const State& state, const EmptyWorkspacePlaceholder& background) const;
    [[nodiscard]] Rect niriWorkspaceSurfaceRect(const State& state, const EmptyWorkspacePlaceholder& background, const Rect& viewportRect,
                                                const Rect& surfaceRect) const;
    [[nodiscard]] Rect niriWorkspaceBackgroundRect(const State& state, const EmptyWorkspacePlaceholder& background, const Rect& viewportRect) const;
    void renderNiriWorkspaceBackgrounds() const;
    void renderEmptyOverviewPlaceholder(bool backingOnlyPass = false) const;
    void renderSelectionChrome() const;
    void renderNiriDragHint() const;
    void renderOutline(const Rect& rect, const CHyprColor& color, double thickness) const;
    void renderOutline(const Rect& rect, const Config::CGradientValueData& gradient, double thickness, double alpha = 1.0, int round = 0,
                       float roundingPower = 2.0F) const;
    void activateStripTarget(std::size_t index);
    void clearStripWindowDragState();
    [[nodiscard]] bool canDragWindowInDirectNiriOverview(const PHLWINDOW& window) const;
    [[nodiscard]] double nativeWindowDragThreshold() const;
    void beginDirectNiriWindowDrag(std::size_t windowIndex, const Vector2D& pointer);
    void updateDirectNiriWindowDrag(const Vector2D& pointer);
    [[nodiscard]] bool finishDirectNiriWindowDrag();
    void cancelDirectNiriWindowDrag();
    void tickDirectNiriWindowDragEdgeScroll();
    [[nodiscard]] Rect directNiriDraggedPreviewRect() const;
    [[nodiscard]] float directNiriDraggedPreviewAlpha(const PHLWINDOW& window, float fallback) const;
    [[nodiscard]] std::optional<NiriDragTarget> directNiriDragTargetAt(const Vector2D& pointer) const;
    [[nodiscard]] bool applyDirectNiriDragTarget(const PHLWINDOW& window, const NiriDragTarget& target,
                                                  const PreviewRectSnapshot& previousPreviewRects, const Rect& releasePreviewRect);
    void applyWorkspaceStripCursorShape() const;
    bool refreshWorkspaceStripActivity(State& state, const PHLMONITOR& overrideMonitor = {}, WORKSPACEID overrideWorkspaceId = WORKSPACE_INVALID) const;
    void resetStaleClientCursorShape() const;
    void refreshPostCloseCursorShape() const;
    void clearPendingStripWorkspaceChange();
    [[nodiscard]] bool matchesPendingStripWorkspaceChange(const PHLWORKSPACE& workspace) const;
    void buildWorkspaceStripEntries(State& state) const;
    bool selectWindowInState(State& state, const PHLWINDOW& window) const;
    State  buildState(const PHLMONITOR& monitor, ScopeOverride requestedScope, const std::vector<WorkspaceOverride>& workspaceOverrides = {},
                      bool keepEmptyParticipatingMonitors = false, bool suppressWorkspaceStrip = false, PHLWINDOW preferredSelectedWindow = {},
                      bool refreshLayoutSnapshots = true) const;
    State  m_state;
    HANDLE m_handle = nullptr;

    CFunctionHook*            m_surfaceTexBoxHook = nullptr;
    CFunctionHook*            m_surfaceBoundingBoxHook = nullptr;
    CFunctionHook*            m_surfaceOpaqueRegionHook = nullptr;
    CFunctionHook*            m_surfaceVisibleRegionHook = nullptr;
    CFunctionHook*            m_surfaceDrawHook = nullptr;
    CFunctionHook*            m_surfaceNeedsLiveBlurHook = nullptr;
    CFunctionHook*            m_surfaceNeedsPrecomputeBlurHook = nullptr;
    CFunctionHook*            m_shouldRenderWindowHook = nullptr;
    CFunctionHook*            m_renderLayerHook = nullptr;
    CFunctionHook*            m_borderDrawHook = nullptr;
    CFunctionHook*            m_shadowDrawHook = nullptr;
    CFunctionHook*            m_calculateUVForSurfaceHook = nullptr;
    CFunctionHook*            m_workspaceSwipeBeginFunctionHook = nullptr;
    CFunctionHook*            m_workspaceSwipeUpdateFunctionHook = nullptr;
    CFunctionHook*            m_workspaceSwipeEndFunctionHook = nullptr;
    CFunctionHook*            m_unifiedWorkspaceSwipeBeginFunctionHook = nullptr;
    CFunctionHook*            m_unifiedWorkspaceSwipeUpdateFunctionHook = nullptr;
    CFunctionHook*            m_unifiedWorkspaceSwipeEndFunctionHook = nullptr;
    CFunctionHook*            m_scrollMoveGestureBeginFunctionHook = nullptr;
    CFunctionHook*            m_scrollMoveGestureUpdateFunctionHook = nullptr;
    CFunctionHook*            m_scrollMoveGestureEndFunctionHook = nullptr;
    CFunctionHook*            m_moveToWorkspaceActionFunctionHook = nullptr;
    CFunctionHook*            m_handleGestureHook = nullptr;
    SurfaceGetTexBoxFn        m_surfaceTexBoxOriginal = nullptr;
    SurfaceBoundingBoxFn      m_surfaceBoundingBoxOriginal = nullptr;
    SurfaceOpaqueRegionFn     m_surfaceOpaqueRegionOriginal = nullptr;
    SurfaceVisibleRegionFn    m_surfaceVisibleRegionOriginal = nullptr;
    SurfaceDrawFn             m_surfaceDrawOriginal = nullptr;
    SurfaceBlurNeedsFn        m_surfaceNeedsLiveBlurOriginal = nullptr;
    SurfaceBlurNeedsFn        m_surfaceNeedsPrecomputeBlurOriginal = nullptr;
    ShouldRenderWindowFn      m_shouldRenderWindowOriginal = nullptr;
    RenderLayerFn             m_renderLayerOriginal = nullptr;
    BorderDrawFn              m_borderDrawOriginal = nullptr;
    BorderDrawFn              m_shadowDrawOriginal = nullptr;
    CalculateUVForSurfaceFn   m_calculateUVForSurfaceOriginal = nullptr;
    DispatcherHandler         m_fullscreenActiveOriginal;
    DispatcherHandler         m_fullscreenStateActiveOriginal;
    DispatcherHandler         m_changeWorkspaceOriginal;
    DispatcherHandler         m_focusWorkspaceOnCurrentMonitorOriginal;
    DispatcherHandler         m_layoutMessageOriginal;
    std::string               m_layoutMessageDispatcherName = "layoutmsg";
    DispatcherHandler         m_moveFocusOriginal;
    DispatcherHandler         m_execOriginal;
    std::unordered_map<std::string, DispatcherHandler> m_overviewEditingDispatchersOriginal;
    bool                      m_fullscreenActiveDispatcherWrapped = false;
    bool                      m_fullscreenStateDispatcherWrapped = false;
    bool                      m_changeWorkspaceDispatcherWrapped = false;
    bool                      m_focusWorkspaceOnCurrentMonitorDispatcherWrapped = false;
    bool                      m_layoutMessageDispatcherWrapped = false;
    bool                      m_moveFocusDispatcherWrapped = false;
    bool                      m_execDispatcherWrapped = false;
    bool                      m_overviewEditingDispatcherInProgress = false;
    WorkspaceSwipeBeginFn     m_workspaceSwipeBeginOriginal = nullptr;
    WorkspaceSwipeUpdateFn    m_workspaceSwipeUpdateOriginal = nullptr;
    WorkspaceSwipeEndFn       m_workspaceSwipeEndOriginal = nullptr;
    UnifiedWorkspaceSwipeBeginFn  m_unifiedWorkspaceSwipeBeginOriginal = nullptr;
    UnifiedWorkspaceSwipeUpdateFn m_unifiedWorkspaceSwipeUpdateOriginal = nullptr;
    UnifiedWorkspaceSwipeEndFn    m_unifiedWorkspaceSwipeEndOriginal = nullptr;
    ScrollMoveGestureBeginFn  m_scrollMoveGestureBeginOriginal = nullptr;
    ScrollMoveGestureUpdateFn m_scrollMoveGestureUpdateOriginal = nullptr;
    ScrollMoveGestureEndFn    m_scrollMoveGestureEndOriginal = nullptr;
    using MoveToWorkspaceActionFn = Config::Actions::ActionResult (*)(PHLWORKSPACE, bool, std::optional<PHLWINDOW>);
    MoveToWorkspaceActionFn   m_moveToWorkspaceActionOriginal = nullptr;
    HandleGestureFn           m_handleGestureOriginal = nullptr;
    bool                      m_hooksActive = false;
    bool                      m_inputFollowMouseOverridden = false;
    long                      m_inputFollowMouseBackup = 1;
    bool                      m_restoreInputFollowMouseAfterPostClose = false;
    bool                      m_scrollingFollowFocusOverridden = false;
    long                      m_scrollingFollowFocusBackup = 1;
    bool                      m_restoreScrollingFollowFocusAfterScrollMouseMove = false;
    PHLMONITOR                m_lastActiveWindowMonitor;
    bool                      m_animationsEnabledOverridden = false;
    long                      m_animationsEnabledBackup = 1;
    PHLANIMVAR<float>         m_relayoutProgressAnimation;
    PHLANIMVAR<float>         m_overviewVisibilityAnimation;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> m_overviewVisibilityAnimationConfig;
    PHLANIMVAR<float>         m_workspaceTransitionAnimation;
    PHLWINDOWREF              m_directNiriNativeFloatingGeometryPreview;
    SP<CEventLoopTimer>       m_animationsEnabledRestoreTimer;
    SP<CEventLoopTimer>       m_themeSurfaceFeedbackTimer;
    std::size_t               m_themeSurfaceFeedbackFrames = 0;
    std::size_t               m_themeWorkspaceFeedbackFrames = 0;
    SP<CEventLoopTimer>       m_themeWorkspaceActivationRefreshTimer;
    std::vector<ThemeWorkspaceActivationTarget> m_themeWorkspaceActivationRefreshTargets;
    std::size_t               m_themeWorkspaceActivationRefreshIndex = 0;
    std::size_t               m_themeWorkspaceActivationRefreshGeneration = 0;
    bool                      m_themeWorkspaceActivationRefreshActive = false;
    bool                      m_themeWorkspaceActivationRefreshRestoring = false;
    PHLMONITORREF             m_themeWorkspaceActivationRefreshOriginalMonitor;
    PHLWORKSPACEREF           m_themeWorkspaceActivationRefreshOriginalWorkspace;
    PHLWINDOWREF              m_themeWorkspaceActivationRefreshOriginalFocus;
    bool                      m_damageTrackingOverridden = false;
    long                      m_damageTrackingBackup = 2;
    SP<CEventLoopTimer>       m_toggleSwitchReleasePollTimer;
    SP<CEventLoopTimer>       m_postCloseCursorShapeResetTimer;
    SP<CEventLoopTimer>       m_deferredOpenTimer;
    bool                      m_deferredOpenTimerDispatching = false;
    std::unordered_map<PHLWINDOW, std::uint64_t> m_windowMruSerials;
    std::uint64_t            m_nextWindowMruSerial = 1;
    bool                      m_deactivatePending = false;
    bool                      m_deactivateScheduled = false;
    std::size_t               m_surfaceRenderDataTransformDepth = 0;
    mutable std::optional<float> m_surfaceRenderAlphaOverride;
    mutable PHLWINDOWREF         m_surfaceRenderTargetOverrideWindow;
    mutable std::optional<Rect>  m_surfaceRenderTargetOverride;
    PHLWINDOWREF              m_lastLayoutSelectedWindow;
    PHLWINDOWREF              m_queuedOverviewSelectionTarget;
    bool                      m_queuedOverviewSelectionSyncScrollingSpot = false;
    bool                      m_queuedOverviewSelectionCenterCursor = false;
    PHLWINDOWREF              m_queuedOverviewLiveFocusTarget;
    bool                      m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    bool                      m_queuedOverviewLiveFocusCenterCursor = false;
    PHLWINDOWREF              m_pendingLiveFocusWorkspaceChangeTarget;
    PHLWORKSPACEREF           m_pendingSwapColumnRelayoutCommitWorkspace;
    PHLWORKSPACEREF           m_swapColumnBackendPreviewFreezeWorkspace;
    std::chrono::steady_clock::time_point m_swapColumnBackendPreviewFreezeUntil = {};
    std::vector<ManagedWindow> m_swapColumnBackendPreviewFrozenLayout;
    PHLWINDOWREF              m_pendingWindowGeometryRetryTarget;
    PHLWORKSPACEREF           m_pendingWorkspaceChange;
    std::optional<OverviewWorkspaceChangeAction> m_pendingWorkspaceChangeAction;
    bool                      m_pendingWorkspaceChangeAllowExternalTransition = false;
    bool                      m_visibleStateRebuildScheduled = false;
    std::size_t               m_visibleStateRebuildGeneration = 0;
    bool                      m_workspaceChangeHandlingScheduled = false;
    std::size_t               m_workspaceChangeHandlingGeneration = 0;
    bool                      m_pendingWindowGeometryRetryScheduled = false;
    std::size_t               m_pendingWindowGeometryRetryRemaining = 0;
    std::size_t               m_pendingWindowGeometryRetryGeneration = 0;
    PHLWORKSPACEREF           m_pendingStripWorkspaceChangeTarget;
    std::optional<ScopeOverride> m_pendingDeferredOpenScope;
    std::optional<ScopeOverride> m_postCloseOpenDebounceScope;
    std::chrono::steady_clock::time_point m_postCloseOpenDebounceUntil = {};
    mutable std::chrono::steady_clock::time_point m_lastScrollSyncTime = {};
    std::size_t               m_cursorShapeResetFrames = 0;
    std::size_t               m_postCloseCursorShapeResetTicks = 0;
    PHLWINDOWREF              m_postCloseForcedFocus;
    bool                      m_postCloseForcedFocusLatched = false;
    std::size_t               m_ignorePostCloseMouseMoveCount = 0;
    PostCloseDispatcher       m_postCloseDispatcher = PostCloseDispatcher::None;
    std::string               m_postCloseDispatcherArgs;
    std::vector<GestureRegistration> m_registeredGestures;
    std::vector<WorkspaceNameBackup> m_workspaceNameBackups;
    GestureSession            m_gestureSession;
    ScrollGestureSession      m_scrollGestureSession;
    WorkspaceSwipeGestureContext m_workspaceSwipeGesture;
    WorkspaceTransition      m_workspaceTransition;
    std::deque<WorkspaceTransitionRequest> m_pendingWorkspaceTransitionRequests;
    // Queued overview editing dispatchers (movefocus, movecol, swapcol) to run after
    // an active workspace transition commits. This ensures animations are independent
    // and focus properly settles on the new workspace.
    struct PendingEditDispatcher {
        std::string dispatcherName;
        std::string args;
        DispatcherHandler* original = nullptr;
    };
    std::deque<PendingEditDispatcher> m_pendingEditDispatchers;
    std::vector<WorkspaceTransitionRenderStateBackup> m_workspaceTransitionRenderStateBackups;
    std::vector<WorkspaceTransitionRenderStateBackup> m_overviewRenderStateBackups;
    StripPreviewContext      m_stripPreviewContext;
    std::vector<HiddenStripLayerProxy> m_hiddenStripLayerProxies;
    std::vector<NiriWallpaperSnapshot> m_niriWallpaperSnapshots;
    PHLLS                    m_layerSnapshotCaptureLayer;
    bool                     m_applyingWorkspaceTransitionCommit = false;
    bool                     m_rebuildVisibleStateAfterWorkspaceTransitionCommit = false;
    bool                     m_workspaceTransitionCommitScheduled = false;
    bool                     m_pendingWorkspaceTransitionCommitFollowGesture = false;
    std::size_t              m_workspaceTransitionCommitGeneration = 0;
    bool                     m_beginCloseInProgress = false;
    bool                     m_toggleSwitchSessionActive = false;
    bool                     m_toggleSwitchReleaseArmed = false;
    std::size_t              m_stripSnapshotRenderDepth = 0;
    bool                     m_stripSnapshotsDirty = false;
    bool                     m_stripSnapshotRefreshScheduled = false;
    SP<CEventLoopTimer>      m_stripSnapshotRefreshTimer;
    SP<CEventLoopTimer>      m_niriWallpaperLayoutLayerRefreshTimer;
    std::size_t              m_stripSnapshotSurfaceFeedbackFrames = 0;
    std::size_t              m_overviewSurfaceFeedbackFrames = 0;
    std::size_t              m_pendingOverviewSurfaceFeedbackFrames = 0;
    bool                     m_surfaceFeedbackOverrideActive = false;
    bool                     m_surfaceFeedbackOverrideBackup = false;
    bool                     m_lastStripThemeColorValid = false;
    uint64_t                 m_lastStripThemeColor = 0;
    bool                     m_primaryButtonPressed = false;
    bool                     m_clickedWindowWasAlreadySelected = false;
    std::optional<std::size_t> m_pressedStripIndex;
    std::optional<std::size_t> m_pressedWindowIndex;
    std::optional<std::size_t> m_draggedWindowIndex;
    Vector2D                  m_pressedWindowPointer;
    Vector2D                  m_draggedWindowPointerOffset;
    NiriDragSession           m_niriDragSession;
    Vector2D                  m_hoverSelectionAnchorPointer;
    bool                      m_hoverSelectionAnchorValid = false;
    std::chrono::steady_clock::time_point m_hoverSelectionRetargetBlockedUntil = {};
    std::optional<std::size_t> m_hoverSelectionRetargetCandidateIndex;
    std::chrono::steady_clock::time_point m_hoverSelectionRetargetCandidateSince = {};
    bool                      m_hoverSelectionRetargetCandidatePrimed = false;
    bool                      m_suppressInitialHoverUpdate = false;
    std::size_t               m_postOpenRefreshFrames = 0;
    bool                      m_forceFreshOverviewOrdering = false;
    bool                      m_deferredNiriLayoutRefreshScheduled = false;
    std::size_t               m_deferredNiriLayoutRefreshGeneration = 0;
    std::size_t               m_deferredNiriLayoutRefreshRemaining = 0;
    std::string               m_deferredNiriLayoutRefreshSource;

    CHyprSignalListener       m_renderStageListener;
    CHyprSignalListener       m_mouseMoveListener;
    CHyprSignalListener       m_mouseButtonListener;
    CHyprSignalListener       m_touchDownListener;
    CHyprSignalListener       m_touchMotionListener;
    CHyprSignalListener       m_touchUpListener;
    CHyprSignalListener       m_touchCancelListener;
    CHyprSignalListener       m_keyboardListener;
    CHyprSignalListener       m_windowOpenListener;
    CHyprSignalListener       m_windowDestroyListener;
    CHyprSignalListener       m_windowCloseListener;
    CHyprSignalListener       m_windowActiveListener;
    CHyprSignalListener       m_windowMoveWorkspaceListener;
    CHyprSignalListener       m_workspaceActiveListener;
    CHyprSignalListener       m_monitorRemovedListener;
    CHyprSignalListener       m_monitorFocusedListener;
    CHyprSignalListener       m_configReloadedListener;
};

} // namespace hymission
