# SameBoy Agent Test Harness — SDK Reference

Use this document to learn how to operate the SameBoy Agent Test Harness programmatically. The harness runs Game Boy ROMs headlessly and provides a text-based protocol for controlling emulation, capturing screenshots, and profiling performance.

## Quick Start

```bash
# Build
make agent-tester

# Launch interactive mode
./build/bin/agent-tester/sameboy_agent_tester --interactive

# Run a test script
./build/bin/agent-tester/sameboy_agent_tester --script tests/my_test.json --output results.json
```

## Interactive Mode

Launch as a subprocess. Send commands to stdin, read responses from stdout (one per line). Every response begins with `OK` or `ERR`.

### Commands

#### Loading & Lifecycle

| Command | Response | Description |
|---------|----------|-------------|
| `load <rom_path>` | `OK` | Load a .gb/.gbc ROM file. |
| `reset` | `OK` | Reset the emulator to power-on state. |
| `quit` | *(exits)* | Terminate the harness process. |

#### Execution

| Command | Response | Description |
|---------|----------|-------------|
| `run <N>` | `OK frames=<N> cycles=<total>` | Advance N frames. |
| `run_until_vblank` | `OK cycles=<n>` | Run until the next VBlank. |

#### Input

| Command | Response | Description |
|---------|----------|-------------|
| `press <button> [frames]` | `OK` | Press and hold button for N frames (default 1), then release. Advances N frames. |
| `release <button>` | `OK` | Release a held button immediately. |
| `set_keys <mask>` | `OK` | Set all button states at once via bitmask. |

**Button names:** `A`, `B`, `START`, `SELECT`, `UP`, `DOWN`, `LEFT`, `RIGHT`

**Bitmask bits (for `set_keys`):**

| Bit | Button |
|-----|--------|
| 0 | RIGHT |
| 1 | LEFT |
| 2 | UP |
| 3 | DOWN |
| 4 | A |
| 5 | B |
| 6 | SELECT |
| 7 | START |

#### Display

| Command | Response | Description |
|---------|----------|-------------|
| `screenshot <path.png>` | `OK <width>x<height>` | Save current frame to PNG. |
| `screen_hash` | `OK <sha256>` | Get SHA-256 hash of the current pixel buffer. |

#### Save States

| Command | Response | Description |
|---------|----------|-------------|
| `save_state <path>` | `OK` | Save emulator state to file. |
| `load_state <path>` | `OK` | Load emulator state from file. |

#### Memory & Registers

| Command | Response | Description |
|---------|----------|-------------|
| `read_memory <addr> [len]` | `OK <hex bytes>` | Read len bytes (default 1) from address. Addr is hex (e.g. `C000`). |
| `write_memory <addr> <hex>` | `OK` | Write bytes to address. |
| `registers` | `OK AF=xxxx BC=xxxx DE=xxxx HL=xxxx SP=xxxx PC=xxxx` | Dump CPU registers. |

#### Performance Profiling

| Command | Response | Description |
|---------|----------|-------------|
| `perf_frame` | `OK cpu_usage=<0.0-1.0> busy=<n> idle=<n>` | Get CPU usage for the most recent frame. |
| `perf_start` | `OK` | Begin recording per-frame profiling data. |
| `perf_stop` | `OK <json>` | Stop recording and output profiling summary as JSON. |

#### Configuration

| Command | Response | Description |
|---------|----------|-------------|
| `set turbo <on\|off>` | `OK` | Toggle turbo mode (default: on). |
| `set model <DMG\|CGB\|AGB\|SGB>` | `OK` | Set hardware model. Takes effect on next `load` or `reset`. |
| `set rendering <on\|off>` | `OK` | Toggle rendering. Off = faster but no screenshots. |

### Example: Verify a Title Screen

```
load /path/to/game.gb
run 300
screenshot /tmp/title.png
screen_hash
quit
```

### Example: Navigate a Menu and Profile

```
load /path/to/game.gb
run 300
press START 10
run 60
press A 10
run 120
perf_start
run 600
perf_stop
screenshot /tmp/gameplay.png
quit
```

### Example: Check Game State via Memory

```
load /path/to/game.gb
run 600
read_memory C100 4
registers
quit
```

## Script Mode

Pass a JSON file describing tests to run in batch. Results are written as JSON.

```bash
./sameboy_agent_tester --script test.json --output results.json
```

### Script Format

```json
{
  "rom": "path/to/game.gb",
  "model": "CGB",
  "tests": [
    {
      "name": "test_name",
      "save_state": null,
      "actions": [
        {"run": 300},
        {"press": "START", "frames": 10},
        {"run": 120},
        {"screenshot": "output.png"},
        {"assert_screen_not_blank": true},
        {"assert_screen_hash": "expected_sha256_hex"},
        {"perf_start": true},
        {"run": 600},
        {"perf_stop": true},
        {"assert_max_cpu_usage": 0.95},
        {"assert_no_slowdown_events": true},
        {"assert_memory": {"address": "C100", "expected": "01FF"}},
        {"save_state": "checkpoint.state"}
      ]
    }
  ]
}
```

### Action Reference

| Action | Description |
|--------|-------------|
| `{"run": N}` | Advance N frames. |
| `{"press": "BUTTON", "frames": N}` | Hold button for N frames. |
| `{"screenshot": "path.png"}` | Save screenshot. |
| `{"save_state": "path"}` | Save emulator state. |
| `{"load_state": "path"}` | Load emulator state. |
| `{"perf_start": true}` | Begin profiling. |
| `{"perf_stop": true}` | End profiling, record results. |
| `{"assert_screen_not_blank": true}` | Assert screen is not a single color. |
| `{"assert_screen_hash": "hex"}` | Assert screen SHA-256 matches. |
| `{"assert_max_cpu_usage": 0.95}` | Assert no frame exceeded this CPU usage. |
| `{"assert_avg_cpu_usage": 0.80}` | Assert average CPU usage is below threshold. |
| `{"assert_no_slowdown_events": true}` | Assert no slowdown events detected. |
| `{"assert_memory": {"address": "C100", "expected": "01FF"}}` | Assert memory contents. |

### Results Format

Exit code 0 = all tests passed, 1 = any test failed.

```json
{
  "rom": "game.gb",
  "model": "CGB",
  "timestamp": "2026-04-03T12:00:00Z",
  "pass": false,
  "results": [
    {
      "name": "test_name",
      "pass": true,
      "duration_ms": 42,
      "screenshots": ["output.png"],
      "assertions": [
        {"type": "screen_not_blank", "pass": true},
        {"type": "screen_hash", "expected": "abc...", "actual": "abc...", "pass": true}
      ],
      "perf": {
        "total_frames": 600,
        "avg_cpu_usage": 0.62,
        "max_cpu_usage": 0.88,
        "min_cpu_usage": 0.40,
        "slowdown_events": []
      }
    }
  ]
}
```

## Understanding Performance Data

The Game Boy PPU renders at a fixed ~59.7 Hz. The CPU has 70,224 dots (cycles) per frame to execute game logic. Games that finish early call `HALT` to wait for VBlank — this idle time is what `cpu_usage` measures.

| CPU Usage | Interpretation |
|-----------|---------------|
| < 50% | Light load. Plenty of headroom. |
| 50–75% | Moderate load. Typical for most games. |
| 75–90% | Heavy load. Complex scenes but still smooth. |
| 90–95% | Near capacity. May stutter on edge cases. |
| 95–100% | **Slowdown likely.** Game loop is exceeding frame budget. Player will perceive slow motion. |

**Slowdown events** are detected when CPU usage stays above 95% for multiple consecutive frames AND the screen content doesn't change — meaning the game's main loop is taking multiple VBlanks to produce a single visual update.

## CLI Reference

```
sameboy_agent_tester [options]

Options:
  --interactive          Start in interactive REPL mode (default)
  --script <path.json>   Run a test script in batch mode
  --output <path.json>   Write results to file (script mode, default: stdout)
  --model <model>        Set hardware model: DMG, CGB (default), AGB, SGB
  --boot <path.bin>      Path to boot ROM (optional, uses built-in if omitted)
```
