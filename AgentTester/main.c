#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <Core/gb.h>
#include <Core/random.h>

static GB_gameboy_t gb;
static uint32_t pixel_buffer[256 * 224];
static bool gb_inited = false;
static const char *boot_rom_path = NULL;

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
        /* Continue without boot ROM — not fatal */
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
        fprintf(stderr, "Failed to load ROM: %s\n", rom_path);
        GB_free(&gb);
        gb_inited = false;
        return false;
    }

    return true;
}

static model_t parse_model(const char *str)
{
    if (strcasecmp(str, "DMG") == 0) return MODEL_DMG;
    if (strcasecmp(str, "SGB") == 0) return MODEL_SGB;
    if (strcasecmp(str, "AGB") == 0) return MODEL_AGB;
    return MODEL_CGB;
}

int main(int argc, char **argv)
{
    fprintf(stderr, "SameBoy Agent Tester v" GB_VERSION "\n");

    const char *rom_path = NULL;

    GB_random_set_enabled(false);

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
            continue; /* Default mode, accepted but ignored for now */
        }
        if (argv[i][0] != '-') {
            rom_path = argv[i];
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        fprintf(stderr, "Usage: %s [--interactive] [--model <DMG|CGB|AGB|SGB>] [--boot <path>] [rom]\n", argv[0]);
        return 1;
    }

    if (!rom_path) {
        fprintf(stderr, "Usage: %s [--interactive] [--model <DMG|CGB|AGB|SGB>] [--boot <path>] <rom>\n", argv[0]);
        return 1;
    }

    if (!init_and_load(rom_path)) {
        return 1;
    }

    fprintf(stderr, "ROM loaded: %s\n", rom_path);

    /* Run 10 frames and report */
    uint64_t total_ns = 0;
    for (int i = 0; i < 10; i++) {
        total_ns += GB_run_frame(&gb);
    }
    fprintf(stderr, "Ran 10 frames: %llu ns total\n", (unsigned long long)total_ns);

    GB_free(&gb);
    return 0;
}
