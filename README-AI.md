# MCLA-R SDK fork: agent context

This checkout is the runtime/code-generation SDK used by the parent MCLA-R
repository. Parent `AGENTS.md` and `README-AI` own milestone, acceptance,
privacy, exact-pin and release decisions. Read `docs/rexglue-sdk.md` in the
parent for current and historical deltas. Never update or push this fork
independently of those gates.

The v0.16.1.0 delta repairs the CPU primitive-conversion cache's half-open
invalidation overlap, cancels insertion after overlapping invalidation during
conversion, and bounds its unwatch reply to the inspected buckets even on a
no-hit callback. It restores the 4096-index cache threshold instead of caching
every nonempty conversion. Small inputs are processed fresh; SharedMemory GPU
uploads, GPU watches, guest rights and timing remain unchanged. The threshold
is a Deck throughput candidate, not a measured title-specific optimization yet.

The pre-fix CPU-only fixture failed 6/9 cases. Final coverage is 12 cases / 800
assertions using real Process and Memory paths with CPU-backed index outputs:
threshold 4095/4096/zero, mutation, exact/containing/partial/adjacent ranges,
non-exact invalidation, no-hit unwatch bounds, real A/C/E fault/rearm, per-frame
clear, invalidation during conversion, disjoint active-range retention, and a
two-bucket entry. Both complete all-target RelWithDebInfo and Release suites
pass 1,834 tests plus four known BitStream skips (1,838 registered). Independent
review found and corrected a contradictory crossing-fixture assertion before
final gates. No production blockers remain. Coverage does not establish atomic
current-draw reads during concurrent guest writes or exhaust 32-bit/multi-entry
list behavior; deterministic reentrancy is not a concurrency stress test.

The v0.16.0.0 delta splits Windows physical RAM into one 512 MiB SEC_COMMIT
section while virtual guest backing stays SEC_RESERVE. All five physical
views retain their offsets, independent protections, and existing watches.
The already-present full physical precommit means no increase in final
committed capacity. POSIX retains the old backing layout. This avoids the
reserved-range wineserver query in Proton 9.0-4. Returned Deck arm/disarm means
fell from 64-71 us to about 17 us, but 12.13 guest outputs/s is still not usable
performance. Routes differ, so this is not a controlled A/B FPS claim.
Mapping retry clears unmapped pointers and stops
on cleanup failure. Five native regression cases / 79 assertions pass;
both full all-target configurations pass 1,822 tests plus four known skips
(1,826 registered). Review found no blockers. Actual allocation/unmap API
failure injection is not covered; partial-address collision cleanup is.

The preceding v0.15.0.0 delta adds 21 address-free command/memory diagnostic counters:
inclusive completed primary/draw/swap/interrupt scopes, actual Vulkan fence
calls, physical-watch protection calls/lock waits, and handled write-fault
lock/query/callback phases. It changes no protection, scheduling or GPU
commands. Pre-tag dual-backend RelWithDebInfo compile and CTest pass:
1,817 executed, four known skips, zero failures (1,821 registered), including
eight new memory/graphics cases. Physical Deck throughput/audio remain
blocked; the prior empty-overlay correction fixed reported presentation FPS.

## Architecture and safety

- `src/system` and `include/rex/system`: memory, kernel/runtime services.
- `src/graphics`: shared command processor and D3D12/Vulkan backends.
- `src/ui`: SDL windows, presenter, ImGui drawer/dialog lifecycle.
- `tests/unit`: Catch2 unit cases; the full CTest suite also covers PPC and
  code-generation/version contracts.
- `thirdparty`: separately pinned upstream dependencies; do not modify them
  as part of an application fix. Do not edit generated parent guest code.
- UI mutation/destruction belongs to the UI thread. Hidden persistent ImGui
  dialogs must detach and explicitly wake for notifications; empty `OnDraw`
  alone still drives presenter repaints. `RequestVisibility` cancels queued
  work after destruction, but callers must retain dialog lifetime while
  invoking it. It is not concurrent destruction permission.
- Performance counters are relaxed atomic cumulative samples, not coherent
  frame transactions. Command-wait episodes can straddle F10 boundaries.
  Memory fault time aggregates threads and excludes OS exception delivery;
  never add it to, or subtract it from, graphics frame-wall measurements.
  Primary and packet scopes include nested backend work; fence timing measures
  native calls only (including polls/errors), not cleanup. Memory fault phases
  share the successful-write denominator. Watch arm/disarm timings span all
  callers/aliases; arming lock wait is separate. Safe-gap counts only observe
  fragmentation, never coalesce protection or alter callbacks. Snapshot reads
  may straddle completion; use same-process intervals, not exact stage sums.
- Do not disable protection/coherency or change scheduling based only on
  CPU/GPU utilization. Preserve saves, raw dumps, maps and logs privately.

## Windows workflow

From the parent invoke `scripts/Resolve-Toolchain.ps1 -ExportPath`. Use Clang,
the Visual Studio C++ SDK, and the existing CMake multi-config build tree:

```
ast-grep scan --config third_party/rexglue-sdk/sgconfig.yml
ast-grep test --config third_party/rexglue-sdk/sgconfig.yml
cmake --build third_party/rexglue-sdk/out/build/win-amd64 --config RelWithDebInfo --parallel 8
ctest --test-dir third_party/rexglue-sdk/out/build/win-amd64 -C RelWithDebInfo --output-on-failure
cmake --build third_party/rexglue-sdk/out/build/win-amd64 --config Release --target install --parallel 8
```

The scoped AST rule guards unbounded copying in dialog/diagnostic paths.
Expand rules and fixtures with new architectural constraints; reusable rules
belong in `utils/` once needed. Parent AST gates remain required as well.
Tests under `[ui][dialog]` need no native window, SDL initialization or GPU.
The four known skipped BitStream cases are not passing physical-game tests.
Build all targets before a full CTest run: `--target unit_tests` alone leaves
the separate PPC runner stale after runtime/header ABI changes. For this
delta, the initial Release PPC run failed against a stale runner; rebuilding
all consumers fixed it and both complete suites were rerun successfully.

Review and test code before a four-part version/tag change. Any public SDK
surface change advances the CMake major/minor floor; the immutable four-part
tag determines installed version. After local commit/tag, rebuild/install
matching Release libraries, pin the exact SHA in parent consumers and run
their gates. Do not push until parent milestone authorization/gates permit.
