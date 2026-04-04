// Needs low-level access to gb struct for CPU usage cycle counts
#define GB_INTERNAL

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <Core/gb.h>
#include <Core/random.h>

#include "save_screenshot.h"

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#define SHA256_DIGEST_LENGTH CC_SHA256_DIGEST_LENGTH
static void sha256(const void *data, size_t len, unsigned char *out) {
    CC_SHA256(data, (CC_LONG)len, out);
}
#else
#include <openssl/sha.h>
static void sha256(const void *data, size_t len, unsigned char *out) {
    SHA256(data, len, out);
}
#endif

static GB_gameboy_t gb;
static uint32_t pixel_buffer[256 * 224];
static bool gb_inited = false;
static const char *boot_rom_path = NULL;
static char current_rom_path[1024] = {0};

typedef enum {
    MODEL_CGB,
    MODEL_DMG,
    MODEL_SGB,
    MODEL_AGB,
} model_t;

static model_t current_model = MODEL_CGB;

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static const char *executable_folder(void)
{
    static char path[1024] = {0,};
    if (path[0]) {
        return path;
    }
#ifdef __APPLE__
    uint32_t length = sizeof(path) - 1;
    _NSGetExecutablePath(&path[0], &length);
#else
#ifdef __linux__
    size_t __attribute__((unused)) length = readlink("/proc/self/exe", &path[0], sizeof(path) - 1);
    assert(length != -1);
#else
#ifdef _WIN32
    HMODULE hModule = GetModuleHandle(NULL);
    GetModuleFileName(hModule, path, sizeof(path) - 1);
#else
    getcwd(&path[0], sizeof(path) - 1);
    return path;
#endif
#endif
#endif
    size_t pos = strlen(path);
    while (pos) {
        pos--;
#ifdef _WIN32
        if (path[pos] == '\\') {
#else
        if (path[pos] == '/') {
#endif
            path[pos] = 0;
            break;
        }
    }
    return path;
}

static char *executable_relative_path(const char *filename)
{
    static char path[1024];
    snprintf(path, sizeof(path), "%s/%s", executable_folder(), filename);
    return path;
}

static uint32_t rgb_encode(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b)
{
    /* RGBA format for PNG output */
    return (r << 24) | (g << 16) | (b << 8) | 0xFF;
}

static void log_callback(GB_gameboy_t *gb, const char *string, GB_log_attributes_t attributes)
{
    fprintf(stderr, "[GB] %s", string);
}

static char *async_input_callback(GB_gameboy_t *gb)
{
    return NULL;
}

static void vblank_callback(GB_gameboy_t *gb, GB_vblank_type_t type)
{
    /* Placeholder for future use */
}

static const char *boot_rom_for_model(model_t model)
{
    if (boot_rom_path) return boot_rom_path;
    switch (model) {
        case MODEL_DMG: return executable_relative_path("dmg_boot.bin");
        case MODEL_SGB: return executable_relative_path("sgb2_boot.bin");
        case MODEL_AGB: return executable_relative_path("agb_boot.bin");
        case MODEL_CGB:
        default:        return executable_relative_path("cgb_boot.bin");
    }
}

static GB_model_t gb_model_for_model(model_t model)
{
    switch (model) {
        case MODEL_DMG: return GB_MODEL_DMG_B;
        case MODEL_SGB: return GB_MODEL_SGB2;
        case MODEL_AGB: return GB_MODEL_AGB_A;
        case MODEL_CGB:
        default:        return GB_MODEL_CGB_E;
    }
}

static bool init_and_load(const char *rom_path)
{
    if (gb_inited) {
        GB_free(&gb);
        gb_inited = false;
    }

    GB_init(&gb, gb_model_for_model(current_model));
    gb_inited = true;

    const char *boot = boot_rom_for_model(current_model);
    if (GB_load_boot_rom(&gb, boot)) {
        fprintf(stderr, "Warning: failed to load boot ROM from '%s'\n", boot);
    }

    GB_set_vblank_callback(&gb, (GB_vblank_callback_t)vblank_callback);
    GB_set_pixels_output(&gb, &pixel_buffer[0]);
    GB_set_rgb_encode_callback(&gb, rgb_encode);
    GB_set_log_callback(&gb, log_callback);
    GB_set_async_input_callback(&gb, async_input_callback);
    GB_set_color_correction_mode(&gb, GB_COLOR_CORRECTION_EMULATE_HARDWARE);
    GB_set_rtc_mode(&gb, GB_RTC_MODE_ACCURATE);
    GB_set_emulate_joypad_bouncing(&gb, false);
    GB_set_turbo_mode(&gb, true, true);

    if (GB_load_rom(&gb, rom_path)) {
        GB_free(&gb);
        gb_inited = false;
        return false;
    }

    snprintf(current_rom_path, sizeof(current_rom_path), "%s", rom_path);
    return true;
}

static model_t parse_model(const char *str)
{
    if (strcasecmp(str, "DMG") == 0) return MODEL_DMG;
    if (strcasecmp(str, "SGB") == 0) return MODEL_SGB;
    if (strcasecmp(str, "AGB") == 0) return MODEL_AGB;
    return MODEL_CGB;
}

/* Run N frames, return total cycle count */
static uint64_t run_frames(unsigned int n)
{
    uint64_t total_ns = 0;
    for (unsigned int i = 0; i < n; i++) {
        total_ns += GB_run_frame(&gb);
    }
    return total_ns;
}

/* REPL command handlers */

static void cmd_load(const char *args)
{
    /* Skip leading whitespace */
    while (*args == ' ') args++;
    if (*args == '\0') {
        printf("ERR missing rom path\n");
        return;
    }

    /* Strip trailing whitespace/newline */
    char path[1024];
    snprintf(path, sizeof(path), "%s", args);
    size_t len = strlen(path);
    while (len > 0 && (path[len-1] == ' ' || path[len-1] == '\n' || path[len-1] == '\r')) {
        path[--len] = '\0';
    }

    if (init_and_load(path)) {
        printf("OK\n");
    } else {
        printf("ERR failed to load ROM: %s\n", path);
    }
}

static void cmd_reset(void)
{
    if (!gb_inited || current_rom_path[0] == '\0') {
        printf("ERR no ROM loaded\n");
        return;
    }
    if (init_and_load(current_rom_path)) {
        printf("OK\n");
    } else {
        printf("ERR failed to reset\n");
    }
}

static void cmd_run(const char *args)
{
    if (!gb_inited) {
        printf("ERR no ROM loaded\n");
        return;
    }

    unsigned int n = 1;
    if (args && *args) {
        n = (unsigned int)strtoul(args, NULL, 10);
        if (n == 0) n = 1;
    }

    uint64_t total_ns = run_frames(n);
    printf("OK frames=%u cycles=%llu\n", n, (unsigned long long)total_ns);
}

static bool parse_button(const char *name, GB_key_t *key)
{
    if (strcasecmp(name, "A") == 0) { *key = GB_KEY_A; return true; }
    if (strcasecmp(name, "B") == 0) { *key = GB_KEY_B; return true; }
    if (strcasecmp(name, "START") == 0) { *key = GB_KEY_START; return true; }
    if (strcasecmp(name, "SELECT") == 0) { *key = GB_KEY_SELECT; return true; }
    if (strcasecmp(name, "UP") == 0) { *key = GB_KEY_UP; return true; }
    if (strcasecmp(name, "DOWN") == 0) { *key = GB_KEY_DOWN; return true; }
    if (strcasecmp(name, "LEFT") == 0) { *key = GB_KEY_LEFT; return true; }
    if (strcasecmp(name, "RIGHT") == 0) { *key = GB_KEY_RIGHT; return true; }
    return false;
}

static void cmd_press(const char *args)
{
    if (!gb_inited) {
        printf("ERR no ROM loaded\n");
        return;
    }

    char button_name[32] = {0};
    unsigned int hold_frames = 1;

    if (!args || !*args) {
        printf("ERR missing button name\n");
        return;
    }

    sscanf(args, "%31s %u", button_name, &hold_frames);
    if (hold_frames == 0) hold_frames = 1;

    GB_key_t key;
    if (!parse_button(button_name, &key)) {
        printf("ERR unknown button: %s\n", button_name);
        return;
    }

    GB_set_key_state(&gb, key, true);
    run_frames(hold_frames);
    GB_set_key_state(&gb, key, false);
    printf("OK\n");
}

static void cmd_release(const char *args)
{
    if (!gb_inited) {
        printf("ERR no ROM loaded\n");
        return;
    }

    if (!args || !*args) {
        printf("ERR missing button name\n");
        return;
    }

    char button_name[32] = {0};
    sscanf(args, "%31s", button_name);

    GB_key_t key;
    if (!parse_button(button_name, &key)) {
        printf("ERR unknown button: %s\n", button_name);
        return;
    }

    GB_set_key_state(&gb, key, false);
    printf("OK\n");
}

static void cmd_set_keys(const char *args)
{
    if (!gb_inited) {
        printf("ERR no ROM loaded\n");
        return;
    }

    if (!args || !*args) {
        printf("ERR missing key mask\n");
        return;
    }

    unsigned int mask = (unsigned int)strtoul(args, NULL, 0);
    GB_set_key_mask(&gb, (GB_key_mask_t)mask);
    printf("OK\n");
}

static void cmd_save_state(const char *args)
{
    if (!gb_inited) { printf("ERR no ROM loaded\n"); return; }
    while (args && *args == ' ') args++;
    if (!args || !*args) { printf("ERR missing file path\n"); return; }

    char path[1024];
    snprintf(path, sizeof(path), "%s", args);
    size_t len = strlen(path);
    while (len > 0 && (path[len-1] == ' ' || path[len-1] == '\n' || path[len-1] == '\r'))
        path[--len] = '\0';

    if (GB_save_state(&gb, path) == 0) {
        printf("OK\n");
    } else {
        printf("ERR failed to save state to %s\n", path);
    }
}

static void cmd_load_state(const char *args)
{
    if (!gb_inited) { printf("ERR no ROM loaded\n"); return; }
    while (args && *args == ' ') args++;
    if (!args || !*args) { printf("ERR missing file path\n"); return; }

    char path[1024];
    snprintf(path, sizeof(path), "%s", args);
    size_t len = strlen(path);
    while (len > 0 && (path[len-1] == ' ' || path[len-1] == '\n' || path[len-1] == '\r'))
        path[--len] = '\0';

    if (GB_load_state(&gb, path) == 0) {
        printf("OK\n");
    } else {
        printf("ERR failed to load state from %s\n", path);
    }
}

static void cmd_read_memory(const char *args)
{
    if (!gb_inited) { printf("ERR no ROM loaded\n"); return; }
    if (!args || !*args) { printf("ERR missing address\n"); return; }

    unsigned int addr = 0;
    unsigned int len = 1;
    sscanf(args, "%x %u", &addr, &len);
    if (len == 0) len = 1;
    if (len > 256) len = 256;

    printf("OK");
    for (unsigned int i = 0; i < len; i++) {
        printf(" %02X", GB_read_memory(&gb, (uint16_t)(addr + i)));
    }
    printf("\n");
}

static void cmd_write_memory(const char *args)
{
    if (!gb_inited) { printf("ERR no ROM loaded\n"); return; }
    if (!args || !*args) { printf("ERR missing address and data\n"); return; }

    unsigned int addr = 0;
    char *endptr = NULL;
    addr = (unsigned int)strtoul(args, &endptr, 16);

    while (endptr && *endptr == ' ') endptr++;
    if (!endptr || !*endptr) { printf("ERR missing data bytes\n"); return; }

    /* Parse hex bytes */
    char *p = endptr;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        unsigned int byte;
        if (sscanf(p, "%2x", &byte) != 1) break;
        GB_write_memory(&gb, (uint16_t)addr, (uint8_t)byte);
        addr++;
        p += 2;
    }
    printf("OK\n");
}

static void cmd_registers(void)
{
    if (!gb_inited) { printf("ERR no ROM loaded\n"); return; }

    GB_registers_t *regs = GB_get_registers(&gb);
    printf("OK AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X PC=%04X\n",
           regs->registers[GB_REGISTER_AF],
           regs->registers[GB_REGISTER_BC],
           regs->registers[GB_REGISTER_DE],
           regs->registers[GB_REGISTER_HL],
           regs->registers[GB_REGISTER_SP],
           regs->registers[GB_REGISTER_PC]);
}

static void cmd_screenshot(const char *args)
{
    if (!gb_inited) {
        printf("ERR no ROM loaded\n");
        return;
    }

    while (*args == ' ') args++;
    if (*args == '\0') {
        printf("ERR missing file path\n");
        return;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s", args);
    size_t len = strlen(path);
    while (len > 0 && (path[len-1] == ' ' || path[len-1] == '\n' || path[len-1] == '\r')) {
        path[--len] = '\0';
    }

    unsigned w = GB_get_screen_width(&gb);
    unsigned h = GB_get_screen_height(&gb);

    if (save_screenshot(path, w, h, pixel_buffer)) {
        printf("OK %ux%u\n", w, h);
    } else {
        printf("ERR failed to save screenshot to %s\n", path);
    }
}

static void cmd_screen_hash(void)
{
    if (!gb_inited) {
        printf("ERR no ROM loaded\n");
        return;
    }

    unsigned w = GB_get_screen_width(&gb);
    unsigned h = GB_get_screen_height(&gb);
    size_t data_size = w * h * sizeof(uint32_t);

    unsigned char hash[SHA256_DIGEST_LENGTH];
    sha256(pixel_buffer, data_size, hash);

    printf("OK ");
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        printf("%02x", hash[i]);
    }
    printf("\n");
}

static void cmd_set(const char *args)
{
    if (!args || !*args) {
        printf("ERR missing setting name\n");
        return;
    }

    char setting[32] = {0};
    char value[64] = {0};
    sscanf(args, "%31s %63s", setting, value);

    if (strcmp(setting, "turbo") == 0) {
        if (!gb_inited) { printf("ERR no ROM loaded\n"); return; }
        if (strcasecmp(value, "on") == 0) {
            GB_set_turbo_mode(&gb, true, true);
        } else if (strcasecmp(value, "off") == 0) {
            GB_set_turbo_mode(&gb, false, false);
        } else {
            printf("ERR turbo value must be 'on' or 'off'\n");
            return;
        }
        printf("OK\n");
    } else if (strcmp(setting, "model") == 0) {
        current_model = parse_model(value);
        printf("OK\n");
    } else if (strcmp(setting, "rendering") == 0) {
        if (!gb_inited) { printf("ERR no ROM loaded\n"); return; }
        if (strcasecmp(value, "on") == 0) {
            GB_set_rendering_disabled(&gb, false);
        } else if (strcasecmp(value, "off") == 0) {
            GB_set_rendering_disabled(&gb, true);
        } else {
            printf("ERR rendering value must be 'on' or 'off'\n");
            return;
        }
        printf("OK\n");
    } else {
        printf("ERR unknown setting: %s\n", setting);
    }
}

static void cmd_perf_frame(void)
{
    if (!gb_inited) { printf("ERR no ROM loaded\n"); return; }

    double usage = GB_debugger_get_frame_cpu_usage(&gb);
    printf("OK cpu_usage=%.4f busy=%u idle=%u\n",
           usage,
           gb.last_frame_busy_cycles,
           gb.last_frame_idle_cycles);
}

/* Main REPL loop */
static void repl(void)
{
    char line[4096];

    while (fgets(line, sizeof(line), stdin)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        /* Skip empty lines */
        if (len == 0) continue;

        /* Parse command and arguments */
        char *cmd = line;
        char *args = NULL;

        /* Find first space to separate command from args */
        char *space = strchr(line, ' ');
        if (space) {
            *space = '\0';
            args = space + 1;
            /* Skip leading whitespace in args */
            while (*args == ' ') args++;
        }

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        } else if (strcmp(cmd, "load") == 0) {
            cmd_load(args ? args : "");
        } else if (strcmp(cmd, "reset") == 0) {
            cmd_reset();
        } else if (strcmp(cmd, "run") == 0) {
            cmd_run(args);
        } else if (strcmp(cmd, "press") == 0) {
            cmd_press(args);
        } else if (strcmp(cmd, "release") == 0) {
            cmd_release(args);
        } else if (strcmp(cmd, "set_keys") == 0) {
            cmd_set_keys(args);
        } else if (strcmp(cmd, "set") == 0) {
            cmd_set(args);
        } else if (strcmp(cmd, "save_state") == 0) {
            cmd_save_state(args);
        } else if (strcmp(cmd, "load_state") == 0) {
            cmd_load_state(args);
        } else if (strcmp(cmd, "read_memory") == 0) {
            cmd_read_memory(args);
        } else if (strcmp(cmd, "write_memory") == 0) {
            cmd_write_memory(args);
        } else if (strcmp(cmd, "registers") == 0) {
            cmd_registers();
        } else if (strcmp(cmd, "screenshot") == 0) {
            cmd_screenshot(args ? args : "");
        } else if (strcmp(cmd, "perf_frame") == 0) {
            cmd_perf_frame();
        } else if (strcmp(cmd, "screen_hash") == 0) {
            cmd_screen_hash();
        } else {
            printf("ERR unknown command: %s\n", cmd);
        }

        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    fprintf(stderr, "SameBoy Agent Tester v" GB_VERSION "\n");

    const char *rom_path = NULL;

    GB_random_set_enabled(false);

    /* Force line buffering on stdout for agent communication */
    setvbuf(stdout, NULL, _IOLBF, 0);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            return 0;
        }
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            current_model = parse_model(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--boot") == 0 && i + 1 < argc) {
            boot_rom_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--interactive") == 0) {
            continue; /* Default mode */
        }
        if (argv[i][0] != '-') {
            rom_path = argv[i];
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        fprintf(stderr, "Usage: %s [--interactive] [--model <DMG|CGB|AGB|SGB>] [--boot <path>] [rom]\n", argv[0]);
        return 1;
    }

    /* If ROM provided on command line, load it before entering REPL */
    if (rom_path) {
        if (!init_and_load(rom_path)) {
            return 1;
        }
        fprintf(stderr, "ROM loaded: %s\n", rom_path);
    }

    fprintf(stderr, "Entering interactive mode. Type 'quit' to exit.\n");
    repl();

    if (gb_inited) {
        GB_free(&gb);
    }
    return 0;
}
