# Issue 264: predictable selection and reversible navigation

Source: https://github.com/AMReX-Codes/amrexplorer/issues/264

## Status and handoff

- Approved on 2026-09-17; base: `eef8587f018211afecb519c962b8337bf0c4a9f0`.
- Delivery is three ordered commits for stacked PRs: selection controls,
  navigation history, and fixed-crosshair scanning. Each commit builds on the
  previous one. Local commits are authorized; publishing remains with the user.
- This first commit implements Phase 1 and its widget regressions. Phases 2 and
  3 follow in the next two commits. Their full agreed scope is retained below.
- The complete implementation was preserved before splitting at
  `/tmp/amrexplorer-issue264-stack/complete`.
- Validation: Clang Release build with warnings as errors succeeded; all 212
  tests run passed. Logs: `/tmp/amrexplorer-issue264-stack/phase1-{build,tests}.log`.
- Native macOS Control-click/trackpad behavior and external CI remain untested.
- The `git_version_generator` CTest is excluded because its temporary Git
  commits and tags are outside the authorization to commit this implementation.

## Summary

Implement three independently reviewable phases:

1. Escape cancellation and stable line orientation.
2. Back/Forward navigation history for image panels and line-plot windows.
3. Fixed-crosshair scanning for Cartesian 3-D views.

Keep existing navigation gestures available. New controls belong in the existing
toolbar and View menu. No remote protocol changes or persistent history are
required.

## Phase 1: selection controls

### Escape cancellation

- Track whether a rubber-band gesture is active or canceled in both `ImageView`
  and `LinePlotWidget`.
- Escape hides the selection immediately and consumes further movement and
  release events from that gesture. Releasing the button must not zoom, probe,
  or create a history entry.
- Also cancel an unfinished line-selection gesture and its preview.
- Clear gesture state when the view loses focus, its dataset changes, or the
  widget closes. Ensure the line-plot widget receives keyboard focus on
  interaction.
- The next press starts a fresh gesture normally.

### Line orientation

- Add a **Line orientation** selector with **Auto**, **Horizontal**, and
  **Vertical**, shared by the image panels in one window. Default to Auto;
  keep this setting session-local.
- In Auto, an unmodified right drag chooses its orientation once movement
  exceeds a single shared 6-pixel threshold. Choose the dominant axis; ties
  choose vertical. Lock that choice until release, including when the pointer
  reverses or returns near its starting point.
- Use the same latched orientation for the preview and the emitted line request.
- Preserve unmodified right-click slice positioning. Shift+right-click creates
  a vertical line in Auto; explicit Horizontal/Vertical overrides that
  orientation. Shift+middle-click retains its existing horizontal-line behavior.
- Keep line selection disabled wherever the existing display does not support it.

## Phase 2: navigation history

### Controls and transaction rules

- Add **Back** and **Forward** beside the image scale controls and to the View
  menu. Add corresponding buttons to each line-plot window. Use Qt's standard
  Back/Forward shortcuts, scoped to the visualization widgets.
- Maintain one shared history for all image panels in a main window and a
  separate history for each line-plot window.
- Record rubber-band zoom, wheel zoom, drag and arrow panning, scrollbar
  navigation, fixed-scale changes, and every existing Reset Zoom entry point.
- Record one action per drag or scrollbar drag, one per held arrow-key sequence,
  and one per wheel burst ending after 300 ms of inactivity. Changing the target
  panel or navigation operation closes the current group.
- Before Back/Forward, finish any pending navigation transaction. Restoring
  history must not create another entry.
- Ignore no-ops. A new action after Back discards the forward branch. Retain at
  most 100 actions in memory.

### State and restoration

- Introduce an internal navigation-history controller and value snapshots, with
  capture/restore helpers in the image-view and main-window interaction layers.
- Store requested data regions, visible display-space bounds, view centers,
  transform modes, fixed-scale factors, and the canvas/window state needed for
  ordinary, remote, mapped, and companion views. Do not copy images, cached
  planes, or worker objects.
- Capture the before/after state of every panel affected by an action.
  Synchronized zoom is one transaction; a panel-local operation restores only
  that panel.
- Restore Fit by fitting the restored region, Fixed Scale by reapplying its
  factor and center, and Custom by restoring its visible display-space bounds.
  This also defines behavior after a window resize.
- Apply restored state atomically, invalidate obsolete work immediately, then
  schedule necessary slices. Late slice or cached-render completions must not
  replace the restored state. Derive the scale indicator and overlays from the
  restored state.
- Preserve history across cosmetic changes such as palette updates. Clear it on
  dataset/frame replacement, reload, geometry or coordinate-layout changes,
  companion attachment/removal, and manual slice-position changes.
- For line plots, store automatic-range state or explicit endpoint pairs,
  preserving the existing extreme-value handling. Adding or hiding curves
  retains zoom history; clearing curves resets it.

## Phase 3: fixed-crosshair scanning

- Add a checkable **Keep crosshair fixed while panning** control to the toolbar
  and View menu, off by default and session-local.
- With it enabled, Shift+left-drag and ordinary arrow panning keep the active
  panel's crosshair intersection at its starting viewport position. Shift+arrow
  performs the same operation temporarily, regardless of the toggle.
- Reuse existing pan direction and step size. Move the image beneath the anchor
  and update the two in-plane slice coordinates together; leave the active
  panel's normal coordinate unchanged.
- Calculate coordinates from the requested navigation state and actual accepted
  pan displacement, so updates do not depend on an old raster arriving.
- Clamp the combined pan and slice movement at domain boundaries. A pan that
  cannot move must not advance the other slices.
- Enable scanning only for Cartesian 3-D panels with both crosshair guides
  visible and their intersection inside the viewport. Include local, remote,
  and Cartesian companion views; disable it for 2-D and warped displays.
- Batch slice-position publication and refresh requests to avoid intermediate
  inconsistent coordinates. Reuse existing request debouncing and stale-result
  rejection.
- Record each scan gesture as one history action containing the active panel's
  navigation change and the changed slice coordinates. Back/Forward restores
  them together.

## Validation and delivery

- **Selection:** exercise actual press → move → Escape → release sequences in
  image and line-plot widgets. Verify unchanged zoom, no probe or line request,
  no history entry, and successful subsequent gestures.
- **Orientation:** test threshold boundaries, diagonal motion, direction
  reversal, returning to the origin, explicit H/V selection, and agreement
  between preview and final request.
- **History:** test nested zooms, pan, fixed scale, Reset Zoom, grouping, no-ops,
  branch truncation, capacity, resize, lifecycle resets, and independent
  line-plot history.
- **Rendering:** cover 2-D, synchronized and unsynchronized 3-D panels, remote
  virtual canvases, mapped displays, and companions. Gate background workers
  to test zoom → Back → Forward while earlier results remain in flight.
- **Scanning:** verify all three Cartesian planes, drag and keyboard input,
  fixed viewport anchors, boundaries, local/remote behavior, companions, and
  undo/redo of both pan and slice coordinates.
- Build successfully before running the focused Qt tests and affected smoke
  tests. Run the broader suite after integration, and validate Control-click
  and trackpad interaction on native macOS. Report unavailable native validation
  explicitly.
- Update the user guide's controls table and describe Back/Forward and scanning
  briefly. Deliver the phases in order; commits and GitHub operations remain
  subject to the repository's approval rules.
