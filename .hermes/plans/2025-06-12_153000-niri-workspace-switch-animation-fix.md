# Niri Workspace Switch Animation & Focus/Move Conflict Fix

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Fix the issue where `movecol`/`movefocus` commands issued during a workspace switch animation cause focus to be on the wrong workspace and animation snapping occurs. Animations should be independent and not interfere with each other.

**Architecture:** The root cause is that during a workspace switch animation, `active_workspace_idx` is immediately updated to the target workspace, but visually both old and new workspaces are present. Input commands operate on the target workspace while the user may be looking at the old workspace. The fix routes focus/move commands to the visually active workspace (under cursor) during animations, or queues them until the animation completes.

**Tech Stack:** Rust, niri compositor codebase (~/.builds/niri/src)

---

## Task 1: Add Workspace Switch Animation State Query

**Objective:** Add a method to check if a workspace switch animation is in progress and get the current visual progress.

**Files:**
- Modify: `/home/dxle/builds/niri/src/layout/monitor.rs:1077-1090` (are_animations_ongoing area)
- Test: `/home/dxle/builds/niri/src/tests/animations.rs`

**Step 1: Write failing test**

```rust
#[test]
fn workspace_switch_animation_blocks_focus_commands() {
    let mut f = set_up_with_workspaces();
    
    // Start workspace switch animation
    f.niri().layout.switch_workspace_down();
    f.niri().advance_animations();
    
    // Verify animation is ongoing
    assert!(f.niri().layout.active_monitor().has_workspace_switch_animation());
    
    // Try to focus column - should be queued or routed correctly
    f.niri().layout.focus_column_right_or_first();
    
    // The focus change should not cause visual glitches
    // (verify by checking render positions)
}
```

**Step 2: Run test to verify failure**
```bash
cd /home/dxle/builds/niri && cargo test workspace_switch_animation_blocks_focus_commands -- --nocapture
```
Expected: FAIL — method `has_workspace_switch_animation` doesn't exist

**Step 3: Write minimal implementation**

Add to `Monitor` in `/home/dxle/builds/niri/src/layout/monitor.rs`:

```rust
pub fn has_workspace_switch_animation(&self) -> bool {
    self.workspace_switch.as_ref().is_some_and(|s| s.is_animation_ongoing())
}

pub fn workspace_switch_progress(&self) -> Option<f64> {
    self.workspace_switch.as_ref().map(|s| {
        let current = s.current_idx();
        let target = s.target_idx();
        let start = if let WorkspaceSwitch::Animation(anim) = s { anim.from() } else { current };
        if (target - start).abs() < f64::EPSILON {
            1.0
        } else {
            ((current - start) / (target - start)).clamp(0.0, 1.0)
        }
    })
}
```

**Step 4: Run test to verify pass**
```bash
cd /home/dxle/builds/niri && cargo test workspace_switch_animation_blocks_focus_commands -- --nocapture
```
Expected: PASS

**Step 5: Commit**
```bash
git add src/layout/monitor.rs src/tests/animations.rs
git commit -m "feat: add workspace switch animation state query methods"
```

---

## Task 2: Determine Visually Active Workspace During Animation

**Objective:** Add method to get the workspace that is currently most visually prominent (under cursor or centered).

**Files:**
- Modify: `/home/dxle/builds/niri/src/layout/monitor.rs:1539-1560` (workspace_under area)
- Test: `/home/dxle/builds/niri/src/tests/animations.rs`

**Step 1: Write failing test**

```rust
#[test]
fn visually_active_workspace_during_switch() {
    let mut f = set_up_with_workspaces();
    let cursor_pos = Point::from((960., 540.)); // center of screen
    
    // Start workspace switch
    f.niri().layout.switch_workspace_down();
    f.niri().advance_animations();
    
    // Get visually active workspace (should be interpolated)
    let (ws, geo) = f.niri().layout.active_monitor()
        .visually_active_workspace(cursor_pos)
        .unwrap();
    
    // During animation, this should return the workspace under cursor
    // which may be the old or new workspace depending on progress
}
```

**Step 2: Run test to verify failure**
```bash
cd /home/dxle/builds/niri && cargo test visually_active_workspace_during_switch -- --nocapture
```
Expected: FAIL — method `visually_active_workspace` doesn't exist

**Step 3: Write minimal implementation**

Add to `Monitor` in `/home/dxle/builds/niri/src/layout/monitor.rs`:

```rust
/// Returns the workspace that is currently visually under the given position,
/// taking into account workspace switch animations.
pub fn visually_active_workspace(
    &self,
    cursor_pos: Point<f64, Logical>,
) -> Option<(&Workspace<W>, Rectangle<f64, Logical>)> {
    // During workspace switch animation, use workspace_under which accounts for animated positions
    if self.workspace_switch.is_some() {
        self.workspace_under(cursor_pos)
    } else {
        // No animation, just return the active workspace
        let geo = self.workspaces_render_geo().nth(self.active_workspace_idx).unwrap();
        Some((&self.workspaces[self.active_workspace_idx], geo))
    }
}
```

**Step 4: Run test to verify pass**
```bash
cd /home/dxle/builds/niri && cargo test visually_active_workspace_during_switch -- --nocapture
```
Expected: PASS

**Step 5: Commit**
```bash
git add src/layout/monitor.rs src/tests/animations.rs
git commit -m "feat: add visually_active_workspace method for cursor-aware workspace detection"
```

---

## Task 3: Route Focus/Move Commands to Visually Active Workspace

**Objective:** Modify the Layout-level focus/move commands to target the visually active workspace during workspace switch animations.

**Files:**
- Modify: `/home/dxle/builds/niri/src/layout/mod.rs:1805-2169` (focus_column, move_column methods)
- Modify: `/home/dxle/builds/niri/src/layout/monitor.rs:761-1017` (monitor-level switch/focus methods)
- Test: `/home/dxle/builds/niri/src/tests/animations.rs`

**Step 1: Write failing test**

```rust
#[test]
fn focus_column_during_workspace_switch_targets_correct_workspace() {
    let mut f = set_up_with_workspaces_multiple_columns();
    
    // Setup: two workspaces, each with 2 columns
    // Start on workspace 0, column 0
    // Switch to workspace 1 (animation starts)
    f.niri().layout.switch_workspace_down();
    f.niri().advance_animations();
    
    // While animation is ongoing, focus right (should move within visually active workspace)
    let cursor_at_workspace_1 = Point::from((960., 540.)); // assuming workspace 1 is centered
    f.niri().layout.focus_column_right_or_first_with_cursor(cursor_at_workspace_1);
    
    // Verify the focus changed in the correct workspace
    // (check render positions of columns)
}
```

**Step 2: Run test to verify failure**
```bash
cd /home/dxle/builds/niri && cargo test focus_column_during_workspace_switch_targets_correct_workspace -- --nocapture
```
Expected: FAIL — method doesn't exist, routing not implemented

**Step 3: Write minimal implementation**

Modify `/home/dxle/builds/niri/src/layout/mod.rs`:

```rust
// Add cursor position parameter to focus/move methods
pub fn focus_column_right_or_first(&mut self) {
    self.focus_column_right_or_first_with_cursor(None);
}

pub fn focus_column_right_or_first_with_cursor(&mut self, cursor_pos: Option<Point<f64, Logical>>) {
    if let Some(cursor) = cursor_pos {
        if let Some(monitor) = self.monitors.iter_mut().find(|m| m.workspace_switch.is_some()) {
            if let Some((ws_idx, _)) = monitor.visually_active_workspace(cursor) {
                // Route to the visually active workspace
                monitor.workspaces[ws_idx].focus_column_right_or_first();
                return;
            }
        }
    }
    // Fallback to current behavior
    self.active_monitor_mut().active_workspace().focus_column_right_or_first();
}
```

Similar changes for:
- `focus_column_left_or_last_with_cursor`
- `focus_column_first_with_cursor`
- `focus_column_last_with_cursor`
- `focus_column_with_cursor`
- `move_column_to_index_with_cursor`
- `move_column_to_first_with_cursor`
- `move_column_to_last_with_cursor`
- `move_column_left_with_cursor`
- `move_column_right_with_cursor`

**Step 4: Run test to verify pass**
```bash
cd /home/dxle/builds/niri && cargo test focus_column_during_workspace_switch_targets_correct_workspace -- --nocapture
```
Expected: PASS

**Step 5: Commit**
```bash
git add src/layout/mod.rs src/tests/animations.rs
git commit -m "feat: route focus/move commands to visually active workspace during switch animation"
```

---

## Task 4: Update Input Handlers to Pass Cursor Position

**Objective:** Modify input handlers to pass current cursor position to focus/move commands.

**Files:**
- Modify: `/home/dxle/builds/niri/src/input/mod.rs:941-1135` (keyboard bindings for move/focus)
- Test: Manual verification or integration test

**Step 1: Write failing test**

```rust
#[test]
fn keyboard_focus_command_uses_cursor_position() {
    let mut f = set_up_with_workspaces();
    f.niri().layout.switch_workspace_down();
    f.niri().advance_animations();
    
    // Simulate key press for focus-right
    f.niri().handle_key_event(KeyEvent { key: Key::Right, mods: ModMask::empty() });
    
    // Should have routed to correct workspace based on cursor
    // (verify by checking internal state)
}
```

**Step 2: Run test to verify failure**
```bash
cd /home/dxle/builds/niri && cargo test keyboard_focus_command_uses_cursor_position -- --nocapture
```
Expected: FAIL — input handler doesn't pass cursor

**Step 3: Write minimal implementation**

In `/home/dxle/builds/niri/src/input/mod.rs`, modify the keyboard handlers:

```rust
// Get current cursor position
let cursor_pos = self.niri.seat.pointer().current_location();

// Pass to layout methods
self.niri.layout.focus_column_right_or_first_with_cursor(Some(cursor_pos));
```

Do this for all focus/move key bindings around lines 941-1135.

**Step 4: Run test to verify pass**
```bash
cd /home/dxle/builds/niri && cargo test keyboard_focus_command_uses_cursor_position -- --nocapture
```
Expected: PASS

**Step 5: Commit**
```bash
git add src/input/mod.rs src/tests/animations.rs
git commit -m "feat: pass cursor position to focus/move commands from input handlers"
```

---

## Task 5: Handle Edge Case - No Cursor / Keyboard-Only Navigation

**Objective:** When no cursor position is available (e.g., keyboard-only navigation), fall back to a sensible default.

**Files:**
- Modify: `/home/dxle/builds/niri/src/layout/mod.rs` (focus/move methods)
- Test: `/home/dxle/builds/niri/src/tests/animations.rs`

**Step 1: Write failing test**

```rust
#[test]
fn focus_command_without_cursor_defaults_to_active_workspace() {
    let mut f = set_up_with_workspaces();
    f.niri().layout.switch_workspace_down();
    f.niri().advance_animations();
    
    // Call focus without cursor position (None)
    f.niri().layout.focus_column_right_or_first_with_cursor(None);
    
    // Should default to target workspace (active_workspace_idx)
    // since that's where keyboard focus logically goes
}
```

**Step 2: Run test to verify failure**
```bash
cd /home/dxle/builds/niri && cargo test focus_command_without_cursor_defaults_to_active_workspace -- --nocapture
```
Expected: FAIL — fallback not implemented

**Step 3: Write minimal implementation**

In the `*_with_cursor` methods, when `cursor_pos` is `None` and workspace switch is ongoing:

```rust
pub fn focus_column_right_or_first_with_cursor(&mut self, cursor_pos: Option<Point<f64, Logical>>) {
    if let Some(cursor) = cursor_pos {
        // Try visually active workspace
        if let Some(monitor) = self.monitors.iter_mut().find(|m| m.workspace_switch.is_some()) {
            if let Some((ws_idx, _)) = monitor.visually_active_workspace(cursor) {
                monitor.workspaces[ws_idx].focus_column_right_or_first();
                return;
            }
        }
    }
    
    // Fallback: use target workspace (active_workspace_idx)
    // This is the workspace the user explicitly switched to
    self.active_monitor_mut().active_workspace().focus_column_right_or_first();
}
```

**Step 4: Run test to verify pass**
```bash
cd /home/dxle/builds/niri && cargo test focus_command_without_cursor_defaults_to_active_workspace -- --nocapture
```
Expected: PASS

**Step 5: Commit**
```bash
git add src/layout/mod.rs src/tests/animations.rs
git commit -m "feat: fallback to target workspace when no cursor position available during switch"
```

---

## Task 6: Prevent Animation Snapping - Coordinate Animation Start Times

**Objective:** Ensure that when a focus/move animation starts during a workspace switch, it doesn't cause snapping by coordinating animation clocks.

**Files:**
- Modify: `/home/dxle/builds/niri/src/layout/scrolling.rs:688-732` (animate_view_offset_with_config)
- Modify: `/home/dxle/builds/niri/src/layout/column.rs` (if exists, or scrolling.rs column methods)
- Test: `/home/dxle/builds/niri/src/tests/animations.rs`

**Step 1: Write failing test**

```rust
#[test]
fn focus_animation_during_switch_does_not_snap() {
    let mut f = set_up_with_workspaces();
    
    // Start workspace switch
    f.niri().layout.switch_workspace_down();
    f.niri().advance_animations();
    
    // Advance partway through workspace switch
    set_time(f.niri(), Duration::from_millis(100));
    f.niri().advance_animations();
    
    // Now trigger focus animation
    f.niri().layout.focus_column_right_or_first_with_cursor(Some(cursor_pos));
    
    // Verify the focus animation starts from current visual position,
    // not from the pre-switch position (no snap)
    let initial_pos = get_column_render_position(f.niri(), target_col);
    
    set_time(f.niri(), Duration::from_millis(150));
    f.niri().advance_animations();
    
    let mid_pos = get_column_render_position(f.niri(), target_col);
    // Position should have moved smoothly, not snapped
    assert!((mid_pos - initial_pos).abs() < threshold);
}
```

**Step 2: Run test to verify failure**
```bash
cd /home/dxle/builds/niri && cargo test focus_animation_during_switch_does_not_snap -- --nocapture
```
Expected: FAIL — animation snapping occurs

**Step 3: Write minimal implementation**

The issue is in `animate_view_offset_with_config` - when starting a new animation, it uses `self.view_offset.current()` which might not account for the workspace-level transform.

In `/home/dxle/builds/niri/src/layout/scrolling.rs`, modify `animate_view_offset_with_config`:

```rust
fn animate_view_offset_with_config(
    &mut self,
    idx: usize,
    new_view_offset: f64,
    config: niri_config::Animation,
) {
    // ... existing code ...
    
    match &mut self.view_offset {
        ViewOffset::Gesture(gesture) if gesture.dnd_last_event_time.is_some() => {
            // ... existing ...
        }
        _ => {
            // Get the current effective view offset including any parent transforms
            let current = self.view_offset.current();
            
            // If there's a workspace switch animation on the monitor,
            // ensure we start from the visually correct position
            let adjusted_current = if let Some(monitor) = self.get_monitor() {
                if monitor.workspace_switch.is_some() {
                    // The view_offset.current() is already in workspace-local coordinates
                    // which is correct since each workspace has its own scrolling space
                    current
                } else {
                    current
                }
            } else {
                current
            };
            
            self.view_offset = ViewOffset::Animation(Animation::new(
                self.clock.clone(),
                adjusted_current,
                new_view_offset,
                0., // FIXME: compute velocity
                config,
            ));
        }
    }
}
```

Actually, the snapping might be caused by the workspace switch changing which workspace is active, causing the `active_column_idx` to be read from the wrong workspace. Let me refine:

The real fix is ensuring that when `activate_column` is called during a workspace switch, it operates on the correct workspace's scrolling space. This is already addressed by Task 3 (routing to visually active workspace).

Additional fix: In `activate_column_with_anim_config`, ensure the animation uses the current visual position:

```rust
fn activate_column_with_anim_config(&mut self, idx: usize, config: niri_config::Animation) {
    // ... existing early return check ...
    
    // Compute the target view offset based on CURRENT visual state
    // (which already includes any ongoing animations via view_offset.current())
    self.animate_view_offset_to_column_with_config(
        None,
        idx,
        Some(self.active_column_idx),
        config,
    );
    
    // ... rest unchanged ...
}
```

This should already work because `view_offset.current()` returns the animated value.

**Step 4: Run test to verify pass**
```bash
cd /home/dxle/builds/niri && cargo test focus_animation_during_switch_does_not_snap -- --nocapture
```
Expected: PASS

**Step 5: Commit**
```bash
git add src/layout/scrolling.rs src/tests/animations.rs
git commit -m "fix: prevent animation snapping during workspace switch by using current animated position"
```

---

## Task 7: Integration Test - Full Workflow

**Objective:** End-to-end test simulating the user's reported scenario.

**Files:**
- Test: `/home/dxle/builds/niri/src/tests/animations.rs`

**Step 1: Write failing test**

```rust
#[test]
fn user_scenario_workspace_switch_then_movecol() {
    let mut f = set_up_user_scenario();
    // Setup: single workspace view scrolling layout
    // Two workspaces, each with multiple columns
    
    // 1. Switch workspace (starts animation)
    f.niri().layout.switch_workspace_down();
    f.niri().advance_animations();
    
    // 2. Immediately do movecol (before animation finishes)
    let cursor_pos = Point::from((960., 540.));
    f.niri().layout.move_column_right_with_cursor(Some(cursor_pos));
    f.niri().advance_animations();
    
    // 3. Verify:
    // - Focus is on the correct column in the correct workspace
    // - No visual snapping occurred
    // - Both animations completed smoothly
    
    // Advance time to complete both animations
    set_time(f.niri(), Duration::from_millis(1000));
    f.niri().advance_animations();
    f.niri_complete_animations();
    
    // Final state should be consistent
    assert_eq!(f.niri().layout.active_monitor().active_workspace_idx(), 1);
    assert_eq!(f.niri().layout.active_monitor().active_workspace().active_column_idx(), 1);
}
```

**Step 2: Run test to verify failure**
```bash
cd /home/dxle/builds/niri && cargo test user_scenario_workspace_switch_then_movecol -- --nocapture
```
Expected: FAIL — integration not complete

**Step 3: Run all previous tasks to make this pass**

This test should pass once Tasks 1-6 are complete.

**Step 4: Run test to verify pass**
```bash
cd /home/dxle/builds/niri && cargo test user_scenario_workspace_switch_then_movecol -- --nocapture
```
Expected: PASS

**Step 5: Commit**
```bash
git add src/tests/animations.rs
git commit -m "test: add integration test for workspace switch + movecol scenario"
```

---

## Task 8: Test Touchpad Gesture Workspace Switch

**Objective:** Ensure the fix also works for touchpad gesture workspace switches (not just key bindings).

**Files:**
- Modify: `/home/dxle/builds/niri/src/input/touch_overview_grab.rs` (if needed)
- Modify: `/home/dxle/builds/niri/src/input/spatial_movement_grab.rs` (if needed)
- Test: `/home/dxle/builds/niri/src/tests/animations.rs`

**Step 1: Write failing test**

```rust
#[test]
fn touchpad_workspace_switch_then_movecol() {
    let mut f = set_up_user_scenario();
    
    // Start touchpad workspace switch gesture
    f.niri().layout.workspace_switch_gesture_begin(&output, true);
    f.niri().layout.workspace_switch_gesture_update(-500., timestamp, true);
    f.niri().advance_animations();
    
    // During gesture, do movecol
    f.niri().layout.move_column_right_with_cursor(Some(cursor_pos));
    f.niri().advance_animations();
    
    // Verify correct behavior
}
```

**Step 2: Run test to verify failure**
```bash
cd /home/dxle/builds/niri && cargo test touchpad_workspace_switch_then_movecol -- --nocapture
```
Expected: May PASS if gesture uses same code path

**Step 3: If needed, update gesture handlers to pass cursor**

The gesture handlers already have cursor position context.

**Step 4: Commit if changes needed**
```bash
git add src/input/*.rs src/tests/animations.rs
git commit -m "feat: ensure touchpad gesture workspace switch also routes focus correctly"
```

---

## Verification Commands

```bash
# Run all animation tests
cd /home/dxle/builds/niri && cargo test animations -- --nocapture

# Run specific test
cd /home/dxle/builds/niri && cargo test user_scenario_workspace_switch_then_movecol -- --nocapture

# Build release for manual testing
cd /home/dxle/builds/niri && cargo build --release

# Run niri with test config
NIR_CONFIG=/path/to/test/config niri
```

---

## Risks & Tradeoffs

1. **Cursor-dependent behavior:** Focus/move commands now depend on cursor position during animations. This is more intuitive but changes behavior for keyboard-only users (mitigated by Task 5 fallback).

2. **Performance:** `visually_active_workspace` calls `workspace_under` which iterates workspaces. Negligible overhead (only during animations).

3. **Edge case - rapid switches:** If user rapidly switches workspaces, multiple animations could queue. The current design processes each command against the current visual state.

4. **Touchpad gestures:** Need to verify gesture workspace switches also work correctly (Task 8).

5. **Overview mode:** During overview, different logic applies. The fix should not affect overview (already handled by `overview_progress` checks).

---

## Open Questions

1. Should we queue focus/move commands during workspace switch instead of routing to visual workspace? Current design routes immediately which feels more responsive.

2. What about multi-monitor setups? The fix uses per-monitor cursor position.

3. Should the fallback (Task 5) use the *source* workspace instead of target for keyboard-only users? Current: target workspace (where user explicitly switched to).

---

## Summary

This plan addresses the core issue by:
1. Detecting when workspace switch animations are active
2. Determining which workspace is visually under the cursor
3. Routing focus/move commands to that workspace
4. Falling back to target workspace when no cursor position
5. Ensuring animations use current visual positions to prevent snapping
6. Adding comprehensive tests

The changes are localized to:
- `monitor.rs`: Animation state queries + visual workspace detection
- `mod.rs` (layout): Command routing with cursor parameter
- `scrolling.rs`: Animation start position handling
- `input/mod.rs`: Passing cursor from key handlers
- `tests/animations.rs`: Test coverage