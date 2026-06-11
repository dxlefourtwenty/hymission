# hymission

`hymission` is a Hyprland plugin that provides a Mission Control-style overview with live compositor-side previews, scope-aware collection, trackpad gestures, and a workspace strip for active-workspace overview mode.

> [!IMPORTANT]
> This project is a fork to accomidate my own custom desktop environment. Use at your own discretion.

> [!WARNING]
> Hyprland plugins run inside the compositor process. Install plugins only from sources you trust.
> `hymission` may not work correctly on NVIDIA GPUs/drivers.

> [!WARNING]
> This software is 99% vibe coded with OpenAI CodeX, but have been manual audited, warn in case you mind it.

**Inspired By Apple Mission Control**

**Referenced [hyprexpo](https://github.com/hyprwm/hyprland-plugins/tree/main/hyprexpo), [hycov](https://github.com/ernestoCruz05/hycov), and [Hyprspace](https://github.com/KZDKM/Hyprspace).**
## Features

- Mission Control-style overview with animated window previews
- Scope control with default config scope, `onlycurrentworkspace`, and `forceall`
- Mouse, keyboard, and trackpad-driven overview interaction
- Optional selected-preview expansion with local push-away animation
- Gesture-only `recommand` mode for two-sided `toggle` gestures
- Workspace strip when the current overview scope shows only the active workspace
- Multi-monitor support
- Pinned-window, special-workspace, and scrolling-layout aware behavior
- Workspace-to-workspace overview transitions without showing the native workspace animation in the middle



https://github.com/user-attachments/assets/d3e7625f-a831-474a-ac85-02dca635beda




## Installation

### Install with `hyprpm`

`hyprpm` is the preferred user-facing install path in the Hyprland ecosystem.

```sh
hyprpm update
hyprpm add https://github.com/gfhdhytghd/hymission
hyprpm enable hymission
hyprpm reload
```

If you use Hyprland's permission system, you may need to allow `hyprpm` in your config:

```conf
permission = /usr/(bin|local/bin)/hyprpm, plugin, allow
```

Do not also manually `hyprctl plugin load` the same plugin if you manage it through `hyprpm`.

### Manual build and reload

For local development, `hymission` uses CMake and outputs `build-cmake/libhymission.so`.

Requirements:

- Hyprland development headers for the exact Hyprland build you are running
- `cmake`
- `pkg-config`
- a C++23-capable compiler

Build:

```sh
cmake -DCMAKE_BUILD_TYPE=Release -B build-cmake
cmake --build build-cmake -j"$(nproc)"
ctest --test-dir build-cmake --output-on-failure
```

Safe reload sequence on this machine:

```sh
hyprctl plugin unload "$(pwd)/build/libhymission.so"
hyprctl plugin unload "$(pwd)/build-cmake/libhymission.so"
hyprctl plugin unload "$(pwd)/build-meson/libhymission.so"
hyprctl plugin load "$(pwd)/build-cmake/libhymission.so"
hyprctl plugin list
```

`plugin not loaded` is expected when the unloaded path is not the active copy.

Build outputs:

- Plugin: `build-cmake/libhymission.so`
- Layout demo: `build-cmake/hymission-layout-demo`
- Layout test: `build-cmake/hymission-mission-layout-test`
- Logic test: `build-cmake/hymission-overview-logic-test`

## Usage

### Dispatchers

```conf
bind = SUPER, TAB, hymission:toggle
bind = SUPER SHIFT, TAB, hymission:open
bind = SUPER CTRL, TAB, hymission:close
bind = SUPER, C, hymission:toggle,onlycurrentworkspace
bind = SUPER, A, hymission:toggle,forceall
bind = SUPER, M, hymission:debug_current_layout
```

| Dispatcher | Description |
| --- | --- |
| `hymission:toggle` | Toggle overview. Supports `onlycurrentworkspace` and `forceall`. |
| `hymission:open` | Open overview. Supports `onlycurrentworkspace` and `forceall`. |
| `hymission:close` | Close overview. |
| `hymission:debug_current_layout` | Compute the current layout and show a notification summary without entering overview. |

Scope arguments:

- no argument: use the default config-driven collection scope
- `onlycurrentworkspace`: show only the current regular workspace on the anchor monitor
- `forceall`: show all regular workspaces across participating monitors and include currently visible special workspaces

### Toggle Switch Mode

`toggle_switch_mode` only affects `hymission:toggle`.

With a binding such as `bind = SUPER, TAB, hymission:toggle` and:

```conf
toggle_switch_mode = 1
switch_toggle_auto_next = 1
switch_release_key = Super_L
```

- the first `SUPER+TAB` opens overview as a switch session
- repeated `TAB` presses while `SUPER` stays held cycle to the next overview target
- releasing `SUPER` commits the current selection and exits overview

`hymission:open`, `hymission:close`, and gesture paths keep their normal behavior. Toggle switch mode is meant for modifier-backed `hymission:toggle` bindings such as `ALT+TAB` / `SUPER+TAB`.

### Lua dispatchers

When using Hyprland's Lua config, Hymission exposes native plugin functions under `hl.plugin.hymission`:

```lua
hl.bind("SUPER + TAB", hl.plugin.hymission.toggle())
hl.bind("SUPER + A", function()
    hl.dispatch(hl.plugin.hymission.toggle("forceall"))
end)
hl.bind("SUPER + S", hl.plugin.hymission.open("onlycurrentworkspace"))
hl.bind("SUPER + Escape", hl.plugin.hymission.close())
hl.bind("SUPER + 1", hl.plugin.hymission.workspace("1"))
```

Available functions:

- `hl.plugin.hymission.toggle(args?)` returns an `HL.Dispatcher`
- `hl.plugin.hymission.open(args?)` returns an `HL.Dispatcher`
- `hl.plugin.hymission.close()` returns an `HL.Dispatcher`
- `hl.plugin.hymission.debug_current_layout()` returns an `HL.Dispatcher`
- `hl.plugin.hymission.workspace(args)` returns an `HL.Dispatcher` that routes through Hymission's workspace-transition interception while overview is visible
- `hl.plugin.hymission.dispatch(name, args?)` returns an `HL.Dispatcher`
- `hl.plugin.hymission.gesture(table|string, disable_inhibit?)`

`toggle` and `open` accept the same optional scope arguments as the legacy dispatchers: `forceall` and `onlycurrentworkspace`.

### Gestures

Use Hyprland's official gesture syntax. Scrolling layout panning can use either Hymission's compatibility gesture or Hyprland's native `scrollMove`:

```conf
gesture = 4, vertical, dispatcher, hymission:toggle,forceall
gesture = 4, vertical, dispatcher, hymission:toggle,recommand
gesture = 4, vertical, dispatcher, hymission:open,onlycurrentworkspace
gesture = 3, horizontal, dispatcher, hymission:scroll,layout
# or: gesture = 3, horizontal, scrollMove
gesture = 3, vertical, workspace
```

Lua config should register Hymission gestures through `hl.plugin.hymission.gesture(...)` instead of `hl.gesture({ action = function() ... end })` when you want continuous overview progress:

```lua
hl.plugin.hymission.gesture({
    fingers = 4,
    direction = "vertical",
    action = "toggle",
    args = "forceall",
})

hl.plugin.hymission.gesture({
    fingers = 4,
    direction = "vertical",
    action = "toggle",
    recommand = true,
})

hl.plugin.hymission.gesture({
    fingers = 4,
    direction = "vertical",
    action = "open",
    scope = "onlycurrentworkspace",
})

hl.plugin.hymission.gesture({
    fingers = 3,
    direction = "horizontal",
    action = "scroll",
    mode = "layout",
})

-- Native alternative:
-- hl.gesture({ fingers = 3, direction = "horizontal", action = "scroll_move" })

hl.plugin.hymission.gesture({
    fingers = 3,
    direction = "vertical",
    action = "workspace",
})
```

Optional gesture fields are `mods`, `scale`, and `disable_inhibit`.

Gesture notes:

- `vertical` and `horizontal` are supported for plugin-managed overview gestures; `hymission:scroll,layout` also supports `swipe`
- unofficial shorthand such as `gesture = ..., hymission:toggle,...` is not supported
- default gesture semantics are state-aware: hidden overview opens in the configured direction, and visible `hymission:toggle,*` overview can close in either swipe direction
- `recommand` is gesture-only and is only valid with `hymission:toggle`
- scrolling layout movement supports both `hymission:scroll,layout` and Hyprland's official `scrollMove` / Lua `scroll_move`
- workspace swipes should use Hyprland's standard `gesture = ..., workspace`; Hymission already intercepts that path while overview is visible
- in `recommand` mode, one side opens `forceall` and the other side opens `onlycurrentworkspace`
- switching from one visible `recommand` side to the other only works in the side-changing direction; it must pass through hidden state and then cross a small transfer gap before the opposite side starts opening
- swiping the other visible `recommand` direction only exits overview back to hidden and does not continue into the opposite side
- a gesture that started from hidden can still be pulled back to cancel, but it cannot become a new visible-start close/transfer gesture until you lift and swipe again
- release still uses a `50% + velocity` commit rule

## Configuration

All user-facing settings live under `plugin:hymission`.

Example:

```conf
plugin {
    hymission {
        outer_padding_top = 92
        outer_padding_right = 32
        outer_padding_bottom = 32
        outer_padding_left = 32
        row_spacing = 32
        column_spacing = 32
        min_window_length = 120
        min_preview_short_edge = 32
        small_window_boost = 1.35
        max_preview_scale = 0.95
        workspace_overview_max_preview_scale = 0.95
        min_slot_scale = 0.10
        natural_scale_flex = 0.22
        layout_engine = grid
        layout_scale_weight = 1.0
        layout_space_weight = 0.10

        expand_selected_window = 1
        multi_workspace_expand_selected_window = 1
        overview_focus_follows_mouse = 1
        refresh_previews_on_config_reload = 1
        strip_theme_surface_feedback_frames = 300
        multi_workspace_sort_recent_first = 1
        niri_mode = 0
        niri_scroll_pixels_per_delta = 1.0
        niri_layout_scale = 1.0
        niri_overview_scale = 0.65
        niri_window_gaps = -1.0
        niri_workspace_gap = -1.0
        niri_workspace_scale = 1.0
        niri_strip_workspace_zoom = 2.0
        niri_mode_show_empty_workspaces_btwn = 1
        niri_mode_wallpaper_zoom = 0
        niri_preview_disabled = 0
        niri_overview_animations = 1
        niri_overview_open_close_speed_multiplier = 1.5
        toggle_switch_mode = 1
        switch_toggle_auto_next = 1
        switch_release_key = Super_L
        gesture_invert_vertical = 0
        one_workspace_per_row = 0
        only_active_workspace = 0
        only_active_monitor = 0
        show_special = 0
        workspace_change_keeps_overview = 1
        damage_tracking_override = 1
        close_special_workspaces_on_open = 1

        workspace_strip_anchor = left
        workspace_strip_empty_mode = existing
        workspace_strip_thickness = 160
        workspace_strip_gap = 24
        hide_bar_when_strip = 1
        hide_bar_namespaces = hypr-dock,waybar,chromack,wardnc,wardbnc,dashboard,rofi
        hide_layers_when_overview = 1
        hide_overview_layer_namespaces = chromack,wardnc,wardbnc,dashboard,rofi
        hide_bar_animation = 1
        hide_bar_animation_blur = 1
        hide_bar_animation_move_multiplier = 0.8
        hide_bar_animation_scale_divisor = 1.1
        hide_bar_animation_alpha_end = 0
        bar_single_mission_control = 0
        show_focus_indicator = 0

        debug_logs = 0
        debug_surface_logs = 0
    }
}
```

Lua config uses the same names under `plugin.hymission`:

```lua
hl.config({
    plugin = {
        hymission = {
            outer_padding_top = 92,
            layout_engine = "grid",
            niri_mode = 0,
            niri_layout_scale = 1.0,
            niri_overview_scale = 0.65,
            niri_workspace_gap = -1.0,
            niri_mode_wallpaper_zoom = 0,
            niri_preview_disabled = 0,
            niri_overview_animations = 1,
            niri_overview_open_close_speed_multiplier = 1.5,
            multi_workspace_expand_selected_window = 1,
            switch_release_key = "Super_L",
            workspace_strip_anchor = "left",
        },
    },
})
```

### Layout options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `outer_padding` | int | `32` | Legacy fallback for all four edge paddings. |
| `outer_padding_top` | int | `92` | Top padding for the overview content area. |
| `outer_padding_right` | int | `32` | Right padding for the overview content area. |
| `outer_padding_bottom` | int | `32` | Bottom padding for the overview content area. |
| `outer_padding_left` | int | `32` | Left padding for the overview content area. |
| `row_spacing` | int | `32` | Vertical spacing between preview rows. |
| `column_spacing` | int | `32` | Horizontal spacing between preview columns. |
| `min_window_length` | int | `120` | Minimum edge length used before layout scoring. |
| `min_preview_short_edge` | int | `32` | Minimum rendered short edge for previews, used to keep ultra-wide, ultra-tall, or very small windows recognizable. |
| `small_window_boost` | float | `1.35` | Weight boost applied to smaller windows during layout. |
| `max_preview_scale` | float | `0.95` | Maximum preview scale for all-workspace / multi-workspace overview. |
| `workspace_overview_max_preview_scale` | float | `0.95` | Maximum preview scale for active-workspace overview, including niri direct overview. |
| `min_slot_scale` | float | `0.10` | Minimum allowed slot scale. |
| `natural_scale_flex` | float | `0.22` | Natural-engine-only free scale range. Values are clamped to `0.0` - `0.25`; recent-first multi-workspace ordering keeps earlier windows visibly larger, while natural layouts may use larger per-window scale differences to fill sparse space. |
| `layout_engine` | string | `grid` | Geometry solver. `grid` keeps the existing row-search layout; `natural`, `apple`, `expose`, and `mission-control` enable the Apple-like natural solver that tries to preserve original window positions while removing overlap. The natural engine attempts every window count and only uses row-search as an emergency fallback if solving fails. |
| `layout_scale_weight` | float | `1.0` | Weight of preview scale in the layout scoring pass. |
| `layout_space_weight` | float | `0.10` | Weight of space utilization in the layout scoring pass. |
| `one_workspace_per_row` | bool | `0` | Keep each workspace on its own row instead of searching for the best row count. |

### Behavior options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `expand_selected_window` | bool | `1` | Enlarge the selected preview and push nearby previews away without reshuffling the whole overview grid. Uses the overview-selected target, which usually follows hover when `overview_focus_follows_mouse = 1`. |
| `multi_workspace_expand_selected_window` | bool | `1` | Multi-workspace overview only. Use the same selected-window magnification and neighbor push behavior as the non-niri single-workspace overview. |
| `overview_focus_follows_mouse` | bool | `1` | Keep the overview selection aligned with hover, and sync real focus when allowed. Hover retargeting is frame-coalesced for smoother animation, and multi-workspace overview stays visually anchored when real focus crosses workspaces. |
| `overview_center_cursor_on_hover_focus` | bool | `1` | Move the cursor to the newly focused preview center after hover focus recenters a niri scrolling overview. This prevents a stationary edge hover from chaining through adjacent windows. |
| `refresh_previews_on_config_reload` | bool | `1` | Repaint cached workspace-strip preview snapshots after Hyprland config reloads, so live theme changes update preview colors while overview is open. |
| `strip_theme_surface_feedback_frames` | int | `300` | Number of frames to force surface feedback (sending wl_surface.frame callbacks to background windows) after a theme or config reload, giving hidden applications time to receive the reload and repaint their colors. |
| `multi_workspace_sort_recent_first` | bool | `1` | Multi-workspace overview only. When enabled, `forceall` and any default overview scope that spans multiple workspaces place more recently used windows earlier in the grid, filling left-to-right then top-to-bottom. |
| `niri_mode` | bool | `0` | Enable niri-like overflow behavior for the edge workspace strip. This is opt-in and does not turn the strip into the main overview content. |
| `niri_scroll_pixels_per_delta` | float | `1.0` | Multiplier for `hymission:scroll,layout` movement outside overview. A value of `1.0` maps roughly one `gestures:workspace_swipe_distance` of finger travel to one viewport of scrolling-layout movement. Native `scrollMove` ignores this option. |
| `niri_layout_scale` | float | `1.0` | Extra scale applied to niri-mode active-workspace overview window targets, including direct scrolling-layout previews. Values are clamped to `0.50` - `2.0`. |
| `niri_overview_scale` | float | `0.65` | Extra zoom factor for scrolling-layout windows in niri overview. Lower values reveal more neighboring windows in the scroll order; values are clamped to `0.05` - `1.0`. |
| `niri_window_gaps` | float | `-1.0` | Visible gap between direct niri scrolling-layout previews in single-workspace overview. `-1.0` uses the current `general:gaps_in`; non-negative values are clamped to `0.0` - `160.0`. Legacy `niri_single_ws_gap_pixels` and `niri_single_ws_gap_multiplier` remain fallback inputs when explicitly set. |
| `niri_workspace_gap` | float | `-1.0` | Visible gap between niri-mode workspace scroll strips in single-workspace overview. Values below `0.0` fall back to `general:gaps_out`; `niri_multi_ws_gap` remains supported as a legacy fallback. |
| `niri_workspace_scale` | float | `1.0` | Niri mode strip thumbnail scale inside the configured strip thickness. Values are clamped to `0.05` - `1.0`; `1.0` uses the full strip cross-axis size. |
| `niri_strip_workspace_zoom` | float | `2.0` | Extra zoom for scrolling-layout windows inside niri mode's active-workspace strip thumbnails only. Values are clamped to `0.05` - `4.0`; this does not affect the main overview, multi-workspace overview, non-niri strip thumbnails, or non-scrolling layouts. |
| `niri_mode_show_empty_workspaces_btwn` | bool | `1` | In niri-mode single-workspace scrolling overview, include numeric empty-workspace gaps between occupied workspaces. Set to `0` to only show existing workspace objects. |
| `niri_mode_wallpaper_zoom` | bool | `0` | Single-workspace scrolling niri mode only. Render the monitor background layer inside each workspace viewport. When disabled, workspace viewports use the blurred placeholder card. |
| `niri_preview_disabled` | bool | `0` | Legacy compatibility option. The preview strip is always disabled in single-workspace scrolling niri mode. |
| `niri_overview_animations` | bool | `1` | Keep live Hyprland window movement available while the niri overview is open. Open/close and relayout motion use `windowsMove`; workspace switching uses `workspaces`. |
| `niri_overview_open_close_speed_multiplier` | float | `1.5` | Speed multiplier applied to the live `windowsMove` animation for niri single-workspace overview open and close transitions. The configured curve and style are preserved. |
| `toggle_switch_mode` | bool | `1` | Turn `hymission:toggle` into a toggle-only switch session. Intended for modifier-backed bindings such as `ALT+TAB` / `SUPER+TAB`. |
| `switch_toggle_auto_next` | bool | `1` | Toggle switch mode only. When enabled, the first switch-mode `toggle` both opens overview and advances to the next target. |
| `switch_release_key` | string | `Super_L` | Toggle switch mode only. Release of this key commits the current selection and closes the switch session. Supports keysym names such as `Alt_L` / `Super_L` and `code:N`, and release tracking is resilient to missing per-window release events. |
| `gesture_invert_vertical` | bool | `0` | Invert the plugin-managed vertical overview gesture direction. |
| `only_active_workspace` | bool | `0` | Restrict the default scope to the active regular workspace per participating monitor. |
| `only_active_monitor` | bool | `0` | Restrict the default scope to the monitor under the cursor. |
| `show_special` | bool | `0` | Include currently visible special workspaces in the default scope. |
| `workspace_change_keeps_overview` | bool | `1` | Keep overview open when switching workspaces in active-workspace scope. |
| `damage_tracking_override` | bool | `1` | Temporarily set Hyprland `debug:damage_tracking` to `0` while overview is visible and restore the previous value on close. This can reduce flicker on NVIDIA/multi-monitor setups. |
| `close_special_workspaces_on_open` | bool | `1` | Close any currently visible special workspaces before opening overview. Applies to both multi-workspace and active-workspace overview. |
| `show_focus_indicator` | bool | `0` | Render selected and hovered preview focus chrome. |

Behavior notes:

- In multi-workspace overview, hover-driven real focus may still cross workspaces, but the overview grid stays anchored instead of rebuilding on every workspace change.
- In active-workspace overview, workspace changes still use the dedicated overview-to-overview transition path.
- Toggle switch mode keeps current hover semantics: if `overview_focus_follows_mouse = 1`, moving the pointer can still retarget the final committed selection during the switch session.

### Workspace strip options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `workspace_strip_anchor` | string | `left` | Strip anchor. Supports `top`, `left`, and `right`. |
| `workspace_strip_empty_mode` | string | `existing` | Empty-workspace strip policy. `existing` only shows real workspaces; `continuous` inserts the next missing numbered workspace in each positive-id gap without expanding named-workspace spans. |
| `workspace_strip_thickness` | int | `160` | Strip thickness. |
| `workspace_strip_gap` | int | `24` | Gap between the strip and the main overview content. |
| `hide_bar_when_strip` | bool | `1` | Replace matching exclusive bars with a short self-blur / slide / scale proxy handoff while the strip is shown. |
| `hide_bar_namespaces` | string | `hypr-dock,waybar,chromack,wardnc,wardbnc,dashboard,rofi` | Comma-separated layer-surface namespaces to hide with the same strip handoff, including non-exclusive surfaces. Exclusive bars are still hidden regardless of namespace. |
| `hide_layers_when_overview` | bool | `1` | Hide matching non-window layer surfaces while multi-workspace Mission Control is shown. |
| `hide_overview_layer_namespaces` | string | `chromack,wardnc,wardbnc,dashboard,rofi` | Comma-separated layer-surface namespaces to fade out during multi-workspace Mission Control. |
| `hide_bar_animation` | bool | `1` | Enable the bar handoff animation. When disabled, matching bars hide/show instantly with the strip. |
| `hide_bar_animation_blur` | bool | `1` | Enable blur during the bar handoff. When disabled, the handoff keeps alpha / move / scale only. |
| `hide_bar_animation_move_multiplier` | float | `0.8` | Multiplier for how much the bar follows strip movement. Clamped to `0.0` - `2.0`. `1.0` matches full strip travel and `2.0` doubles it. |
| `hide_bar_animation_scale_divisor` | float | `1.1` | Bar scale divisor at full strip reveal. A value of `n` means the proxy scales to `1 / n` of its original size at maximum. `1.0` disables scaling. |
| `hide_bar_animation_alpha_end` | float | `0.0` | Final bar proxy alpha when the strip is fully revealed. Clamped to `0.0` - `1.0`. `0.0` fully fades out; higher values keep part of the bar visible. |
| `bar_single_mission_control` | bool | `0` | Multi-workspace overview only. Keep this at `0` to preserve the bar's normal numbered workspace display. When enabled, the bar workspace list collapses to a single `Mission Control` entry and the other regular overview workspaces are renamed to an internal hidden prefix so bars can filter them out. Intended for Waybar `ignore-workspaces`. |

The workspace strip is shown when the current overview scope displays only the active workspace.
By default it only shows real workspaces plus the trailing new-workspace card. In `continuous` mode, synthetic empty workspaces progressively expose numbered gaps one slot at a time and render the monitor background/wallpaper when available; the trailing new-workspace card keeps its dedicated `+` styling.
With `niri_mode = 1`, `only_active_workspace = 1`, and a Hyprland `scrolling` layout, there is no separate preview strip. Workspaces share one overview scene and use `niri_workspace_gap` for spacing. Tiled previews are rebuilt from Hyprland's live scrolling layout geometry, so resize, move, swap, and focus dispatchers remain owned by Hyprland. `focus_fit_method = 0` centers the focused column; `focus_fit_method = 1` fits it inside the viewport. Both `hymission:scroll,layout` and Hyprland's official `scrollMove` / Lua `scroll_move` can scroll the layout while overview is open.

### Optional Waybar Single-Entry Setup

Leave `bar_single_mission_control = 0` if you want `hyprland/workspaces` to keep showing the usual numbered workspaces.

If you explicitly want `hyprland/workspaces` to collapse to a single `Mission Control` button while multi-workspace overview is visible:

1. Set `bar_single_mission_control = 1` in `plugin:hymission`.
2. Add an `ignore-workspaces` rule that hides the plugin's temporary names:

```jsonc
"hyprland/workspaces": {
  "all-outputs": true,
  "disable-scroll": true,
  "on-click": "activate",
  "persistent_workspaces": {},
  "ignore-workspaces": ["^__hymission_hidden__:"]
}
```

This keeps normal workspace names untouched outside overview. While overview is open, the anchor workspace remains `Mission Control` and the other regular overview workspaces are renamed to the hidden prefix so Waybar drops them from the module.

### Debug options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `debug_logs` | bool | `0` | Enable overview debug logging. |
| `debug_surface_logs` | bool | `0` | Enable more verbose surface-level debug logging. |

## Development

Useful commands:

```sh
./build-cmake/hymission-layout-demo
./build-cmake/hymission-layout-demo --list-scenes
./build-cmake/hymission-layout-demo --scene forceall --engine natural --output /tmp/hymission-forceall-natural.svg
./build-cmake/hymission-layout-demo --scene forceall --engine grid --output /tmp/hymission-forceall-grid.svg
./build-cmake/hymission-layout-demo --stress 5000 --seed 1 --output /tmp/hymission-stress-worst.svg
./build-cmake/hymission-mission-layout-test
./build-cmake/hymission-overview-logic-test
hyprctl dispatch hymission:debug_current_layout
```

`hymission-layout-demo` runs the geometry solver without loading the Hyprland plugin. In SVG output, dashed rectangles are source window geometry and solid rectangles are overview targets. Built-in scenes include `forceall`, `default`, `stacked`, `right-biased`, and `workspace-rows`. It also reports gravity, heatmap balance, motion, and x/y inversion metrics; SVG output draws heat cells, the screen center, and the target-area centroid. `--stress` generates random pathological scenes and writes the worst-scoring case for solver tuning.

Project docs:

- [`docs/spec.md`](docs/spec.md): behavior and user-facing semantics
- [`docs/architecture.md`](docs/architecture.md): controller, hooks, and state-machine structure
- [`docs/research.md`](docs/research.md): layout tradeoffs and prior-art notes
- [`docs/workspace_strip_plan.md`](docs/workspace_strip_plan.md): strip-specific implementation planning
- [`docs/todo.md`](docs/todo.md): current gaps and next steps
- [`devlog/`](devlog): implementation notes for recent iterations

## Notes

- The repository includes a root [`hyprpm.toml`](hyprpm.toml) manifest, which is expected by `hyprpm`.
- For inclusion in the official `hyprland-plugins` repository, Hyprland asks plugin authors to coordinate with the repository maintainer first.
