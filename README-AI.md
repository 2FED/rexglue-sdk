# MCLA-R SDK fork: agent context

This checkout is the runtime/code-generation SDK used by the parent MCLA-R
repository. Parent `AGENTS.md` and `README-AI` own milestone, acceptance,
privacy, exact-pin and release decisions. Read `docs/rexglue-sdk.md` in the
parent for current and historical deltas. Never update or push this fork
independently of those gates.

The v0.14.0.0 delta detaches empty persistent overlays and exposes four
completed-wait/non-MMIO-write-fault counters. Pre-tag dual-backend
RelWithDebInfo build/install and CTest pass: 1,809 executed, four known skips,
zero failures (1,813 registered), including seven new UI/memory cases.
Physical Deck speed and audio recovery remain unverified.

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
- Do not disable protection/coherency or change scheduling based only on
  CPU/GPU utilization. Preserve saves, raw dumps, maps and logs privately.

## Windows workflow

From the parent invoke `scripts/Resolve-Toolchain.ps1 -ExportPath`. Use Clang,
the Visual Studio C++ SDK, and the existing CMake multi-config build tree:

```
ast-grep scan --config third_party/rexglue-sdk/sgconfig.yml
ast-grep test --config third_party/rexglue-sdk/sgconfig.yml
cmake --build third_party/rexglue-sdk/out/build/win-amd64 --config RelWithDebInfo --target unit_tests --parallel 8
ctest --test-dir third_party/rexglue-sdk/out/build/win-amd64 -C RelWithDebInfo --output-on-failure
cmake --build third_party/rexglue-sdk/out/build/win-amd64 --config Release --target install --parallel 8
```

The scoped AST rule guards unbounded copying in dialog/diagnostic paths.
Expand rules and fixtures with new architectural constraints; reusable rules
belong in `utils/` once needed. Parent AST gates remain required as well.
Tests under `[ui][dialog]` need no native window, SDL initialization or GPU.
The four known skipped BitStream cases are not passing physical-game tests.

Review and test code before a four-part version/tag change. Any public SDK
surface change advances the CMake major/minor floor; the immutable four-part
tag determines installed version. After local commit/tag, rebuild/install
matching Release libraries, pin the exact SHA in parent consumers and run
their gates. Do not push until parent milestone authorization/gates permit.
