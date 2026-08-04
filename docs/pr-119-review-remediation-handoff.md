# PR #119 review remediation handoff

Prepared for [PR #119](https://github.com/AMReX-Codes/amrexplorer/pull/119)
after restacking PRs #107 through #119. The review state was re-read after the
updated stack was published: the pull request has six conversation comments,
no submitted reviews, and no inline review threads. This checklist does not
reply to comments or resolve threads.

## Merge blockers and fixed-scale regression

| Finding | Owning PR | Remediation commit | Focused regression |
|---|---|---|---|
| Durable Fit, fixed-scale, and custom transforms; preserve compatible sequence zoom and pan | [#108](https://github.com/AMReX-Codes/amrexplorer/pull/108) | [`97b8dc7`](https://github.com/AMReX-Codes/amrexplorer/commit/97b8dc790d258d1d0cd44dbb708d27feff5bc871) | `display_coordinator`; `qt_fixed_scale_1x_arrival_smoke`; `qt_fixed_scale_4x_arrival_smoke`; `qt_sequence_transform_preserve_smoke`; `qt_sequence_equal_size_transform_preserve_smoke`; `qt_sequence_geometry_refit_smoke` |
| Portable framing tests and SIGPIPE-safe, bounded socket I/O | [#110](https://github.com/AMReX-Codes/amrexplorer/pull/110) | [`159faa9`](https://github.com/AMReX-Codes/amrexplorer/commit/159faa9fa5c10e87e0ceac51cdd54bdb1cf14e21), [`7ad0964`](https://github.com/AMReX-Codes/amrexplorer/commit/7ad096466f1420ebb2ed9b3907999dca4bf222ad), [`40d318a`](https://github.com/AMReX-Codes/amrexplorer/commit/40d318aa1d15782af1d67e79c0f5eab2a3f6ee7a) | `remote_frame`, including torn headers, malformed lengths, partial frames, abortive close, and closed-peer writes; 100-run abortive-close stress loop |
| Server self-deadlock and socket write while holding the session-state mutex | [#113](https://github.com/AMReX-Codes/amrexplorer/pull/113) | [`e775227`](https://github.com/AMReX-Codes/amrexplorer/commit/e775227941f375a7e579e99e854e68d5a84adcb7), [`17a1b93`](https://github.com/AMReX-Codes/amrexplorer/commit/17a1b9332dbc116e6283fb3d28881df951e21066) | `remote_server` duplicate-live-ID, saturation, disconnect, and bounded-shutdown cases; 100-run duplicate-ID stress loop |
| Wrong-payload promise orphan and permanent client hang | [#114](https://github.com/AMReX-Codes/amrexplorer/pull/114) | [`29fb8e2`](https://github.com/AMReX-Codes/amrexplorer/commit/29fb8e23813366068ed1b9ad28ac0f9de15c536b) | `remote_connection` wrong-payload and disconnect fan-out cases |
| Unbounded or uncancellable connect and hello from Qt | [#114](https://github.com/AMReX-Codes/amrexplorer/pull/114), [#116](https://github.com/AMReX-Codes/amrexplorer/pull/116), [#119](https://github.com/AMReX-Codes/amrexplorer/pull/119) | [`29fb8e2`](https://github.com/AMReX-Codes/amrexplorer/commit/29fb8e23813366068ed1b9ad28ac0f9de15c536b), [`be397cb`](https://github.com/AMReX-Codes/amrexplorer/commit/be397cb70d6de58b10cce6f42d1519c7c36d335a), [`d33789c`](https://github.com/AMReX-Codes/amrexplorer/commit/d33789c6f055336c534cf5241c32332af3356a9a) | `remote_connection` handshake deadline and cancellation; `qt_remote_silent_hello_close_smoke`; `qt_remote_silent_verification_close_smoke` |
| Remote sequence retained stale FAB and duplicated sequence state transitions | [#117](https://github.com/AMReX-Codes/amrexplorer/pull/117) | [`c1649b1`](https://github.com/AMReX-Codes/amrexplorer/commit/c1649b12ae325baf3b98264de584f0363082b29c) | `qt_remote_sequence_after_fab_smoke_2d`; `qt_remote_sequence_smoke` |
| Grid-overlay response could exceed the negotiated frame | [#118](https://github.com/AMReX-Codes/amrexplorer/pull/118) | [`82b8aee`](https://github.com/AMReX-Codes/amrexplorer/commit/82b8aee963093b823fe16c0befdc173f90b6a9b2) | `remote_session` 4 KiB frame case; `qt_remote_grid_boxes_smoke`; `slice_query`; `slice_pipeline`; `display_transitions` |
| Recoverable accept failures terminated the server | [#113](https://github.com/AMReX-Codes/amrexplorer/pull/113) | [`e775227`](https://github.com/AMReX-Codes/amrexplorer/commit/e775227941f375a7e579e99e854e68d5a84adcb7) | `remote_server` accept/stop and connection-limit cases |
| The stack tip had not completed the full CI matrix | [#119](https://github.com/AMReX-Codes/amrexplorer/pull/119) | [`d593d01`](https://github.com/AMReX-Codes/amrexplorer/commit/d593d01f298cbb9a88bb41634cb85f5004aedc1d) | [GitHub Actions run 30751555316](https://github.com/AMReX-Codes/amrexplorer/actions/runs/30751555316): GCC, Clang, AppleClang, MSVC, ASan/UBSan, Qt ASan/UBSan, TSan, headless remote, and hygiene checks |

## Completed non-blocking findings

- [#111](https://github.com/AMReX-Codes/amrexplorer/pull/111)
  [`cf0f47f`](https://github.com/AMReX-Codes/amrexplorer/commit/cf0f47fc7ce17f1b25f067d5e7cff57ce7971f51)
  pins every schema field ID and enum value and adds explicit capabilities.
- [#112](https://github.com/AMReX-Codes/amrexplorer/pull/112)
  [`762f431`](https://github.com/AMReX-Codes/amrexplorer/commit/762f431149779e6ad400b7eabc83744d47d226e4)
  pins native/wire enum mappings and expands hostile codec validation;
  `remote_codec` covers the negative cases.
- [#113](https://github.com/AMReX-Codes/amrexplorer/pull/113)
  [`e775227`](https://github.com/AMReX-Codes/amrexplorer/commit/e775227941f375a7e579e99e854e68d5a84adcb7)
  reserves dataset capacity before loading, closes stop/open races, bounds
  complete encoded responses, and makes accept/stop lifecycle transitions
  deterministic.
- [#114](https://github.com/AMReX-Codes/amrexplorer/pull/114) and
  [#115](https://github.com/AMReX-Codes/amrexplorer/pull/115),
  [`29fb8e2`](https://github.com/AMReX-Codes/amrexplorer/commit/29fb8e23813366068ed1b9ad28ac0f9de15c536b)
  and [`2813b70`](https://github.com/AMReX-Codes/amrexplorer/commit/2813b70efed39a324ff3985a779095bc0e9ad4d6),
  add bounded transactions, cancellation cleanup, serialized close, shared
  local/remote validation, asynchronous best-effort dataset close, and
  value-level equivalence coverage in `remote_connection` and `remote_session`.
- [#116](https://github.com/AMReX-Codes/amrexplorer/pull/116)
  [`be397cb`](https://github.com/AMReX-Codes/amrexplorer/commit/be397cb70d6de58b10cce6f42d1519c7c36d335a)
  adds strict bracketed IPv6 parsing, viewport-aware initial planning, and
  coordinate-aware spherical output sizing. `remote_endpoint` and the remote
  geometry smokes cover these paths.
- [#117](https://github.com/AMReX-Codes/amrexplorer/pull/117) and
  [#118](https://github.com/AMReX-Codes/amrexplorer/pull/118),
  [`c1649b1`](https://github.com/AMReX-Codes/amrexplorer/commit/c1649b12ae325baf3b98264de584f0363082b29c)
  and [`82b8aee`](https://github.com/AMReX-Codes/amrexplorer/commit/82b8aee963093b823fe16c0befdc173f90b6a9b2),
  invalidate reconnect-sensitive range state, avoid duplicate auxiliary box
  transfers, filter degenerate boxes, and expose explicit overlay truncation.
- [#119](https://github.com/AMReX-Codes/amrexplorer/pull/119)
  [`d33789c`](https://github.com/AMReX-Codes/amrexplorer/commit/d33789c6f055336c534cf5241c32332af3356a9a)
  prefers an installed FlatBuffers package with a pinned fallback, rejects
  `--max-datasets 0`, documents transform behavior, and corrects PR #109's
  viewport-bounded line-decimation description.
- The mandatory random session token and dynamic-port workflow remain in
  [#119](https://github.com/AMReX-Codes/amrexplorer/pull/119), including the
  rule that tokens are never persisted.

## Intentionally deferred conversation items

- Unix-domain-socket transport remains an optional future transport. The
  mandatory unpredictable session token closes the reviewed cross-user
  authorization gap for the TCP workflow retained by this stack.
- Evaluating or replacing the implementation with Cap'n Proto RPC is a
  separate architecture decision, not a remediation of the accepted stack.
- Posting review replies or resolving comments remains deferred until it is
  explicitly requested. There are currently no inline threads to resolve.
