Niri Overview Zoom Implementation Analysis & Hymission Adaptation Plan

    Summary of Niri's Overview Zoom Behavior

    Core Files in Niri (Rust):

    File: src/layout/monitor.rs
    Key Functions: overview_zoom() (line 1373), render_workspaces() (line 1671), render_workspace_shadows() (line 1752)
    Purpose: Computes zoom factor from animation progress; renders all workspaces scaled by zoom
    ────────────────────────────────────────
    File: src/layout/mod.rs
    Key Functions: compute_overview_zoom() (line 5014), overview_zoom() (line 2365)
    Purpose: Zoom calculation: zoom.clamp(0.0001, 0.75) with progress interpolation: (1. - p * (1. - zoom)).max(0.0001)
    ────────────────────────────────────────
    File: src/layout/workspace.rs
    Key Functions: render_scrolling() (line 1628), render_background() (line 1669)
    Purpose: Scrolling layout renders windows; workspace renders solid-color background
    ────────────────────────────────────────
    File: src/render_helpers/xray.rs
    Key Functions: Xray::render() (line 97), XrayPos struct (line 34)
    Purpose: Captures layer-shell BACKGROUND surfaces + backdrop into offscreen buffers; composites with zoom transform during overview
    ────────────────────────────────────────
    File: src/niri.rs
    Key Functions: render_inner() (line 4205)
    Purpose: Main render pipeline: backgrounds → workspaces (scaled) → backdrop (last, at full resolution)

    How Niri's Zoom Works:
    1. Animation progress (0→1) drives overview_zoom() from 1.0 down to configured overview.zoom (default ~0.1-0.3)
    2. All workspaces are scaled via RescaleRenderElement(zoom) + RelocateRenderElement — windows and workspace backgrounds scale together
    3. Xray system pre-renders layer-shell BACKGROUND surfaces + solid backdrop color into offscreen textures; these are then drawn with the same zoom transform
    4. Render order (back to front): backdrop → workspace backgrounds (scaled) → windows (scaled) → workspace shadows (scaled) → layer-shell overlays
    5. No separate "wallpaper" concept — the "wallpaper" is just the layer-shell BACKGROUND layer surface, which gets captured into Xray and zooms with everything else



    Hymission Current State (C++)

    Existing Infrastructure:
    | Location                               | Purpose                                                                                                  |
    |----------------------------------------|----------------------------------------------------------------------------------------------------------|
    | overview_controller.hpp:494            | niriModeWallpaperZoomEnabled() config getter                                                             |
    | overview_controller.cpp:346            | Config def: INT_CONF("niri_mode_wallpaper_zoom", 0)                                                      |
    | overview_controller.cpp:7833-7878      | syncNiriWallpaperSnapshots() — captures layer-shell BACKGROUND layer into framebuffers                   |
    | overview_controller.cpp:4892-4899      | renderEmptyOverviewPlaceholder() — uses captured wallpaper texture for empty workspace placeholders only |
    | overview_controller_niri_scrolling.cpp | Direct niri scrolling layout rendering (windows only, no wallpaper)                                      |

    Gap: Hymission captures the wallpaper but only renders it for empty workspace placeholders. When windows are present in the scrolling layout, the wallpaper is NOT rendered underneath them during overview zoom.



    Required Implementation for Hymission

    Goal: When plugin:hymission: { niri_zoom_wallpaper } is enabled (maps to niri_mode_wallpaper_zoom = 1), the wallpaper (layer-shell BACKGROUND surface) should zoom out together with the scrolling layout windows during overview — exactly like niri.

    Config Mapping:
    ini
    User config
    plugin:hymission: {
        niri_zoom_wallpaper = true  # → sets niri_mode_wallpaper_zoom = 1
    }




    Implementation Plan

    Files to Modify (Hymission)

    File: src/overview_controller.hpp
    Changes: Add wallpaper rendering state; ensure niriModeWallpaperZoomEnabled() is checked during active overview render
    ────────────────────────────────────────
    File: src/overview_controller.cpp
    Changes: Modify renderStage(RENDER_POST_WALLPAPER) and OverviewOverlayPassElement::draw() to render wallpaper under scrolling layout
    ────────────────────────────────────────
    File: src/overview_controller_niri_scrolling.cpp
    Changes: Add wallpaper rendering to the niri-scrolling overview path



    Task 1: Add Wallpaper Rendering State to OverviewController

    File: src/overview_controller.hpp (inside OverviewController private section)

    cpp
    // Add after NiriWallpaperSnapshot (line ~226)
    struct NiriWallpaperSnapshot {
        PHLMONITOR               monitor;
        PHLLS                    layer;
        SP<Render::IFramebuffer> framebuffer;
        // NEW: cached texture for rendering
        SP<Render::ITexture> texture;
    };

    // In State struct (around line 228), add:
    bool                 wallpaperZoomActive = false;  // true when overview open + niri_zoom_wallpaper enabled
    Rect                 wallpaperTargetRect;          // target rect in monitor coordinates for zoomed wallpaper
    double               wallpaperZoom = 1.0;          // current zoom factor (matches overview progress)




    Task 2: Compute Wallpaper Zoom in Sync with Overview Progress

    File: src/overview_controller.cpp (in updateAnimation() or render prep)

    cpp
    // In updateAnimation() or wherever overview progress is computed (around line 3156)
    void OverviewController::updateAnimation() {
        // ... existing progress computation ...
        const double progress = visualProgress();  // 0.0 to 1.0

        // NEW: Compute wallpaper zoom matching niri's formula
        if (niriModeWallpaperZoomEnabled() && niriModeAppliesToState(m_state)) {
            const double overviewZoom = niriOverviewScale();  // config: 0.0001-0.75, default ~0.15
            m_state.wallpaperZoom = std::max(0.0001, 1.0 - progress * (1.0 - overviewZoom));
            m_state.wallpaperZoomActive = (progress > 0.001);
        } else {
            m_state.wallpaperZoom = 1.0;
            m_state.wallpaperZoomActive = false;
        }

        // Compute target rect: center wallpaper on monitor, scaled by zoom
        if (m_state.wallpaperZoomActive) {
            const auto monitor = ownedMonitors().empty() ? PHLMONITOR{} : ownedMonitors().front();
            if (monitor) {
                const Vector2D monitorSize = monitor->m_size;
                const double scaledW = monitorSize.x * m_state.wallpaperZoom;
                const double scaledH = monitorSize.y * m_state.wallpaperZoom;
                m_state.wallpaperTargetRect = Rect{
                    (monitorSize.x - scaledW) * 0.5,
                    (monitorSize.y - scaledH) * 0.5,
                    scaledW,
                    scaledH
                };
            }
        }
    }




    Task 3: Render Wallpaper During Overview (Post-Wallpaper Stage)

    File: src/overview_controller.cpp — modify renderStage(RENDER_POST_WALLPAPER) (around line 3151)

    cpp
    } else if (stage == RENDER_POST_WALLPAPER) {
        // ... existing code ...
        updateOverviewWorkspaceTransition();
        updateAnimation();  // This now computes wallpaperZoom
        flushQueuedSelectionRetargetDuringOverview();
        flushQueuedRealFocusDuringOverview();

        // NEW: Render zoomed wallpaper BEFORE backdrop/placeholders
        if (m_state.wallpaperZoomActive) {
            renderNiriZoomedWallpaper();
        }

        renderBackdrop();
        renderEmptyOverviewPlaceholder(true);
        // ... rest unchanged
    }

    // NEW FUNCTION: Add after renderBackdrop() (around line 11873)
    void OverviewController::renderNiriZoomedWallpaper() const {
        const auto renderMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!renderMonitor)
            return;

        // Find wallpaper snapshot for this monitor
        const auto it = std::find_if(m_niriWallpaperSnapshots.begin(), m_niriWallpaperSnapshots.end(),
            & {
                return snap.monitor == renderMonitor && snap.framebuffer && snap.framebuffer->isAllocated();
            });

        if (it == m_niriWallpaperSnapshots.end() || !it->framebuffer->getTexture())
            return;

        const auto texture = it->framebuffer->getTexture();
        const Rect targetRect = m_state.wallpaperTargetRect;

        if (targetRect.width <= 0.0 || targetRect.height <= 0.0)
            return;

        // Render wallpaper scaled to target rect (which already has zoom applied)
        // Convert to monitor-local render coordinates
        const Rect targetLocal = rectToMonitorLocal(targetRect, renderMonitor);
        const Rect renderRect = scaleRectForRender(targetLocal, renderMonitor);

        // Alpha follows overview progress
        const double progress = visualProgress();
        const float alpha = static_cast<float>(std::clamp(progress, 0.0, 1.0));

        g_pHyprOpenGL->renderTexture(texture, toBox(renderRect), {.a = alpha});
    }




    Task 4: Integrate Wallpaper into Niri Scrolling Overview Render Path

    File: src/overview_controller_niri_scrolling.cpp — modify the main overview render entry point

    Currently the niri-scrolling windows render via OverviewOverlayPassElement::draw() which calls:
    - renderHiddenStripLayerProxies()
    - renderEmptyOverviewPlaceholder()
    - renderSelectionChrome()
    - renderWorkspaceStrip()

    We need the wallpaper to render BEFORE windows but AFTER the backdrop. The cleanest integration is in renderStage(RENDER_POST_WALLPAPER) (Task 3) since that runs for all overview modes.

    However, for the direct niri scrolling overview (usesDirectNiriScrollingOverview), ensure the wallpaper is rendered at the correct z-order. The current render order in renderStage is:


    RENDER_POST_WALLPAPER:
      1. renderBackdrop()           ← solid color / layer-shell BACKGROUND (non-zoomed)
      2. renderEmptyOverviewPlaceholder(true)
      3. renderEmptyOverviewPlaceholder()

    RENDER_POST_WINDOWS:
      4. OverviewOverlayPassElement ← windows, strip, selection chrome


    Niri's order: backdrop (full-res) → workspaces+windows (zoomed) → shadows (zoomed) → overlay layers

    Hymission fix: The wallpaper belongs in step 1-2 area (zoomed with windows). Since renderNiriZoomedWallpaper() is added in RENDER_POST_WALLPAPER before renderBackdrop(), it will render under the solid backdrop — wrong order.

    Correction: Move renderNiriZoomedWallpaper() to render after renderBackdrop() but before the window pass:

    cpp
    // In renderStage(RENDER_POST_WALLPAPER), reorder:
    renderBackdrop();
    if (m_state.wallpaperZoomActive) {
        renderNiriZoomedWallpaper();  // Zoomed wallpaper between backdrop and windows
    }
    renderEmptyOverviewPlaceholder(true);
    renderEmptyOverviewPlaceholder();
    // ...


    Then in RENDER_POST_WINDOWS, the OverviewOverlayPassElement renders windows on top.



    Task 5: Ensure Wallpaper Snapshots Are Refreshed During Overview

    File: src/overview_controller.cpp — handleConfigReloaded() (line 3202) and overview open/close

    cpp
    void OverviewController::handleConfigReloaded() {
        replaceNativeWorkspaceGestures("config-reloaded");
        if (isVisible())
            syncNiriWallpaperSnapshots();  // Already present ✓
        // ...
    }

    // In open()/toggle() — ensure snapshots captured on open
    [[nodiscard]] SDispatchResult OverviewController::open(const std::string& args) {
        // ... existing open logic ...
        if (niriModeWallpaperZoomEnabled()) {
            syncNiriWallpaperSnapshots();
        }
        // ...
    }




    Task 6: Handle Multi-Monitor Wallpaper

    File: src/overview_controller.cpp — renderNiriZoomedWallpaper()

    Extend to iterate all owned monitors:

    cpp
    void OverviewController::renderNiriZoomedWallpaper() const {
        if (!m_state.wallpaperZoomActive)
            return;

        for (const auto& monitor : ownedMonitors()) {
            if (!monitor)
                continue;

            // Find snapshot for this monitor
            const auto it = std::find_if(m_niriWallpaperSnapshots.begin(), m_niriWallpaperSnapshots.end(),
                & {
                    return snap.monitor == monitor && snap.framebuffer && snap.framebuffer->isAllocated();
                });

            if (it == m_niriWallpaperSnapshots.end() || !it->framebuffer->getTexture())
                continue;

            // Compute per-monitor target rect
            const Vector2D monitorSize = monitor->m_size;
            const double scaledW = monitorSize.x * m_state.wallpaperZoom;
            const double scaledH = monitorSize.y * m_state.wallpaperZoom;
            const Rect targetRect{
                (monitorSize.x - scaledW) * 0.5,
                (monitorSize.y - scaledH) * 0.5,
                scaledW,
                scaledH
            };

            const Rect targetLocal = rectToMonitorLocal(targetRect, monitor);
            const Rect renderRect = scaleRectForRender(targetLocal, monitor);

            if (renderRect.width <= 0.0 || renderRect.height <= 0.0)
                continue;

            const double progress = visualProgress();
            const float alpha = static_cast<float>(std::clamp(progress, 0.0, 1.0));

            // Need to bind correct monitor for rendering
            g_pHyprOpenGL->renderTexture(it->framebuffer->getTexture(), toBox(renderRect), {.a = alpha});
        }
    }




    Task 7: Config Documentation & Testing

    File: docs/config.md (or equivalent)

    markdown
    plugin:hymission:niri_zoom_wallpaper (bool, default: false)

    When enabled alongside niri-mode overview, the desktop wallpaper (layer-shell BACKGROUND surface)
    will zoom out together with the scrolling layout windows during the overview animation,
    matching niri's behavior exactly.

    Requires: niri_mode enabled and a layer-shell BACKGROUND surface (e.g., hyprpaper, swww, waypaper).




    Verification Steps

    | Step | Command / Action                                                   | Expected                                                          |
    |------|--------------------------------------------------------------------|-------------------------------------------------------------------|
    | 1    | Set plugin:hymission: { niri_zoom_wallpaper = true } in config     | Config loads without error                                        |
    | 2    | Start hyprland with hyprpaper/swww running                         | Layer-shell BACKGROUND layer detected                             |
    | 3    | Open overview (hymission toggle)                                   | Wallpaper zooms from full-screen to ~15% scale along with windows |
    | 4    | Scroll in overview                                                 | Wallpaper stays fixed relative to workspace positions             |
    | 5    | Close overview                                                     | Wallpaper zooms back to full-screen smoothly                      |
    | 6    | Multi-monitor: verify each monitor's wallpaper zooms independently | Per-monitor snapshots work                                        |
    | 7    | Toggle config off → overview shows solid backdrop only             | Feature correctly disabled                                        |



    Risks & Tradeoffs

    | Risk                                            | Mitigation                                                                                     |
    |-------------------------------------------------|------------------------------------------------------------------------------------------------|
    | Wallpaper texture not ready on overview open    | syncNiriWallpaperSnapshots() called in open path; fallback to solid color if missing           |
    | Performance: capturing framebuffer every frame  | Snapshots cached in m_niriWallpaperSnapshots; only refreshed on config reload / monitor change |
    | Multi-monitor z-order conflicts                 | Each monitor renders its own wallpaper in RENDER_POST_WALLPAPER before window pass             |
    | Niri scrolling layout changes break integration | Wallpaper render is independent of layout; only depends on m_state.wallpaperZoom               |



    Open Questions for You

    1. Config key naming: You mentioned plugin:hymission: { niri_zoom_wallpaper } — should this map to existing niri_mode_wallpaper_zoom or a new config key? (Current code uses niri_mode_wallpaper_zoom)

    use niri_mode_wallpaper_zoom;

    2. Blur on zoomed wallpaper: Niri applies blur/noise/saturation via Xray shaders. Should Hymission replicate this, or just render the raw texture with alpha?

    do not blur, just replace the behavior of the placeholder blur with whatever niri does when niri_mode_wallpaper_zoom is enabled (set to 1)

    3. Empty workspace placeholder vs. wallpaper: Currently renderEmptyOverviewPlaceholder uses wallpaper texture. With this change, empty workspaces will show the zoomed wallpaper automatically — should the placeholder rendering be suppressed when wallpaper zoom is active?

    placeholder rendering should be suppressed when wallpaper zoom is active.

    4. Dynamic wallpaper changes: If user changes wallpaper while overview is open, should it live-update? (Niri doesn't — captures on open)

    don't change it, does not need to. but leave this open in case we want to do it in the future. 

    
