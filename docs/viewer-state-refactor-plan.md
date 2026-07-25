# Viewer State Architecture Refactor Plan

Status: Implemented

Scope: Architectural refactor only; no user-visible feature work

Implementation completed: 2026-07-24

## Summary

`MainWindow` currently owns both the Qt interface and most of the viewer state
machine. It creates and replaces datasets, stores expression definitions,
reconciles field selections, restores ranges, coordinates sequence frames and
prefetches, invalidates cached views, and schedules asynchronous work. The same
state transition is consequently implemented several times for initial loads,
expression edits, sequence frames, FAB transitions, and prefetched frames.

This plan introduces a Qt-independent `ViewerSession` module. One
`ViewerSession` represents the logical state of one viewer window and the
resolved snapshot of the dataset it currently displays. `MainWindow` becomes a
Qt adapter: it translates widget actions into session actions, schedules the
work requested by the session, and projects accepted snapshots back into
widgets.

The refactor will preserve the current parser, `PlotfileDataset`, rendering
algorithms, sequence behavior, expression-list file format, and cancellation
mechanisms. It will proceed incrementally behind the existing tests.

## Motivation

The expression editor exposed a broader architectural problem rather than an
isolated parser problem. Viewer state currently has several representations:

- `m_derivedFields` stores expression definitions requested for future loads.
- `m_installedDerivedFields` stores the definitions successfully installed in
  the current dataset.
- `PlotfileDataset` metadata stores the actual fields and their numeric IDs.
- The field selector stores numeric field IDs and displays field names.
- The editor uses list rows as definition identity.
- `FrameSliceSpec` carries both numeric field IDs and field names.
- Range and vector state are stored by numeric field ID and remapped by name
  during dataset replacement.
- Asynchronous compatibility is distributed across dataset, specification,
  prefetch, and per-view generation counters.

These representations are individually understandable, but no module owns the
invariants between them. Every workflow must remember which representations to
capture, translate, replace, restore, and invalidate.

Recent review fixes are examples of that missing locality:

- Record only expressions successfully installed in a frame.
- Preserve fields by name when failed definitions compact numeric IDs.
- Distinguish desired definitions from the installed subset.
- Prevent playback and debounce timers from changing state inside the modal
  editor.
- Preserve displayed-field identity independently of the selected editor row.

Adding another special case to `MainWindow` can fix each individual bug, but it
does not reduce the number of state combinations that future changes must
handle.

## Goals

1. Establish one authoritative owner for viewer intent and resolved dataset
   state.
2. Give expression definitions and field selections stable identities that do
   not depend on editor rows or dataset-local numeric IDs.
3. Use one dataset-preparation path for initial load, expression Apply,
   sequence frames, FAB transitions, and prefetch.
4. Use one snapshot-acceptance path and one Qt projection path for every
   successful dataset replacement.
5. Make stale-result rejection an invariant of the viewer state machine.
6. Preserve unavailable expression and field intent across heterogeneous
   sequence frames.
7. Test state transitions through a Qt-independent interface.
8. Reduce `MainWindow` to Qt construction, event adaptation, work scheduling,
   and widget projection.

## Non-goals

- Change expression grammar, evaluation, caching, or CPU-only execution.
- Change `PlotfileDataset::addDerivedField()` semantics.
- Change the expression-list JSON format.
- Change slice, contour, vector, line-query, or scalar-rendering algorithms.
- Introduce a new general-purpose task scheduler.
- Replace QtConcurrent, `QFutureWatcher`, stop tokens, or per-view request
  coalescing.
- Rewrite all of `MainWindow` at once.
- Introduce virtual interfaces or dependency injection for in-process
  dependencies that have only one implementation.
- Implement the remote viewer architecture.
- Change user-visible fallback behavior without a separate reviewed decision.

## Terminology

The following terms are used throughout this plan.

### Viewer session

The logical state belonging to one top-level viewer window. Independent windows
own independent viewer sessions.

### Viewer intent

User choices that should survive replacement of the underlying dataset when
possible. Examples include desired expressions, selected fields, per-field
ranges, vector fields, level selection, display mode, palette, logarithmic
mode, contour count, slice positions, and visible regions.

### Viewer snapshot

A fully resolved view of one dataset at one session revision. It contains the
`PlotfileDataset`, bindings from stable field keys to dataset-local numeric
field IDs, expression-installation statuses, resolved fallbacks, metadata, and
any initial rendered displays prepared for installation.

### Field key

A stable logical identity for a field:

- A native field is identified by its dataset name.
- A derived field is identified by its expression ID.

A `FieldId` is not a field key. It is a dataset-local numeric binding valid only
within one viewer snapshot.

### Expression catalog

The ordered set of expressions desired by the user. Order remains significant:
an expression may reference only native fields and earlier derived fields,
preserving current behavior.

### Session revision

A monotonically increasing value identifying the viewer intent from which work
was prepared. A result may be accepted only if its revision is still current.

## Current Architecture

### Expression editing

`MainWindow::showExpressionEditor()` currently:

1. Rejects entry while `m_activeRequests` is nonzero.
2. Pauses playback and debounce timers.
3. Copies `m_installedDerivedFields` into a local draft.
4. Uses list rows and a separately maintained displayed-definition index as
   identity.
5. Imports, exports, creates, edits, and deletes definitions.
6. Constructs a replacement `PlotfileDataset` on Apply.
7. Adds all definitions using strict all-or-nothing error handling.
8. Converts ranges and vector fields from numeric IDs to names.
9. Replaces the live dataset.
10. Rebuilds widget contents and converts names back to numeric IDs.
11. Invalidates views, child windows, and prefetch state.
12. Schedules replacement slices.
13. Restores paused activity when the dialog closes.

The replacement dataset is constructed before assignment, so editor Apply is
already transactional with respect to `m_dataset`. The weakness is that the
transaction is implemented locally inside the Qt dialog workflow.

### Initial and sequence loading

`executeFrameLoad()` constructs a dataset, installs desired expressions,
resolves scalar and vector fields, renders initial slices, and returns an
`InitialSliceResult`.

The current `FrameSliceSpec` is an early form of viewer intent. The current
`InitialSliceResult` is an early form of a prepared viewer snapshot.

Installation is nevertheless duplicated:

- The initial-load completion handler assigns the dataset, resets both
  expression vectors, configures controls, restores an optional specification,
  and installs displays.
- `displayFrameResult()` separately assigns the sequence dataset, updates the
  installed-expression subset, remaps ranges, restores fields, configures
  controls, and installs displays.
- Editor Apply performs another assignment and restoration sequence.

### Asynchronous compatibility

The current counters serve different local purposes:

- `m_generation` rejects stale dataset and sequence-frame loads.
- `m_specGeneration` rejects prefetched frames prepared from obsolete display
  state.
- `m_prefetchGeneration` rejects cancelled prefetches.
- `PlaneViewState::sliceGeneration` rejects stale per-view renders.
- `m_sequenceIndex` confirms that a frame result still belongs to the selected
  frame.

These mechanisms should not all be removed. The missing concept is one
top-level revision that states whether a prepared result matches the logical
viewer intent.

## Proposed Architecture

### The `ViewerSession` module

Add one deep, Qt-independent module:

```text
include/amrexplorer/viewer/ViewerSession.hpp
src/viewer/ViewerSession.cpp
src/viewer/CMakeLists.txt
```

Start with one public header and one implementation file. Do not split every
value type into its own module. Internal helpers should remain private until a
second real caller demonstrates a useful seam.

The module will be built as `amrexplorer_viewer` with alias
`amrexplorer::viewer`. It may depend directly on the existing in-process
`core`, `io`, `query`, and `render2d` modules. It must not depend on Qt.

The module owns:

- Desired expression catalog and expression IDs.
- Persistent scalar and vector field intent.
- Per-field range intent.
- Other dataset-independent viewer intent migrated in later stages.
- The current session revision.
- The currently accepted viewer snapshot.
- Creation of immutable preparation plans.
- Validation of prepared results before acceptance.
- Reconciliation and fallback policies.

The module does not own:

- Qt widgets, dialogs, actions, settings, or translations.
- `QFutureWatcher`, QtConcurrent, timers, or the Qt event loop.
- File-selection dialogs or message boxes.
- Image presentation and scene objects.
- Per-view mouse gesture state.

### Provisional data model

The exact spelling may change during implementation, but the distinctions are
required.

```cpp
struct ExpressionId {
    std::uint64_t value = 0;
    auto operator<=>(const ExpressionId&) const = default;
};

struct ExpressionDefinition {
    ExpressionId id;
    std::string name;
    std::string source;
};

struct NativeFieldKey {
    std::string name;
    auto operator<=>(const NativeFieldKey&) const = default;
};

struct DerivedFieldKey {
    ExpressionId expression;
    auto operator<=>(const DerivedFieldKey&) const = default;
};

using FieldKey = std::variant<NativeFieldKey, DerivedFieldKey>;

struct ViewerRevision {
    std::uint64_t value = 0;
    auto operator<=>(const ViewerRevision&) const = default;
};
```

Expression IDs are session-local identities. The existing version 1 JSON format
continues to store names and sources only:

- Import assigns fresh expression IDs.
- Export does not expose session-local IDs.
- Renaming or reordering an existing definition preserves its ID.

### Intent versus resolution

The session must distinguish what the user requested from what the current
dataset can display.

For example:

```text
requested scalar field: DerivedFieldKey{42}
current frame binding:  unavailable
resolved scalar field:  NativeFieldKey{"density"} -> FieldId{0}
```

The resolved fallback is used for the current display, but the requested key is
retained. If a later frame can install expression 42, it becomes selected again
without reconstructing intent from the fallback widget selection.

The same distinction applies to vector fields and per-field ranges.

### Expression installation status

Every desired expression receives a result in every prepared snapshot:

```cpp
struct InstalledExpression {
    ExpressionId id;
    FieldId field;
};

struct UnavailableExpression {
    ExpressionId id;
    std::string reason;
};

using ExpressionInstallation =
    std::variant<InstalledExpression, UnavailableExpression>;
```

The desired catalog is never replaced with the installed subset. The editor
therefore shows all desired definitions and may annotate definitions that are
unavailable in the current dataset.

### Validation and failure policy

The implementation must distinguish catalog errors from dataset availability.

Catalog errors reject an expression edit transaction:

- Empty or invalid expression name.
- Duplicate derived-field name.
- Empty expression source.
- Parser syntax error independent of dataset availability.
- Reference to a later derived definition.
- Any other violation of the ordered-catalog invariant.

Dataset availability does not delete desired intent:

- A native input does not exist in the current sequence frame.
- An earlier desired expression is unavailable in the current frame.
- A field cannot be installed because the current dataset centering is
  incompatible.

For initial and sequence loads, dataset availability produces an unavailable
status and a warning while other definitions continue to install, preserving
current best-effort behavior.

Editor Apply remains strict for catalog errors. After a valid catalog is
accepted, dataset-specific unavailability is represented explicitly rather
than silently removing the definition.

If fully separating syntax validation from dataset binding proves impossible
with the current parser interface, preparation may classify errors while
installing. The important invariant remains: a catalog edit is either committed
as a whole or not committed, while a committed definition may be unavailable
in a particular dataset snapshot.

### Planning, preparation, and acceptance

The target flow has three conceptual operations:

```text
GUI action
    |
    v
ViewerSession::plan(action)
    |
    | immutable plan + ViewerRevision
    v
prepare(plan, StopToken)       [worker thread, no Qt]
    |
    | prepared snapshot
    v
ViewerSession::accept(snapshot) [GUI thread]
    |
    | accepted or stale
    v
MainWindow::applySnapshot()
```

The exact public interface should remain small. A provisional shape is:

```cpp
class ViewerSession {
public:
    [[nodiscard]] ViewerPlan plan(ViewerAction action);
    [[nodiscard]] AcceptResult accept(PreparedViewerSnapshot snapshot);
    [[nodiscard]] const ViewerSnapshot& snapshot() const;
};

[[nodiscard]] PreparedViewerSnapshot prepareViewerSnapshot(
    ViewerPlan plan, StopToken cancellation);
```

This sketch is intentionally provisional. Implementation should prefer typed
actions and results over a broad collection of setters, but it should not
introduce a generic framework merely to force every minor widget event through
one variant.

Required interface invariants:

1. A plan is immutable and self-contained for worker execution.
2. A prepared snapshot records the revision and source/frame identity of its
   plan.
3. `accept()` changes session state only when the result is compatible with the
   current revision.
4. Rejected or stale results leave the accepted snapshot unchanged.
5. Numeric field IDs cannot escape the snapshot in which they were resolved.
6. A catalog edit either commits completely or leaves the previous catalog and
   snapshot unchanged.

### Worker execution

Qt remains responsible for scheduling:

1. `MainWindow` translates a widget event into a viewer action.
2. The session returns an immutable plan and increments its revision when
   logical intent changes.
3. `MainWindow` launches `prepareViewerSnapshot()` through QtConcurrent.
4. The watcher returns the prepared result to the GUI thread.
5. The session accepts or rejects the result.
6. `MainWindow` projects an accepted snapshot into widgets.

This keeps Qt threading details outside the viewer module while making the
worker computation directly callable from non-Qt tests.

Cooperative cancellation remains an optimization. Correctness comes from
revision validation: a task that ignores or observes cancellation late still
cannot replace newer state.

### Revisions and local generations

Add a `ViewerRevision` for cross-cutting compatibility. Increment it whenever a
change affects prepared dataset or display state, including:

- Dataset source or sequence frame.
- Desired expression catalog.
- Requested scalar or vector fields.
- Level or range intent.
- Logarithmic mode, palette, display mode, or contour count.
- Slice positions or visible regions when included in a preparation plan.

Keep local counters where they provide narrower ordering:

- Per-view slice generations may continue to coalesce and reject obsolete
  interactive renders.
- A prefetch-slot generation may continue to distinguish cancellation of the
  bounded slot.

Every prefetch must additionally carry the viewer revision and frame identity.
A matching local prefetch generation is insufficient if viewer intent changed.

### Qt adapter responsibilities

After the refactor, `MainWindow` remains responsible for:

- Constructing and arranging widgets.
- Connecting Qt signals to viewer actions.
- Opening file and expression dialogs.
- Launching worker operations.
- Applying an accepted snapshot using `QSignalBlocker`.
- Displaying status, warnings, and errors.
- Maintaining scene/view objects and mouse gestures.
- Managing top-level child windows as presentation objects.

It should no longer:

- Own desired or installed expression definition vectors.
- Use editor rows as expression identity.
- Store persistent field ranges by numeric `FieldId`.
- Rebuild datasets inside dialog callbacks.
- Independently decide how expressions are installed for different workflows.
- Independently reconcile fields after each kind of dataset replacement.
- Decide whether an asynchronous dataset result is compatible with current
  viewer intent.

### One Qt snapshot projection

Add one `MainWindow` method responsible for projecting an accepted session
snapshot:

```cpp
void MainWindow::applySnapshot(const ViewerSnapshot& snapshot);
```

This is a Qt adapter method, not the state transition itself. It should:

1. Block relevant widget signals.
2. Rebuild field and level controls from snapshot bindings and metadata.
3. Select the snapshot's resolved scalar and vector fields.
4. Load resolved range controls.
5. Update metadata, menus, diagnostics, and animation information.
6. Install any prepared displays.
7. Clear or close presentation objects tied to the previous snapshot.
8. Unblock signals.

Initial load, editor Apply, sequence load, FAB restoration, and prefetch
consumption must all reach this same method after session acceptance.

### Expression editor after extraction

`showExpressionEditor()` becomes a Qt-only workflow:

1. Ask the session for an expression draft and its base revision.
2. Populate rows keyed by `ExpressionId` stored in `Qt::UserRole`.
3. Edit the draft without touching the accepted session or dataset.
4. On Cancel, discard the draft.
5. On Apply, submit the draft as one session action.
6. If the base revision is stale, reject Apply with a clear message or use an
   explicitly reviewed rebase policy.
7. Schedule the returned plan.
8. Keep the dialog open if catalog validation fails.

The initial migration should preserve the current behavior of pausing playback
and pending debounces while the modal editor is open. Replace the manual save
and restoration flags with one scoped Qt-side activity guard. Whether playback
may safely continue while a draft is open can be reconsidered later.

### CMake integration

Add:

```cmake
add_subdirectory(src/viewer)
```

The new target should be a normal always-enabled internal library:

```cmake
add_library(amrexplorer_viewer ViewerSession.cpp)
add_library(amrexplorer::viewer ALIAS amrexplorer_viewer)
```

It should use the existing warning target and link only the existing modules
required by its implementation. The Qt executable links
`amrexplorer::viewer`.

Do not add a feature switch for the viewer session. It is part of the core
desktop architecture, not an optional feature.

## Testing Strategy

The interface of `ViewerSession` is the primary test seam.

### State-transition tests

Add `tests/unit/test_viewer_session.cpp` with table-driven transitions:

- Add, edit, rename, reorder, delete, and import expressions.
- Preserve expression identity across rename and reorder.
- Delete an expression before or after the selected expression.
- Reject an invalid draft without changing the accepted catalog.
- Preserve a requested derived field when it is unavailable in one frame.
- Restore that field when it is available in a later frame.
- Preserve scalar and vector intent independently.
- Preserve per-field range state across ID compaction.
- Ensure a native and derived field with related names remain distinct keys.
- Reject a prepared result with an obsolete session revision.
- Reject a prepared result for the wrong source or frame.
- Confirm that rejected results leave the current snapshot unchanged.

These tests should use observable session results rather than inspect private
containers.

### Dataset preparation tests

Add integration coverage using existing materialized fixtures:

- Install a chain of valid expressions.
- Report an unavailable expression when a native field is missing.
- Mark dependent later expressions unavailable.
- Bind stable field keys to the correct compacted `FieldId` values.
- Preserve desired definitions across frames with different native schemas.
- Exercise strict catalog validation and best-effort frame availability.
- Verify cancellation and stale completion cannot alter accepted state.

### Qt smoke tests

Retain the current smoke scenarios:

- Expression create, import, export, edit, rename, and delete.
- Expression range behavior.
- Sequence frames with differing field schemas.
- MultiFab/FAB transitions.
- Playback interaction.

Reduce their responsibility over time. Qt tests should verify widget wiring and
snapshot projection; the state-transition matrix belongs in non-Qt tests.

The smoke-test helpers currently embedded in production `main.cpp` may later be
moved to a test-only source or target. That cleanup is useful but not required
to establish the viewer-state seam.

## Migration Plan

Each milestone must leave the application buildable and preserve the existing
test suite. Do not combine all milestones into one rewrite.

### Milestone 0: Characterize current behavior

- Add focused tests for the currently supported expression and sequence
  transitions before moving code.
- Record current strict editor Apply versus best-effort frame-loading behavior.
- Record fallback behavior for unavailable scalar and vector fields.
- Verify the full current CTest suite.

Exit criteria:

- Existing behavior relevant to the refactor is covered at its current seams.
- Any ambiguous behavior is resolved in this document before implementation
  continues.

### Milestone 1: Introduce stable identities

- Add `ExpressionId`, `ExpressionDefinition`, and `FieldKey`.
- Assign session-local IDs when definitions are created or imported.
- Store expression IDs in editor rows.
- Preserve existing JSON input and output.
- Introduce conversion helpers at current `MainWindow` call sites.
- Continue using the existing dataset replacement paths temporarily.

Exit criteria:

- Editor selection no longer depends on definition row position.
- Rename and reorder preserve derived-field identity.
- Existing user-visible behavior remains unchanged.

### Milestone 2: Extract expression catalog and field intent

- Create the initial `ViewerSession` module.
- Move the desired expression catalog into it.
- Move requested scalar/vector field keys and field-range intent into it.
- Represent installed and unavailable definitions as snapshot status.
- Keep `m_dataset` installation in `MainWindow` temporarily.

Exit criteria:

- Remove `m_derivedFields` and `m_installedDerivedFields` from `MainWindow`.
- Remove persistent range storage keyed by numeric field IDs.
- The editor displays the authoritative desired catalog.
- State-transition tests pass without Qt.

### Milestone 3: Unify dataset preparation

- Move dataset construction and expression installation out of
  `showExpressionEditor()`.
- Generalize the useful parts of `executeFrameLoad()` into immutable planning
  and `prepareViewerSnapshot()`.
- Use the same preparation implementation for initial load, editor Apply,
  sequence load, FAB restoration, and prefetch.
- Preserve explicit strict catalog validation and best-effort per-frame
  availability.

Exit criteria:

- No Qt dialog callback constructs a `PlotfileDataset`.
- Exactly one implementation installs an expression catalog into a dataset.
- Every prepared result contains stable bindings and expression statuses.

### Milestone 4: Unify snapshot acceptance and Qt projection

- Move revision validation and accepted-snapshot ownership into
  `ViewerSession`.
- Add `MainWindow::applySnapshot()`.
- Route initial load, editor Apply, sequence frames, FAB transitions, and
  prefetch consumption through session acceptance and this one projection
  method.
- Remove duplicated range, field, and vector remapping blocks.

Exit criteria:

- Exactly one Qt method projects a newly accepted dataset snapshot.
- A stale prepared result cannot mutate session or widget state.
- `MainWindow` no longer owns the authoritative dataset separately from the
  session.

### Milestone 5: Consolidate asynchronous compatibility

- Add the top-level viewer revision to every load and prefetch plan.
- Retain local per-view and prefetch generations only for local ordering.
- Replace editor timer/playback save-and-restore logic with a scoped activity
  guard.
- Verify rapid edits, frame steps, playback, and prefetch completion ordering.

Exit criteria:

- Every asynchronous dataset replacement is guarded by viewer revision and
  source/frame identity.
- Adding a new preparation workflow does not require inventing another
  cross-cutting compatibility counter.

### Milestone 6: Reduce `MainWindow`

- Move any remaining Qt-independent viewer transitions into `ViewerSession`.
- Keep widget construction, Qt event adaptation, scheduling, and projection in
  `MainWindow`.
- Move test-only smoke machinery out of production `main.cpp` if doing so does
  not complicate application startup or CMake portability.
- Update comments and documentation to describe the new ownership model.

Exit criteria:

- `MainWindow` contains no parser/dataset reconciliation policy.
- Qt-independent viewer behavior is testable without constructing a
  `QApplication`.
- The full Qt smoke suite remains as end-to-end wiring coverage.

## Verification Gates

Run at each milestone:

1. Configure and build the default Qt-enabled tree.
2. Run the full CTest suite.
3. Run the expression editor, expression range, MultiFab/FAB, and sequence
   smoke tests explicitly.
4. Run the headless build and tests to ensure the viewer module does not
   accidentally introduce a Qt dependency into lower-level modules.
5. Run sanitizer validation where currently supported.
6. Run `git diff --check`.

Before final acceptance:

- Exercise rapid sequence stepping while prefetch is active.
- Exercise playback before, during, and after expression editing.
- Exercise a sequence in which a selected derived field is unavailable in one
  frame and returns in the next.
- Exercise rename, reorder, delete, import, and Cancel with a derived field
  selected.
- Confirm rejected and stale preparations leave the displayed snapshot
  unchanged.

## Architectural Acceptance Criteria

The refactor is complete when:

1. One `ViewerSession` owns the desired expression catalog, persistent field
   intent, ranges, revision, and accepted viewer snapshot.
2. `MainWindow` does not own parallel desired and installed expression vectors.
3. Persistent viewer state does not use editor rows or dataset-local
   `FieldId`s as identity.
4. All dataset replacement workflows use the same preparation implementation.
5. All successful dataset replacements use the same session acceptance and Qt
   projection paths.
6. Unavailable definitions remain in the desired catalog and have explicit
   per-snapshot status.
7. A stale asynchronous result cannot modify the accepted snapshot or widgets.
8. The primary state-transition matrix runs without Qt.
9. Existing end-to-end Qt smoke behavior remains covered.
10. No parser, rendering, caching, or expression-list format change is bundled
    into the refactor.

## Risks and Mitigations

### Risk: Big-bang state rewrite

Moving all widget and render state simultaneously would create a large,
difficult-to-review patch.

Mitigation: Migrate stable identities, expression state, preparation, and
acceptance in separate milestones. Temporary adapters are acceptable between
milestones but must be deleted by the stated exit criteria.

### Risk: A shallow pass-through module

`ViewerSession` would add indirection without leverage if it merely mirrored
every `MainWindow` member through getters and setters.

Mitigation: The module interface is organized around planning and accepting
state transitions. Reconciliation, fallback, revision validation, and catalog
invariants stay behind the seam.

### Risk: Behavior changes hidden inside refactoring

Expression failures currently use strict editor semantics and best-effort
sequence semantics.

Mitigation: Characterize these policies before extraction and encode them as
explicit tests. Any proposed behavior change requires separate review.

### Risk: Over-generalized event framework

A universal action/effect framework could make simple interactions harder to
follow.

Mitigation: Begin with the dataset/expression lifecycle that motivated the
refactor. Add typed actions only where they replace existing duplicated
transitions.

### Risk: Snapshot lifetime and memory usage

Prepared and accepted snapshots may temporarily retain both old and new
datasets and their caches.

Mitigation: Preserve the current replacement lifetime behavior, release stale
results promptly, and add a focused lifetime/cache test before expanding
snapshot contents.

### Risk: Qt projection emits unintended requests

Rebuilding several controls can emit signals against a partially projected
snapshot.

Mitigation: Centralize projection in `applySnapshot()`, block all relevant
signals for the whole projection, and schedule rendering only after projection
is complete.

## Open Design Questions

These questions should be resolved during review or the milestone that first
depends on them.

1. Should `PreparedViewerSnapshot` include rendered initial displays, as
   `InitialSliceResult` does now, or should preparation and rendering be two
   explicit phases?
   - Recommendation: preserve the combined path initially, then split only if
     tests demonstrate useful leverage.
2. When a requested scalar field is unavailable in one sequence frame, should
   the selector visibly show only the fallback while the session silently
   retains the request?
   - Recommendation: yes, preserve current widget behavior while retaining
     intent in the session.
3. Should unavailable desired expressions be shown in the editor with warning
   decoration?
   - Recommendation: yes, because hiding them conflates desired state with the
     installed projection.
4. Should a stale editor draft be rejected or automatically rebased?
   - Recommendation: reject it initially. Automatic rebase adds conflict
     semantics that are unnecessary for a modal single-user editor.
5. How much non-expression display state should move into `ViewerSession`
   during the first implementation series?
   - Recommendation: move only state needed for coherent dataset replacement;
     migrate the remaining Qt-independent state after the seam is proven.

## Implementation Outcome

All milestones in this plan are complete:

- `ViewerSession` is a Qt-independent module that owns the desired expression
  catalog, accepted dataset snapshot, requested field intent, persistent
  ranges, and viewer revision.
- Expression identity is stable across rename and reorder operations.
- Dataset and expression preparation produce one typed
  `PreparedViewerSnapshot`, with explicit installed and unavailable expression
  outcomes.
- Editor application and sequence-frame loading use the same preparation,
  acceptance, and Qt projection paths.
- Asynchronous viewer compatibility is checked with `ViewerRevision`; local
  counters remain only for request cancellation and supersession inside
  individual views.
- Unavailable desired expressions remain visible in the editor with diagnostic
  decoration, while the installed projection contains only usable fields.
- Qt-free integration tests cover strict and best-effort expression policies,
  stable identities, unavailable dependencies, field-intent restoration,
  persistent ranges, and stale-result rejection.

The combined dataset-preparation and initial-render path was preserved for this
refactor. Splitting rendering into a separate phase remains a possible future
optimization, but is not required to establish the ownership boundary.

Verification completed on 2026-07-24:

- Qt build and full test suite: 32 of 32 tests passed.
- Headless build and full test suite: 18 of 18 tests passed.
- Headless AddressSanitizer and UndefinedBehaviorSanitizer build: 18 of 18
  tests passed with leak detection disabled.
- Headless warnings-as-errors build: 18 of 18 tests passed.
