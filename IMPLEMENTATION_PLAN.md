# Agent Test Harness — Implementation Plan

> This document tracks the implementation plan for the Agent Test Harness described in [AGENT_TEST_HARNESS.md](AGENT_TEST_HARNESS.md). Delete this file once implementation is complete.

Each step below is a single commit. Steps are ordered so that each one builds on the last and can be tested independently before moving on.

---

## Step 1: Skeleton build target

- Create `AgentTester/main.c` with a minimal `main()` that prints a version string and exits.
- Add `agent-tester` target to `Makefile` that compiles and links against the Core library.
- **Verify:** `make agent-tester` succeeds, `./build/bin/agent-tester/sameboy_agent_tester` prints version and exits cleanly.

## Step 2: Headless emulator init + ROM loading

- Initialize `GB_gameboy_t` with `GB_init`, set up pixel buffer, RGB encode callback, log callback.
- Accept `--model` and `--boot` CLI args.
- Accept a ROM path as positional arg, load it with `GB_load_rom`.
- Enable turbo mode, disable joypad bouncing, disable randomization.
- Run a single frame with `GB_run_frame()` and print the cycle count, then exit.
- **Verify:** `./sameboy_agent_tester game.gb` prints a cycle count and exits without crashing. Test with a known ROM (e.g. one of the acid2 test ROMs in `.github/actions/`).

## Step 3: REPL loop with `load`, `reset`, `quit`, `run`

- Add `--interactive` flag (default mode).
- Implement the stdin/stdout REPL loop with line buffering.
- Implement commands: `load <rom>`, `reset`, `quit`, `run <N>`.
- All responses start with `OK` or `ERR`.
- **Verify:** launch interactively, type `load .github/actions/cgb-acid2.gbc`, `run 300`, `quit`. Confirm `OK` responses and frame/cycle counts.

## Step 4: Screenshot output

- Integrate PNG writing (reuse `SDL/save_png/` or write a minimal PNG writer).
- Implement `screenshot <path.png>` command.
- Implement `screen_hash` command (SHA-256 of raw pixel buffer).
- **Verify:** `screenshot /tmp/test.png` produces a valid PNG. `screen_hash` returns a consistent hash for the same ROM + frame count. Open the PNG and confirm it looks correct.

## Step 5: Input commands

- Implement `press <button> [frames]` — hold button, advance N frames, release.
- Implement `release <button>` — release a held button without advancing.
- Implement `set_keys <mask>` — set all buttons at once via bitmask.
- **Verify:** load a ROM that responds to input (e.g. acid2 or any game). `press START 10`, `run 120`, `screenshot /tmp/after_input.png`. Confirm the screenshot shows the game advanced past the title screen.

## Step 6: Save state and memory commands

- Implement `save_state <path>`, `load_state <path>`.
- Implement `read_memory <addr> [len]`, `write_memory <addr> <hex>`.
- Implement `registers` — dump AF, BC, DE, HL, SP, PC.
- **Verify:** `save_state /tmp/test.state`, `run 60`, `load_state /tmp/test.state`, `screen_hash` — hash should match the hash at save time. `read_memory FF44 1` returns the LY register value. `registers` returns valid hex values.

## Step 7: Configuration commands

- Implement `set turbo <on|off>`.
- Implement `set model <DMG|CGB|AGB|SGB>` (takes effect on next `load`/`reset`).
- Implement `set rendering <on|off>`.
- **Verify:** `set model DMG`, `load game.gb`, `run 300`, `screenshot /tmp/dmg.png` — confirm DMG-style output. `set rendering off`, `run 300` runs faster (no rendering overhead).

## Step 8: Performance profiling — `perf_frame`

- After each `GB_run_frame()`, store the most recent frame's CPU usage, busy cycles, and idle cycles.
- Implement `perf_frame` command — returns the last frame's metrics.
- **Verify:** `run 300`, `perf_frame` returns `OK cpu_usage=X.XX busy=NNNN idle=NNNN` with plausible values (CPU usage between 0 and 1, busy + idle > 0).

## Step 9: Performance profiling — `perf_start` / `perf_stop`

- Implement per-frame data accumulation between `perf_start` and `perf_stop`.
- Track: CPU usage, busy/idle cycles, screen hash per frame.
- On `perf_stop`, compute and output JSON summary: avg/min/max CPU usage, histogram, total frames.
- **Verify:** `perf_start`, `run 600`, `perf_stop` — returns valid JSON with histogram buckets that sum to 600.

## Step 10: Slowdown detection

- During profiling, detect consecutive frames with CPU usage > 95% AND unchanged screen hash.
- Report slowdown events in `perf_stop` output with start/end frame, duration, avg CPU usage, screen-unchanged count.
- **Verify:** will be fully validated with the test ROM in step 15. For now, confirm the fields appear (likely empty) when profiling a normal ROM.

## Step 11: Vendor cJSON

- Add `cJSON.c` and `cJSON.h` to `AgentTester/`.
- Update Makefile to compile cJSON into the agent-tester target.
- **Verify:** `make agent-tester` still builds cleanly.

## Step 12: Script mode — runner

- Implement `--script <path.json>` and `--output <path.json>` CLI args.
- Parse the JSON script format: `rom`, `model`, `tests[]` with `name`, `save_state`, `actions[]`.
- Execute each test's action sequence using the same internal functions as the REPL.
- Write JSON results (without assertions yet — just test name, pass=true, duration, screenshots).
- **Verify:** write a simple script that loads a ROM, runs 300 frames, takes a screenshot. Run it and confirm JSON output is valid and screenshot exists.

## Step 13: Assertions

- Implement assertion evaluators within script mode:
  - `assert_screen_not_blank`
  - `assert_screen_hash`
  - `assert_max_cpu_usage` / `assert_avg_cpu_usage`
  - `assert_no_slowdown_events`
  - `assert_memory`
- Set test `pass` to false if any assertion fails. Set exit code to 1 if any test fails.
- **Verify:** write a script with `assert_screen_not_blank: true` after running a known ROM for 300 frames. Confirm it passes. Write a script with a wrong `assert_screen_hash` and confirm it fails with exit code 1.

## Step 14: Test ROM — build the GBDK fixture

- Create `AgentTester/test_rom/` with `main.c` and `Makefile`.
- GBDK project at `/Users/ryan/Projects/gbdk`.
- ROM has 4 screens, navigated with START:
  - **Screen 1 (title):** static text "HARNESS TEST" on screen. Wait for START.
  - **Screen 2 (input echo):** show which buttons are currently held. Wait for START to advance.
  - **Screen 3 (perf stress):** idle loop by default. Press A to toggle a busy loop that burns CPU. Wait for START to advance.
  - **Screen 4 (memory probe):** write `0xDE 0xAD 0xBE 0xEF` to address `0xC0A0` on entry.
- Fully deterministic — no RNG, no RTC.
- Add `make test-rom` target.
- **Verify:** `make test-rom` produces a `.gb` file. Load it in the SDL frontend and manually confirm the 4 screens work.

## Step 15: End-to-end test scripts

- Write JSON test scripts exercising every harness feature against the test ROM:
  - `test_title.json` — load, run 300 frames, screenshot, assert not blank, record screen hash.
  - `test_input.json` — navigate to input screen, press each button, screenshot after each.
  - `test_perf.json` — navigate to perf screen, profile idle (assert low CPU), press A, profile busy (assert high CPU / slowdown detected).
  - `test_memory.json` — navigate to memory screen, `assert_memory` at `0xC0A0` equals `DEADBEEF`.
- Add `make test-agent-tester` target that builds the test ROM, builds the harness, and runs all test scripts.
- **Verify:** `make test-agent-tester` passes. Break something in the ROM or harness and confirm it fails.

## Step 16: Update SDK docs and clean up

- Update `AgentTester/AGENT_SDK.md` with any protocol changes that happened during implementation.
- Add common patterns section with real examples from the test scripts.
- Final review of `AGENT_TEST_HARNESS.md` for accuracy.
- **Verify:** another agent can read `AGENT_SDK.md` and successfully run a test against the test ROM without additional guidance.

---

## File Layout (planned)

```
AgentTester/
├── main.c                  # Harness entry point + REPL loop
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
