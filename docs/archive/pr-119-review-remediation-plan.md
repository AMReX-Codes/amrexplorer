# PR #119 review remediation plan

Status: completed and validated on 2026-08-01.

This plan addresses the review feedback posted on
[AMReX-Codes/amrexplorer#119](https://github.com/AMReX-Codes/amrexplorer/pull/119).
Although the comments were posted on the top pull request, the main review
covers the complete client/server stack, PRs #107 through #119. Fixes should be
made on the earliest branch that owns the affected behavior and then propagated
upward through the stack. They should not be accumulated as unrelated changes
on `remote-packaging`.

## Goals

- Remove the confirmed deadlock, permanent-hang, process-termination, and
  unbounded-response paths before the remote protocol is merged.
- Preserve the dependency-ordered structure of the existing pull-request stack.
- Make protocol 1.0 evolvable before its schema and enum values become public.
- Give connection establishment, request execution, and shutdown explicit
  bounds and cancellation semantics.
- Preserve compatible user-controlled view transforms across asynchronous
  raster replacement and sequence frame changes.
- Add deterministic regressions for the hostile-input and lifecycle paths
  identified by the review.

## Non-goals

- Directly exposing the server on a non-loopback interface.
- Adding TLS, application-level compression, remote rendering, or remote
  filesystem browsing.
- Replacing the shared `DatasetSession` boundary or moving rendering to the
  server.
- Replying to review comments or resolving GitHub threads as part of the code
  work. Those actions require a separate explicit request.

## Current baseline

The following feedback is already addressed on the current PR head and should
be retained while the stack is rewritten:

- The server generates a mandatory per-session token, prints it at startup,
  and requires it in the hello exchange.
- The documented workflow uses an operating-system-selected server port and
  describes how to forward that port over SSH.
- The current GCC, Clang, AppleClang, MSVC, sanitizer, TSan, and headless remote
  CI jobs have completed successfully.

The source-level `std::jthread` portability concern remains worth fixing even
though the current AppleClang job is green. The remaining major findings and
the fixed-scale regression are also still present.

## Stack ownership

| Step | Pull request | Branch | Primary responsibility |
|---|---:|---|---|
| 1 | #108 | `remote-display-refit` | Durable view-transform policy |
| 2 | #110 | `remote-framing` | Framing and socket portability |
| 3 | #111–#112 | `remote-codec`, `remote-codec-impl` | Schema and codec safety |
| 4 | #113 | `remote-server` | Server locking and lifecycle |
| 5 | #114–#115 | `remote-connection`, `remote-dataset-session` | Client lifecycle and session cleanup |
| 6 | #116 | `remote-qt-dataset` | Qt connection and viewport integration |
| 7 | #117 | `remote-sequences` | Remote sequence ownership |
| 8 | #118 | `remote-grid-overlays` | Bounded grid-overlay responses |
| 9 | #119 | `remote-packaging` | Packaging, CLI validation, and documentation |
| 10 | Entire stack | all branches | Restacking and final validation |

## 1. Make display transforms durable

Owning PR: #108, `remote-display-refit`.

### Decision

Preserve wheel zoom and drag-pan across a sequence frame change when the old and
new rasters have compatible physical geometry. A fresh `DatasetId` alone is not
evidence that the transform is incompatible. Refit only when the physical
domain, coordinate system, displayed orientation, or raster-to-data mapping
changes incompatibly.

Fit, fixed scale, and custom zoom are persistent modes rather than incidental
properties of the current `QTransform`:

- Fit mode refits on compatible image replacement and future viewport resizes.
- Fixed-scale mode reapplies the selected integer factor after any raster
  replacement and does not revert to Fit when a resize-triggered slice arrives.
- Custom mode preserves the visible data window when pixel density changes.

### Changes

- Add an explicit transform mode to `ImageView`, including the fixed-scale
  factor when applicable.
- Make `setImage` honor that mode instead of allowing `GeometryAware` or
  `Refit` to overwrite a fixed scale selected immediately before a delayed
  slice arrival.
- Replace dataset-identity-only compatibility checks with a comparison of the
  physical domain, coordinate system, normal direction, and applicable raster
  geometry.
- Keep the existing data-window remapping for capped-to-native-resolution
  rubber-band results.
- Update the stale `DisplayCoordinator.cpp` comment to describe the final
  compatibility rule.

### Tests

- Open a small plotfile, select `1x`, force the viewport resize/re-slice path,
  and verify the final transform remains exactly `1x`.
- Repeat at `4x` and verify both the transform and toolbar/menu state.
- Step between same-geometry sequence frames after wheel zoom and drag-pan;
  verify the visible physical window is preserved.
- Step to a frame with incompatible geometry and verify the view refits.
- Retain the rubber-band over-zoom and synchronized-panel regressions.

### Acceptance criteria

- The Scale control and the visible transform cannot disagree after an
  asynchronous raster arrival.
- Compatible sequence frames preserve wheel zoom and pan.
- Incompatible datasets still refit deterministically.

## 2. Harden framing and socket behavior

Owning PR: #110, `remote-framing`.

### Changes

- Replace both test-only `std::jthread` instances in
  `test_remote_frame.cpp` with explicitly joined `std::thread` instances.
- Suppress broken-pipe process termination on macOS by applying
  `SO_NOSIGPIPE` to connected sockets. Retain `MSG_NOSIGNAL` on platforms that
  provide it.
- Set `TCP_NODELAY` for the framed request/response connection.
- Use `SO_EXCLUSIVEADDRUSE` rather than `SO_REUSEADDR` for the Windows listener.
- Report an explicit address-resolution failure if `getaddrinfo` succeeds but
  returns no usable addresses.
- Keep all socket option setup in one helper used consistently by accepted and
  outbound sockets.

### Tests

- Add torn-header, short-header EOF, zero-length frame, oversized-frame
  injection, and short-payload tests.
- Write to a peer that has closed its socket and verify `writeFrame` throws
  rather than terminating the test process.
- Keep the tests portable across Linux, macOS, and Windows.

### Acceptance criteria

- A dropped SSH tunnel cannot terminate either client or server via SIGPIPE.
- Malformed framing is rejected before unbounded allocation.
- The framing unit test builds on the supported AppleClang/libc++ baseline.

## 3. Freeze protocol 1.0 safely

Owning PRs: #111–#112, `remote-codec` and `remote-codec-impl`.

### Changes

- Give every FlatBuffers table field an explicit `(id:)` and every enum member
  an explicit numeric value. Preserve the current wire layout when assigning
  those values.
- Add capabilities to both hello messages, even if the initial capability set
  is empty.
- Keep the session-token field in the hello request and assign it an explicit
  field ID.
- Add compile-time assertions that pin every native `PayloadKind` and
  `ErrorCode` value to its FlatBuffers counterpart.
- Reject negative `finest_level`, checked-overflow failures in level-count
  arithmetic, invalid level ranges when the field catalog is empty, bad enums,
  non-finite numeric inputs, truncated vectors, and request ID zero.
- Make all native/wire conversions validate first and construct second so a
  rejected message cannot leave partially trusted state.

### Tests

- Round-trip every payload and error enum value.
- Add one focused negative test for each validation family.
- Add a schema-compatibility fixture that decodes bytes produced by the
  pre-capabilities layout where protocol compatibility permits it.

### Acceptance criteria

- Adding a future optional field cannot silently renumber an existing field.
- Native and wire enums cannot drift without a compile failure.
- Every decoder rejection path named by the review is covered.

## 4. Repair server locking and lifecycle

Owning PR: #113, `remote-server`.

### Locking changes

- Restrict `m_stateMutex` scopes to state inspection and mutation.
- In `dispatch`, decide whether a request is accepted, over limit, or a
  duplicate while holding the mutex, then release the mutex before calling
  `sendError` or `stop`.
- Audit every other `send`, `sendError`, `stop`, dataset close, and blocking
  operation to ensure none occurs while `m_stateMutex` is held.
- Preserve `m_writeMutex` as the sole serializer for socket writes.

### Dataset lifecycle changes

- Reserve a dataset slot under the state mutex before constructing a
  `LocalDatasetSession`.
- Construct outside the mutex, then publish the dataset only if the request is
  not cancelled and the session is not stopping.
- Release the reservation and close the constructed dataset on every failure,
  cancellation, or shutdown path.
- Prevent a completed open from registering a dataset into a stopped session.

### Accept-loop changes

- Check shutdown state again when registering a newly accepted session so
  `requestStop` cannot miss it.
- Retry transient accept failures such as `EINTR` and `ECONNABORTED`.
- Handle descriptor/resource exhaustion without allowing an exception to
  escape a bare `std::thread`; use bounded backoff and an observable server
  error path.
- Add a configured maximum connection count so the thread-per-connection model
  cannot consume descriptors without a limit.

### Response-bound changes

- Include the FlatBuffers envelope and vector overhead in response-size
  validation.
- Apply a preflight bound to dataset-opened, slice, line, particle, range, and
  cache responses.
- Return a typed `ResourceLimitExceeded` response whenever the error itself can
  still fit; do not discover ordinary size violations only inside `writeFrame`.

### Tests

- Duplicate a live request ID and require a bounded error/disconnect rather
  than a server deadlock.
- Fill all outstanding-request slots, stop reading, and verify `requestStop`
  and destruction complete within a deadline.
- Cover cancellation, concurrent requests, abrupt disconnect, stop-during-open,
  and accept/stop races.
- Exercise the connection and dataset limits.

### Acceptance criteria

- No blocking socket operation or recursive stop path executes under
  `m_stateMutex`.
- All shutdown paths complete within their test deadlines.
- Client-controlled work has explicit connection, request, dataset, and frame
  bounds.

## 5. Eliminate client hangs and leaked handles

Owning PRs: #114–#115, `remote-connection` and
`remote-dataset-session`.

### Connection changes

- Validate the response payload kind before removing its entry from
  `m_pending`. On mismatch, complete that promise exceptionally and then fail
  the remaining pending requests.
- Add a configurable connection/handshake deadline to `ConnectionOptions`.
- Thread a `StopToken` through address connection and the hello transaction.
  Implement connect as an interruptible, deadline-bounded operation rather
  than a permanently blocking constructor step.
- Define the cancellation race so a response that is already complete wins;
  do not discard an opened dataset handle after the server has allocated it.
- Exclude cancellation acknowledgements from the ordinary outstanding-request
  budget.
- Acquire the send mutex before closing the socket to prevent descriptor reuse
  racing an in-flight send.
- Remove `noexcept` from `connected()` because it acquires a mutex.
- Generate ping nonces without consuming an additional request ID.

### Remote session changes

- Move dataset-close RPCs off the GUI thread.
- Keep a session logically open until close is acknowledged, or transfer the
  handle to connection-owned best-effort cleanup state if shutdown prevents an
  acknowledgement.
- Share field/level/range validation with the local session so local and remote
  calls fail consistently before I/O.
- Return whether the resulting cache state is within budget rather than testing
  the tautological stored-budget equality.

### Tests

- Return a wrong non-error payload and verify the waiting caller receives an
  exception within a deadline.
- Use a loopback peer that accepts TCP but never sends hello; verify timeout,
  explicit cancellation, and window-close cancellation.
- Test disconnect fan-out with multiple pending requests.
- Race cancellation with a successful dataset open and verify no server handle
  leaks.
- Verify close failure and reconnect cleanup behavior.
- Add the RFC's value-level local/remote equivalence harness for slices, lines,
  ranges, and representative metadata rather than checking only shapes.

### Acceptance criteria

- Every transaction completes with a value or exception; no promise can become
  unreachable while its caller waits.
- Connect, hello, close, and shutdown have bounded completion behavior.
- Dataset handles are not leaked by cancellation or swallowed close failures.

## 6. Close Qt integration gaps

Owning PR: #116, `remote-qt-dataset`.

### Changes

- Accept bracketed IPv6 endpoints and reject ambiguous bare IPv6 instead of
  interpreting it as a malformed host/port pair.
- Pass the connection deadline and cancellation token through connect-time
  token verification and initial dataset opens.
- Cancel verification workers during application shutdown and ensure they do
  not make `waitForDone` unbounded.
- If the viewport changes while the initial request is in flight, remember the
  newest requested size and issue exactly one follow-up request after the
  current result settles.
- Do not plan initial 3-D requests from hidden views before layout. Plan the
  visible view first and defer other panels until they have valid viewport
  sizes.
- Guard `m_openMetadata` before the remote `sliceOutputSize` dereference.
- Define a normalized aspect-sizing policy for spherical `(r, theta)` data so
  heterogeneous units cannot collapse an axis to the one-pixel clamp.

### Tests

- Unit-test IPv4, hostname, bracketed IPv6, token-bearing, missing-port, and
  ambiguous endpoint forms.
- Resize during initial remote load and verify the final raster uses the latest
  viewport.
- Close the window during connect verification and during a silent hello.
- Open 3-D and spherical remote fixtures and verify bounded, nondegenerate
  initial requests.

### Acceptance criteria

- No Qt task can make application shutdown wait for an operating-system network
  timeout or an indefinitely silent peer.
- The first stable raster reflects the latest laid-out viewport geometry.
- Invalid endpoints fail before starting background work.

## 7. Fix remote sequence ownership

Owning PR: #117, `remote-sequences`.

### Changes

- After validating the remote frame list, call the same FAB-state reset used by
  the local sequence path before starting the sequence.
- Prefer a shared sequence-preparation helper for the common playback, range,
  particle, FAB, animation-panel, and line-window state transitions. Keep
  connection creation and remote loading in the remote-specific path.
- Scope range-cache entries by connection generation as well as dataset ID, or
  clear all dataset-scoped display caches whenever a remote connection is
  replaced.
- Apply the asynchronous/best-effort close ownership from step 5 so frame
  changes do not perform a timeout-less round trip on the GUI thread.

### Tests

- Extend `qt_sequence_after_fab_smoke` to cover a remote sequence and verify the
  FAB suffix, dock, and stale selectors are cleared.
- Reconnect to a server whose dataset IDs restart at one and verify Visible
  range data cannot be reused from the prior connection.
- Step quickly through frames while closes are pending and verify playback and
  shutdown remain bounded.

### Acceptance criteria

- Local and remote sequences establish the same MainWindow state invariants.
- Reused server-local IDs cannot alias client display caches.
- Sequence stepping performs no blocking network work on the GUI thread.

## 8. Bound grid-overlay responses

Owning PR: #118, `remote-grid-overlays`.

### Changes

- Add an explicit overlay count/byte budget derived from the negotiated frame
  size after reserving space for raster data, metrics, the envelope, and
  FlatBuffers overhead.
- Pass the maximum overlay count into slice planning so `SliceQuery` stops
  collecting boxes once the limit is reached; do not build an unbounded vector
  and truncate it only during serialization.
- Add `grid_boxes_truncated` to `SliceViewResponse` using the explicit field-ID
  discipline from step 3.
- Filter clipped boxes whose lower bound is not strictly below the upper bound
  on both displayed axes.
- Request grid boxes only on the primary scalar slice. Contour and vector
  auxiliary slices should set `includeGridBoxes` to false.
- Preserve the raster response when overlays are truncated; do not tear down
  the connection for an optional visualization layer.

### Tests

- Use a many-box fixture with a near-maximum viewport and verify the encoded
  response stays below the negotiated frame limit.
- Verify truncation is reported and the raster still displays.
- Verify halo-only/degenerate boxes are not transmitted.
- Verify contour and vector modes transfer the box list only once.

### Acceptance criteria

- Grid overlays cannot make a client-controlled response unbounded.
- Optional overlay truncation does not fail the underlying slice.
- Every transmitted box has a nonempty visible intersection.

## 9. Finish packaging and documentation

Owning PR: #119, `remote-packaging`.

### Changes

- Search for an installed FlatBuffers CMake package first and retain the pinned
  FetchContent dependency as the fallback.
- Reject `--max-datasets 0` with a clear CLI error.
- Retain the mandatory-token and dynamic-port workflow, including the rule that
  tokens are never persisted.
- Document the compatible-geometry sequence transform decision from step 1.
- Correct PR #109's review-facing description to acknowledge viewport-bounded
  line decimation instead of claiming there is no behavior change.
- Update the architecture plan's completion status only after the shutdown,
  bounded-response, protocol, and equivalence-test requirements are satisfied.
- Ensure installation checks continue to cover both the GUI application and
  `amrexplorer-server`.

### Tests

- Configure once with an installed FlatBuffers package and once through the
  pinned fallback.
- Exercise `amrexplorer-server --help`, `--max-datasets 0`, and a valid dynamic
  port startup.
- Run the macOS app-bundle and server installation check.

### Acceptance criteria

- Offline/package-managed builds can avoid FetchContent when FlatBuffers is
  already installed.
- Invalid zero-resource configurations fail at argument parsing.
- User and architecture documentation match the implemented security,
  transform, and bounded-response behavior.

## 10. Restack, validate, and close the review loop

### Restacking procedure

1. Make each change on its owning branch, starting with
   `remote-display-refit` and proceeding upward.
2. Rebase each dependent branch after its parent changes, resolving conflicts
   semantically rather than copying the stack-tip version wholesale.
3. Keep security-token commits and documentation already present on
   `remote-packaging` while replaying them over the revised protocol branches.
4. Audit the final base chain with `gh stack view`.
5. Publish the updated same-repository stack with `gh stack submit` only after
   local validation passes.

### Focused validation

Run the closest tests on each owning branch before advancing:

- Display: coordinator unit tests and fixed-scale/sequence Qt smokes.
- Framing: `remote_frame` plus socket-disconnect tests.
- Protocol: `remote_codec` and schema compatibility tests.
- Server: duplicate-ID, saturation, cancellation, accept/stop, and response
  bound tests.
- Client/session: payload mismatch, timeout, disconnect fan-out, cleanup, and
  local/remote value equivalence.
- Qt/sequences/overlays: their focused smoke and integration tests.

### Stack-tip validation

Run the complete default build and test suite:

```bash
cmake --build build -j4
ctest --test-dir build --output-on-failure --parallel 4
```

Run a fresh warnings-as-errors remote build:

```bash
cmake --preset remote -DAMREXPLORER_WARNINGS_AS_ERRORS=ON
cmake --build --preset remote --parallel 4
ctest --preset remote --parallel 4
```

Also run the ASan/UBSan and TSan presets, the macOS install check, and the full
GitHub matrix. Tests for former hang paths must carry explicit deadlines so a
regression fails rather than stalling CI.

### Review handoff

After the updated stack is published:

- Re-read PR #119's conversation and current review-thread state.
- Prepare a finding-by-finding checklist linking each item to its owning PR,
  commit, and regression test.
- Separate intentionally deferred non-blocking work from completed merge
  blockers.
- Do not post replies or resolve threads until explicitly requested.

## Completion criteria

The remediation is complete when:

- All nine major findings and the fixed-scale regression have focused tests and
  validated fixes.
- The session token, dynamic-port workflow, and complete CI matrix remain
  intact.
- Protocol fields and enum values are explicitly pinned.
- Connection establishment, requests, responses, session cleanup, and shutdown
  are bounded and cancellable where applicable.
- Local and remote value-level results agree on the equivalence fixtures.
- The default, remote, sanitizer, TSan, macOS install, and GitHub CI validations
  are green on the final stack tip.

## Completion record

- The 13-PR dependency chain was audited and published as GitHub stack #120.
- The default build passed all 90 tests, and a fresh warnings-as-errors remote
  build passed all 43 tests.
- Clean configurations passed with both an installed FlatBuffers package and
  the pinned FetchContent fallback.
- Headless ASan/UBSan and TSan passed 33 tests each; Qt ASan/UBSan passed all
  90 tests. Local macOS ASan runs disabled unsupported leak detection while
  retaining fail-fast address and undefined-behavior instrumentation.
- The macOS install check produced an executable `amrexplorer.app` and
  `bin/amrexplorer-server` in a clean prefix.
- [GitHub Actions run 30751555316](https://github.com/AMReX-Codes/amrexplorer/actions/runs/30751555316)
  passed GCC, Clang, AppleClang, MSVC, ASan/UBSan, Qt ASan/UBSan, TSan,
  headless remote, codespell, tabs, and trailing-whitespace checks.
- The finding-by-finding review handoff is recorded in
  [pr-119-review-remediation-handoff.md](pr-119-review-remediation-handoff.md).
