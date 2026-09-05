# MCLA-R SDK fork: agent context

This checkout is the runtime/code-generation SDK used by the parent MCLA-R
repository. Parent `AGENTS.md` and `README-AI` own milestone, acceptance,
privacy, exact-pin and release decisions. Read `docs/rexglue-sdk.md` in the
parent for current and historical deltas. Never update or push this fork
independently of those gates.

The current v0.18.0.1 retains v0.18.0.0 descriptor reuse and corrects its two
short-call timers: accumulate nanoseconds internally, convert once on snapshot
read to the unchanged public microsecond fields. Per-call us truncation made
millions of native calls appear almost free; first native control counts/reuse
remain valid, but its timing totals are underestimates. No rendering/cache
policy change. Independent follow-up review and both full configurations pass
1,845 executed plus four skips (1,849 registered). Five diagnostic-scope cases /
53 assertions include an independently timed enclosed-workload unit check and
early return/unwind. Timer resolution/overhead still depends on the platform.

The preceding v0.18.0.0 delta reuses the immediately retained Vulkan vertex/pixel
texture descriptor sets only when actual ordered image views/layouts/samplers,
current set/validity, pipeline/stage layout, frame and submission match.
Texture/sampler requests, uploads, barriers and usage marks remain unconditional.
Layout/external/frame/submission/pool boundaries invalidate; misses allocate
fresh sets and snapshots commit only after all stage writes succeed. An unused
layout-compatible-prefix calculation is replaced by conservative full rebind.
No update-after-bind or cross-frame resource cache is introduced.

Six public snapshot counters measure completed UpdateBindings inclusive calls/
time, successfully reused/written texture STAGE sets, and actual native
vkUpdateDescriptorSets calls/time (including constants). They are relaxed,
overlapping totals, not a transactional or exclusive budget. This public
surface advances the CMake compatibility floor to 0.18.0. No addresses/logging
are added to normal per-draw work. Native/Deck reuse rate and benefit remain
integration gates, not consequences of source review or helper tests.

Independent review, AST scan/one fixture and both all-target configurations
pass: 1,843 executed plus four known BitStream skips (1,847 registered).
Five new helper cases / 37 assertions cover ordered fields, counts, nulls,
epochs, invalidation, partial-stage publication and scratch deep-copy.
They do not inject actual Vulkan allocation/eviction failures or execute the
renderer upload/barrier paths; their integration is source-reviewed.

The preceding v0.17.1.0 delta emits one `simde_mm_pause()` for each exact decoded
`db16cyc` (0x7ffffb78). This restores a host spin hint to the sampled guest
poll path without Sleep, OS yields, changed guest registers/memory, synchronization
primitives or game-clock policy. It is NOT exact 16-cycle emulation. SIMDe
uses PAUSE on x86; other host architectures differ and are not physically tested.
The neighboring record-form OR and ordinary OR remain unchanged. Consumers
must regenerate guest code with this SDK; relinking old generated code alone
does not apply the fix. Never hand-edit generated guest sources.

Independent source review, AST scan/1 fixture and both full all-target builds
pass 1,838 executed tests plus four known BitStream skips (1,842 registered).
Three new PPC cases cover CR/XER/r31/memory preservation, bounded looping and
neighbor OR semantics. The generated-stream check was red before the fix
(zero of nine required hints), then green. No throughput gain is established
by those tests: host spin latency and SMT benefit require physical evidence.

The preceding v0.17.0.0 delta adds seven Vulkan resolve diagnostic snapshot fields:
completed IssueCopy count/inclusive time, actual resolve-to-guest memcpy count,
bytes/time, and four-path explicit readback wait count/time. Disabled/no-op and
failed IssueCopy calls are counted; memexport waits are excluded. Copy time
includes handled write faults but excludes mapped-memory invalidation. These
relaxed cumulative totals overlap/nest and are not a transactional frame budget.
No readback/coherency policy, cache threshold, scheduling or timing is changed.
Accepted Deck comparison: 12.14 outputs/s gameplay and 10.02 in pause, with
zero resolve readback waits during pause. Resolve memcpy alone does not
explain the overall slowdown. The sampled guest polling path and redundant
Vulkan descriptor updates are separate leads; the latter is addressed by
current v0.18.0.0, not by v0.17.0.0.

Review and both full all-target configurations pass 1,834 tests plus four known
BitStream skips (1,838 registered). Seven saved-game fixtures additionally pass
five repetitions in each configuration. The first parallel run exposed a
pre-existing test-only temporary-directory collision: separate test processes
could delete each other's fixture files. TempContentRoot now atomically reserves
a random candidate, never removes an unsuccessful candidate and cleans only
its owned directory. No production save handling or owner profile changed.
The graphics fixture covers initialized counters and RAII early-return/unwind/
nested timing, not actual Vulkan branch counts; both backend builds compile.

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
