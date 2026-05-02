#import <Cocoa/Cocoa.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

FILE *gb_perf_log_file = NULL;
FILE *gb_pc_log_file = NULL;
FILE *gb_tag_log_file = NULL;
uint32_t gb_pc_sample_cycles = 1024;

static uint64_t mono_start_ns = 0;

uint64_t gb_monotonic_ms(void)
{
    static mach_timebase_info_data_t tb = {0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t now_ns = mach_absolute_time() * tb.numer / tb.denom;
    if (mono_start_ns == 0) mono_start_ns = now_ns;
    return (now_ns - mono_start_ns) / 1000000ull;
}

static void close_perf_log(void)
{
    if (gb_perf_log_file) {
        fclose(gb_perf_log_file);
        gb_perf_log_file = NULL;
    }
}

static void close_pc_log(void)
{
    if (gb_pc_log_file) {
        fclose(gb_pc_log_file);
        gb_pc_log_file = NULL;
    }
}

static void close_tag_log(void)
{
    if (gb_tag_log_file) {
        fclose(gb_tag_log_file);
        gb_tag_log_file = NULL;
    }
}

static void print_help(const char *argv0)
{
    printf(
        "Usage: %s [rom_path] [options]\n"
        "\n"
        "Options:\n"
        "  rom_path                  Optional .gb/.gbc ROM to open at launch.\n"
        "  --perf-log <path>         Append per-frame CPU utilization to <path> (CSV).\n"
        "  --pc-log <path>           Append periodic PC samples to <path> (CSV).\n"
        "  --pc-sample-cycles <N>    PC sampling interval in 16MHz cycles\n"
        "                            (default 1024 = ~140 samples per normal frame).\n"
        "  --tag-log <path>          Append ROM-emitted trace tags to <path> (CSV).\n"
        "                            See \"--tag-log format\" below.\n"
        "  --update-launch           Internal: used after a software update.\n"
        "  --help, -h                Show this help and exit.\n"
        "\n"
        "Both logs share a common notion of \"timestamp_ms\": milliseconds since\n"
        "the SameBoy process started, monotonic, host wallclock. Use deltas\n"
        "between consecutive perf-log timestamps to compute observed FPS:\n"
        "  fps_i = 1000 / (timestamp_ms[i] - timestamp_ms[i-1])\n"
        "Sustained fps below 60 indicates the host couldn't keep up (or turbo\n"
        "mode was active, in which case fps will be much higher).\n"
        "\n"
        "------------------------------------------------------------------\n"
        "--perf-log format\n"
        "------------------------------------------------------------------\n"
        "  CSV, one row per emulated frame. Header is written only on file\n"
        "  creation; subsequent runs append without re-emitting the header.\n"
        "\n"
        "  A '# session start: <rom>' comment line is written when the first\n"
        "  frame of a new ROM session is emitted (loadROM + reset). The frame\n"
        "  counter restarts at 0. Skip lines beginning with '#' when parsing.\n"
        "\n"
        "  Columns:\n"
        "    frame         0-based frame counter for the current session.\n"
        "    timestamp_ms  Host monotonic milliseconds since process start.\n"
        "    busy_cycles   Cycles classified as doing real work this frame\n"
        "                  (SameBoy's normalized 16MHz units).\n"
        "    idle_cycles   Cycles classified as waiting. Includes:\n"
        "                    - HALT / STOP (the usual case)\n"
        "                    - tight spin loops with no game-state writes\n"
        "                      (>256 cycles without a WRAM/VRAM/OAM/HRAM\n"
        "                       write). This catches games like Tetris that\n"
        "                       poll LY instead of HALT-ing.\n"
        "                  busy + idle is ~140448 per normal-speed frame.\n"
        "    cpu_pct       100 * busy / (busy + idle), 2 decimals, 0.00-100.00.\n"
        "                  The headline utilization number. Reflects real\n"
        "                  work for both HALT-using and polling games.\n"
        "    mem_writes    Distinct writes to game state this frame. Counts\n"
        "                  WRAM, VRAM, OAM, HRAM. Excludes ROM-space writes\n"
        "                  (MBC bank-switching) and IO registers. Raw signal\n"
        "                  of how much state churn the frame produced.\n"
        "    unique_pcs    Distinct PC values executed this frame. A polling\n"
        "                  loop hits 3-4 PCs; real game logic hits hundreds\n"
        "                  to thousands. Useful for hot-spot triage.\n"
        "\n"
        "  Only NORMAL vblank frames are emitted (no LCD-off / artificial /\n"
        "  skipped frames). At 60Hz, expect ~60 rows per wallclock second.\n"
        "\n"
        "  Example (Tetris title screen, polling LY):\n"
        "    frame,timestamp_ms,busy_cycles,idle_cycles,cpu_pct,mem_writes,unique_pcs\n"
        "    # session start: tetris.gb\n"
        "    498,9037,6824,133624,4.86,97,272\n"
        "    499,9053,6856,133592,4.88,97,272\n"
        "\n"
        "------------------------------------------------------------------\n"
        "--pc-log format\n"
        "------------------------------------------------------------------\n"
        "  CSV. One row per PC sample (taken every --pc-sample-cycles cycles).\n"
        "  Useful for hot-spot analysis: with .sym files in hand, group rows\n"
        "  by (bank, pc) and the heaviest buckets are your hot code.\n"
        "\n"
        "  At default 1024-cycle interval and 60Hz, expect ~8400 rows per\n"
        "  wallclock second (~250 KB/s). Increase --pc-sample-cycles to thin\n"
        "  the data, decrease it for higher resolution.\n"
        "\n"
        "  Columns:\n"
        "    timestamp_ms  Host monotonic milliseconds since process start.\n"
        "    bank          Hex ROM bank that pc maps to (0 for RAM/IO).\n"
        "    pc            Hex program counter (16-bit).\n"
        "\n"
        "  Example:\n"
        "    timestamp_ms,bank,pc\n"
        "    1247,00,1A40\n"
        "    1247,00,1A48\n"
        "    1247,02,4080\n"
        "\n"
        "------------------------------------------------------------------\n"
        "--tag-log format\n"
        "------------------------------------------------------------------\n"
        "  CSV. One row per ROM-emitted trace tag. The ROM emits tags by\n"
        "  writing a byte to IO address $FF03 -- an unused IO register on\n"
        "  real hardware, so writes are silently ignored on a real DMG/CGB.\n"
        "  The same ROM runs unmodified on real hardware and on SameBoy.\n"
        "\n"
        "  Emit a tag from RGBASM:\n"
        "    ld a, MY_TAG_ID\n"
        "    ldh [$03], a       ; ~16 cycles\n"
        "\n"
        "  Or from GBDK C:\n"
        "    *(volatile uint8_t*)0xFF03 = MY_TAG_ID;\n"
        "\n"
        "  The ROM author defines their own tag scheme: e.g. low nibble for\n"
        "  routine ID, high bit for begin/end. SameBoy logs the byte verbatim;\n"
        "  decoding is post-processing.\n"
        "\n"
        "  Columns:\n"
        "    timestamp_ms  Host monotonic milliseconds since process start.\n"
        "    bank          Hex ROM bank where the write occurred.\n"
        "    pc            Hex program counter just after the LDH instruction.\n"
        "    tag           Hex byte value written.\n"
        "\n"
        "  Example:\n"
        "    timestamp_ms,bank,pc,tag\n"
        "    1247,01,4082,A1\n"
        "    1264,01,40FE,A2\n"
        "\n"
        "  All log files are line-buffered, so a crash loses at most one row.\n",
        argv0);
}

static void extract_arg(int *argc, const char **argv, int idx, int n)
{
    for (int i = idx + n; i < *argc; i++) {
        argv[i - n] = argv[i];
    }
    *argc -= n;
}

static FILE *open_csv_log(const char *path, const char *header, void (*close_fn)(void))
{
    bool existed = (access(path, F_OK) == 0);
    FILE *fp = fopen(path, "a");
    if (!fp) {
        fprintf(stderr, "sameboy: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    setvbuf(fp, NULL, _IOLBF, 0);
    if (!existed) fputs(header, fp);
    atexit(close_fn);
    return fp;
}

int main(int argc, const char *argv[])
{
    gb_monotonic_ms(); // initialize start time

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--perf-log") == 0 && i + 1 < argc) {
            gb_perf_log_file = open_csv_log(argv[i + 1],
                "frame,timestamp_ms,busy_cycles,idle_cycles,cpu_pct,mem_writes,unique_pcs\n",
                close_perf_log);
            if (!gb_perf_log_file) return 1;
            extract_arg(&argc, argv, i, 2);
            i--;
            continue;
        }
        if (strcmp(argv[i], "--pc-log") == 0 && i + 1 < argc) {
            gb_pc_log_file = open_csv_log(argv[i + 1],
                "timestamp_ms,bank,pc\n",
                close_pc_log);
            if (!gb_pc_log_file) return 1;
            extract_arg(&argc, argv, i, 2);
            i--;
            continue;
        }
        if (strcmp(argv[i], "--pc-sample-cycles") == 0 && i + 1 < argc) {
            gb_pc_sample_cycles = (uint32_t)strtoul(argv[i + 1], NULL, 10);
            if (gb_pc_sample_cycles == 0) gb_pc_sample_cycles = 1024;
            extract_arg(&argc, argv, i, 2);
            i--;
            continue;
        }
        if (strcmp(argv[i], "--tag-log") == 0 && i + 1 < argc) {
            gb_tag_log_file = open_csv_log(argv[i + 1],
                "timestamp_ms,bank,pc,tag\n",
                close_tag_log);
            if (!gb_tag_log_file) return 1;
            extract_arg(&argc, argv, i, 2);
            i--;
            continue;
        }
    }
    return NSApplicationMain(argc, argv);
}
