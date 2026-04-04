# Agent Test Harness — Product Requirements

## Overview

An automated test harness that allows AI agents (or CI systems) to run SameBoy headlessly, execute scripted input sequences, capture screenshots, and collect performance profiles. The harness enables two modes of operation:

1. **Testing** — validate product requirements, detect regressions, and verify bug fixes by executing scripted input sequences and asserting on visual output and game state.
2. **Performance Profiling** — measure per-frame CPU usage to identify slowdowns that would be perceptible to a player on real hardware.

## Motivation

Manual playtesting is the bottleneck. An AI agent that can launch the emulator, play through specific scenarios, inspect the screen, and report results removes the human from the loop for repeatable verification tasks. This is especially valuable for:

- Regression testing after ROM changes (did the title screen break?)
- Validating that a bug fix actually works (navigate to the broken screen, confirm it renders correctly)
- Performance profiling (does the battle system cause visible slowdown?)
- Compatibility testing across hardware models (DMG vs CGB vs AGB)

## Background: Game Boy Performance Model

Unlike modern platforms where frames are dropped under heavy CPU load, the Game Boy PPU always renders at a fixed ~59.7 Hz. When game logic exceeds the per-frame CPU budget (70,224 dots), the game's main loop takes multiple VBlank periods to complete. The player perceives this as the game running in slow motion — sprites move at half speed, animations stutter, etc.

The key metric is **CPU usage per frame**: the ratio of busy cycles to total cycles. When this approaches 100%, the CPU has no idle time (no HALT between frames), meaning the game is at or past its performance limit. SameBoy already tracks `last_frame_busy_cycles` and `last_frame_idle_cycles` internally, exposed via `GB_debugger_get_frame_cpu_usage()`.

## Architecture

### New Build Target: `AgentTester`

A new C program (`AgentTester/main.c`) that links against the SameBoy Core library. No SDL, no display, no audio — purely headless. It operates in two modes selected by CLI flags.

```
sameboy_agent_tester --interactive [--model CGB] [--boot boot.bin]
sameboy_agent_tester --script test.json [--model CGB] [--boot boot.bin]
```

Built via `make agent-tester`. Uses the existing PNG export code from `SDL/save_png/` for screenshot output.

### Mode 1: Interactive (REPL)

The agent launches the harness as a subprocess and communicates via stdin/stdout. Each command is a single line; each response starts with `OK` or `ERR`, followed by structured data on the same or subsequent lines. This mode is designed for exploratory testing where the agent needs to see the screen and decide what to do next.

#### Command Protocol

```
# Lifecycle
load <rom_path>                    → OK
reset                              → OK
quit                               → (process exits)

# Execution
run <N>                            → OK frames=<N> cycles=<total>
run_until_vblank                   → OK cycles=<n>

# Input
press <button> [frames]            → OK          (hold for N frames, default 1)
release <button>                   → OK
set_keys <mask>                    → OK          (bitmask of all buttons)

# Display
screenshot <path.png>              → OK <width>x<height>
screen_hash                        → OK <sha256>

# State
save_state <path>                  → OK
load_state <path>                  → OK

# Memory / Registers
read_memory <addr> [length]        → OK <hex bytes>
write_memory <addr> <hex bytes>    → OK
registers                          → OK AF=xxxx BC=xxxx DE=xxxx HL=xxxx SP=xxxx PC=xxxx

# Performance
perf_frame                         → OK cpu_usage=<0.0-1.0> busy=<n> idle=<n>
perf_start                         → OK
perf_stop                          → OK (prints summary JSON to stdout)

# Configuration
set turbo <on|off>                 → OK
set model <DMG|CGB|AGB|SGB>       → OK
set rendering <on|off>             → OK
```

**Button names:** `A`, `B`, `START`, `SELECT`, `UP`, `DOWN`, `LEFT`, `RIGHT`

#### Example Agent Session

```
> load /path/to/game.gb
OK
> run 300
OK frames=300 cycles=21067200
> screenshot /tmp/title.png
OK 160x144
> press START 10
OK
> run 120
OK frames=120 cycles=8426880
> screenshot /tmp/after_start.png
OK 160x144
> perf_frame
OK cpu_usage=0.61 busy=42837 idle=27387
> quit
```

### Mode 2: Script (Batch)

The agent generates a JSON test script, the harness executes it, and writes a JSON results file. This is for repeatable, CI-friendly tests.

#### Script Format

```json
{
  "rom": "game.gb",
  "model": "CGB",
  "boot_rom": null,
  "tests": [
    {
      "name": "title_screen_renders",
      "save_state": null,
      "actions": [
        {"run": 300},
        {"screenshot": "title.png"},
        {"assert_screen_not_blank": true},
        {"assert_screen_hash": "a1b2c3..."}
      ]
    },
    {
      "name": "menu_navigation",
      "save_state": "after_title.state",
      "actions": [
        {"press": "START", "frames": 10},
        {"run": 60},
        {"press": "A", "frames": 10},
        {"run": 120},
        {"screenshot": "in_game.png"},
        {"assert_screen_not_blank": true}
      ]
    },
    {
      "name": "battle_performance",
      "save_state": "battle_start.state",
      "actions": [
        {"perf_start": true},
        {"run": 600},
        {"perf_stop": true},
        {"assert_max_cpu_usage": 0.95},
        {"assert_no_slowdown_events": true}
      ]
    }
  ]
}
```

#### Results Format

```json
{
  "rom": "game.gb",
  "model": "CGB",
  "timestamp": "2026-04-03T12:00:00Z",
  "results": [
    {
      "name": "title_screen_renders",
      "pass": true,
      "duration_ms": 42,
      "screenshots": ["title.png"],
      "assertions": [
        {"type": "screen_not_blank", "pass": true},
        {"type": "screen_hash", "expected": "a1b2c3...", "actual": "a1b2c3...", "pass": true}
      ]
    },
    {
      "name": "battle_performance",
      "pass": false,
      "duration_ms": 87,
      "perf": {
        "total_frames": 600,
        "avg_cpu_usage": 0.72,
        "max_cpu_usage": 0.99,
        "min_cpu_usage": 0.45,
        "cpu_usage_histogram": {
          "0-50%": 120,
          "50-75%": 300,
          "75-90%": 140,
          "90-95%": 30,
          "95-100%": 10
        },
        "slowdown_events": [
          {
            "start_frame": 412,
            "end_frame": 418,
            "duration_frames": 7,
            "avg_cpu_usage": 0.98,
            "screen_unchanged_frames": 4
          }
        ]
      },
      "assertions": [
        {"type": "max_cpu_usage", "threshold": 0.95, "actual": 0.99, "pass": false},
        {"type": "no_slowdown_events", "count": 1, "pass": false}
      ]
    }
  ]
}
```

### Performance Profiling Detail

When profiling is active, the harness collects per-frame metrics:

| Metric | Source | Description |
|--------|--------|-------------|
| CPU usage | `GB_debugger_get_frame_cpu_usage()` | Busy cycles / total cycles. Values near 1.0 indicate the game is consuming its entire frame budget. |
| Busy cycles | `gb->last_frame_busy_cycles` | CPU cycles spent executing instructions this frame. |
| Idle cycles | `gb->last_frame_idle_cycles` | CPU cycles spent in HALT (waiting for VBlank). |
| Frame cycle count | `GB_run_frame()` return value | Total nanoseconds for the frame (derived from cycle count). |
| Screen changed | Pixel buffer comparison | Whether the rendered frame differs from the previous one. |

**Slowdown detection algorithm:**
1. Each frame, record CPU usage and whether the screen pixel buffer changed.
2. A "slowdown event" is a run of consecutive frames where CPU usage > 95% AND the screen content is unchanged for 2+ frames in a row.
3. This detects the specific Game Boy slowdown pattern: the game's main loop spans multiple VBlanks, so the PPU re-renders the same state while the CPU catches up.

### Assertions

| Assertion | Description |
|-----------|-------------|
| `assert_screen_not_blank` | Fails if all pixels in the frame are the same color. |
| `assert_screen_hash` | Fails if SHA-256 of the pixel buffer doesn't match expected value. |
| `assert_screen_diff` | Compares against a reference PNG; fails if pixel difference exceeds threshold. |
| `assert_max_cpu_usage` | Fails if any frame's CPU usage exceeds the given threshold. |
| `assert_avg_cpu_usage` | Fails if average CPU usage over the profiled range exceeds threshold. |
| `assert_no_slowdown_events` | Fails if any slowdown events were detected. |
| `assert_memory` | Fails if memory at a given address doesn't match expected bytes. |
| `assert_register` | Fails if a CPU register doesn't match expected value. |

### Multi-Model Testing

Scripts can specify a `model` field per test to run the same ROM on different hardware models:

```json
{
  "name": "title_screen_dmg",
  "model": "DMG",
  "actions": [{"run": 300}, {"screenshot": "title_dmg.png"}]
},
{
  "name": "title_screen_cgb",
  "model": "CGB",
  "actions": [{"run": 300}, {"screenshot": "title_cgb.png"}]
}
```

## Dependencies

- SameBoy Core library (already in-tree)
- libpng (already a dependency of the SDL frontend on Linux; on macOS we can use the AppKit PNG writer already in `SDL/save_png/`)
- cJSON or equivalent for JSON parsing/writing (to be vendored, ~1 file)

## Non-Goals

- **GUI / windowed mode** — this is explicitly headless.
- **Audio capture** — not needed for the initial version; could be added later.
- **Network protocol** — stdin/stdout is sufficient for agent subprocess communication.
- **Lua/Python scripting** — the JSON script format and REPL protocol are simpler and sufficient for agent-driven workflows.
