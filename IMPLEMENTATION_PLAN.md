# Agent Test Harness — Implementation Plan

> This document tracks the implementation plan for the Agent Test Harness described in [AGENT_TEST_HARNESS.md](AGENT_TEST_HARNESS.md). Delete this file once implementation is complete.

## Phase 1: Core Harness

Build the minimal headless runner with the interactive REPL protocol.

1. Create `AgentTester/` directory with `main.c`.
2. Implement headless emulator initialization (based on existing `Tester/main.c`):
   - `GB_init`, `GB_load_rom`, `GB_load_boot_rom`
   - Set up pixel buffer, RGB encode callback, log callback
   - Turbo mode enabled by default (no frame timing)
   - Disable joypad bouncing, disable randomization for determinism
3. Implement the interactive REPL command loop on stdin/stdout:
   - `load`, `reset`, `quit`
   - `run <N>`, `run_until_vblank`
   - `press`, `release`, `set_keys`
   - `screenshot`, `screen_hash`
   - `save_state`, `load_state`
   - `read_memory`, `write_memory`, `registers`
   - `set turbo`, `set model`, `set rendering`
4. Integrate PNG screenshot output (reuse `SDL/save_png/` or write raw PNG via libpng/AppKit).
5. Add `make agent-tester` build target to the Makefile.
6. Verify with manual stdin testing against a known ROM.

### Key decisions
- **Boot ROMs**: look for them relative to the executable (same as Tester), allow `--boot` override.
- **Error handling**: all errors return `ERR <message>` on stdout; never crash on bad input.
- **Line buffering**: force stdout line buffering so agents see responses immediately.

## Phase 2: Performance Profiling

Wire up per-frame metrics collection.

1. After each `GB_run_frame()`, record:
   - `GB_debugger_get_frame_cpu_usage()` → CPU usage ratio
   - `gb->last_frame_busy_cycles` and `gb->last_frame_idle_cycles`
   - Hash of current pixel buffer (for screen-change detection)
2. Implement `perf_frame` command (single-frame snapshot).
3. Implement `perf_start` / `perf_stop` commands:
   - Accumulate per-frame data into an array between start/stop.
   - On stop, compute summary: avg/min/max CPU usage, histogram, slowdown events.
   - Output summary as a single JSON line.
4. Implement slowdown detection:
   - Track runs of consecutive frames with CPU usage > 95% AND unchanged screen.
   - Report each run as a slowdown event with start/end frame, duration, avg CPU usage.

## Phase 3: Script Mode & Assertions

Add batch execution from JSON test scripts.

1. Vendor cJSON (~2 files: `cJSON.c`, `cJSON.h`) into `AgentTester/`.
2. Implement `--script <path.json>` mode:
   - Parse the JSON script format defined in the PRD.
   - Execute each test's action sequence using the same internal functions as the REPL.
   - Collect results into a JSON output structure.
3. Implement assertion evaluators:
   - `assert_screen_not_blank` — check if all pixels are identical.
   - `assert_screen_hash` — SHA-256 of pixel buffer vs expected.
   - `assert_screen_diff` — pixel-by-pixel comparison against reference PNG, with threshold.
   - `assert_max_cpu_usage` / `assert_avg_cpu_usage` — from profiling data.
   - `assert_no_slowdown_events` — from profiling data.
   - `assert_memory` — `GB_read_memory()` at address vs expected bytes.
   - `assert_register` — `GB_get_registers()` vs expected value.
4. Write JSON results to `--output <path.json>` or stdout.
5. Exit code: 0 if all tests pass, 1 if any fail.

## Phase 4: Test Game & Validation

Build a minimal GBDK-based ROM specifically designed to exercise the harness.

1. Create `AgentTester/test_rom/` with a GBDK project.
2. The ROM should have deterministic, scripted behavior:
   - **Screen 1 (title)**: display a static image on startup, wait for START.
   - **Screen 2 (input test)**: display which buttons are currently pressed (visual echo).
   - **Screen 3 (perf test)**: a busy loop that intentionally burns CPU cycles, causing measurable slowdown. Toggle between light load and heavy load with A button.
   - **Screen 4 (memory test)**: write known values to a fixed RAM address that the harness can read.
3. Write test scripts that exercise each harness feature against this ROM:
   - `test_title.json` — load ROM, wait, screenshot, assert not blank, assert hash.
   - `test_input.json` — press each button, screenshot, verify the button is echoed on screen.
   - `test_perf.json` — navigate to perf test screen, trigger heavy load, assert slowdown detected.
   - `test_memory.json` — navigate to memory test screen, read known address, assert value.
4. Add a `make test-agent-tester` target that builds the test ROM and runs all test scripts.

### Test ROM requirements
- Must build with GBDK-2020 (installed at `/Users/ryan/Projects/gbdk`).
- Must be fully deterministic (no randomization, no RTC dependency).
- Should be as small and simple as possible — this is a test fixture, not a game.
- Target CGB mode (Game Boy Color) as the primary model, but should also work on DMG.

## Phase 5: Agent SDK Documentation

1. Create `AgentTester/AGENT_SDK.md` — the document another AI agent reads to learn how to use the harness.
2. Include:
   - How to build (`make agent-tester`)
   - How to launch in interactive mode
   - Complete command reference with examples
   - How to interpret performance data
   - How to write and run test scripts
   - Common patterns (navigate menus, wait for screen transitions, profile a scene)
3. Keep it concise — optimized for LLM consumption, not human prose.

## File Layout (planned)

```
AgentTester/
├── main.c                  # Harness entry point
├── commands.c              # REPL command handlers
├── commands.h
├── profiler.c              # Per-frame perf collection & analysis
├── profiler.h
├── script_runner.c         # JSON script executor
├── script_runner.h
├── assertions.c            # Assertion evaluators
├── assertions.h
├── cJSON.c                 # Vendored JSON parser
├── cJSON.h
├── AGENT_SDK.md            # Agent-facing documentation
└── test_rom/
    ├── Makefile
    ├── main.c              # GBDK test ROM source
    ├── test_title.json     # Test scripts
    ├── test_input.json
    ├── test_perf.json
    └── test_memory.json
```
