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
#include "cJSON.h"

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

/* Profiling state */
#define MAX_PROFILE_FRAMES 36000  /* 10 minutes at 60fps */
typedef struct {
    double cpu_usage;
    uint32_t busy_cycles;
    uint32_t idle_cycles;
    bool screen_changed;
} frame_perf_t;

static bool profiling_active = false;
static frame_perf_t *profile_data = NULL;
static unsigned int profile_frame_count = 0;
static unsigned char prev_screen_hash[32] = {0};
static bool prev_hash_valid = false;

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

static void compute_screen_hash(unsigned char *out)
{
    unsigned w = GB_get_screen_width(&gb);
    unsigned h = GB_get_screen_height(&gb);
    sha256(pixel_buffer, w * h * sizeof(uint32_t), out);
}

static void screen_hash_hex(char *out, size_t out_size)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    compute_screen_hash(hash);
    for (int i = 0; i < SHA256_DIGEST_LENGTH && (size_t)(i * 2 + 2) < out_size; i++) {
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    }
}

static bool is_screen_blank(void)
{
    unsigned w = GB_get_screen_width(&gb);
    unsigned h = GB_get_screen_height(&gb);
    for (unsigned i = 1; i < w * h; i++) {
        if (pixel_buffer[i] != pixel_buffer[0]) return false;
    }
    return true;
}

/* Run N frames, return total cycle count */
static uint64_t run_frames(unsigned int n)
{
    uint64_t total_ns = 0;
    for (unsigned int i = 0; i < n; i++) {
        total_ns += GB_run_frame(&gb);

        if (profiling_active && profile_frame_count < MAX_PROFILE_FRAMES) {
            frame_perf_t *f = &profile_data[profile_frame_count];
            f->cpu_usage = GB_debugger_get_frame_cpu_usage(&gb);
            f->busy_cycles = gb.last_frame_busy_cycles;
            f->idle_cycles = gb.last_frame_idle_cycles;

            unsigned char cur_hash[32];
            compute_screen_hash(cur_hash);
            f->screen_changed = !prev_hash_valid || memcmp(cur_hash, prev_screen_hash, 32) != 0;
            memcpy(prev_screen_hash, cur_hash, 32);
            prev_hash_valid = true;

            profile_frame_count++;
        }
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

static void cmd_perf_start(void)
{
    if (!gb_inited) { printf("ERR no ROM loaded\n"); return; }

    if (!profile_data) {
        profile_data = malloc(MAX_PROFILE_FRAMES * sizeof(frame_perf_t));
        if (!profile_data) { printf("ERR failed to allocate profiling buffer\n"); return; }
    }

    profile_frame_count = 0;
    prev_hash_valid = false;
    profiling_active = true;
    printf("OK\n");
}

static void cmd_perf_stop(void)
{
    if (!profiling_active) { printf("ERR profiling not active\n"); return; }

    profiling_active = false;

    if (profile_frame_count == 0) {
        printf("OK {\"total_frames\":0}\n");
        return;
    }

    /* Compute summary stats */
    double sum_usage = 0, min_usage = 1.0, max_usage = 0.0;
    unsigned int histogram[5] = {0}; /* 0-50, 50-75, 75-90, 90-95, 95-100 */

    for (unsigned int i = 0; i < profile_frame_count; i++) {
        double u = profile_data[i].cpu_usage;
        sum_usage += u;
        if (u < min_usage) min_usage = u;
        if (u > max_usage) max_usage = u;

        if (u < 0.50) histogram[0]++;
        else if (u < 0.75) histogram[1]++;
        else if (u < 0.90) histogram[2]++;
        else if (u < 0.95) histogram[3]++;
        else histogram[4]++;
    }

    double avg_usage = sum_usage / profile_frame_count;

    /* Detect slowdown events: consecutive frames with >95% CPU and unchanged screen */
    printf("OK {\"total_frames\":%u,\"avg_cpu_usage\":%.4f,\"min_cpu_usage\":%.4f,\"max_cpu_usage\":%.4f,"
           "\"cpu_usage_histogram\":{\"0-50%%\":%u,\"50-75%%\":%u,\"75-90%%\":%u,\"90-95%%\":%u,\"95-100%%\":%u},"
           "\"slowdown_events\":[",
           profile_frame_count, avg_usage, min_usage, max_usage,
           histogram[0], histogram[1], histogram[2], histogram[3], histogram[4]);

    /* Slowdown detection */
    bool in_slowdown = false;
    unsigned int slowdown_start = 0;
    double slowdown_usage_sum = 0;
    unsigned int slowdown_unchanged = 0;
    bool first_event = true;

    for (unsigned int i = 0; i < profile_frame_count; i++) {
        bool is_slow = profile_data[i].cpu_usage >= 0.95;
        bool unchanged = !profile_data[i].screen_changed;

        if (is_slow) {
            if (!in_slowdown) {
                in_slowdown = true;
                slowdown_start = i;
                slowdown_usage_sum = 0;
                slowdown_unchanged = 0;
            }
            slowdown_usage_sum += profile_data[i].cpu_usage;
            if (unchanged) slowdown_unchanged++;
        }

        if ((!is_slow || i == profile_frame_count - 1) && in_slowdown) {
            unsigned int end = is_slow ? i + 1 : i;
            unsigned int duration = end - slowdown_start;
            /* Only report if at least 2 frames and some unchanged screens */
            if (duration >= 2 && slowdown_unchanged >= 1) {
                if (!first_event) printf(",");
                printf("{\"start_frame\":%u,\"end_frame\":%u,\"duration_frames\":%u,"
                       "\"avg_cpu_usage\":%.4f,\"screen_unchanged_frames\":%u}",
                       slowdown_start, end, duration,
                       slowdown_usage_sum / duration, slowdown_unchanged);
                first_event = false;
            }
            in_slowdown = false;
        }
    }

    printf("]}\n");
}

/* Build perf summary as cJSON object */
static cJSON *build_perf_summary(void)
{
    cJSON *perf = cJSON_CreateObject();
    cJSON_AddNumberToObject(perf, "total_frames", profile_frame_count);

    if (profile_frame_count == 0) return perf;

    double sum = 0, min_u = 1.0, max_u = 0.0;
    unsigned int hist[5] = {0};
    for (unsigned int i = 0; i < profile_frame_count; i++) {
        double u = profile_data[i].cpu_usage;
        sum += u;
        if (u < min_u) min_u = u;
        if (u > max_u) max_u = u;
        if (u < 0.50) hist[0]++;
        else if (u < 0.75) hist[1]++;
        else if (u < 0.90) hist[2]++;
        else if (u < 0.95) hist[3]++;
        else hist[4]++;
    }
    cJSON_AddNumberToObject(perf, "avg_cpu_usage", sum / profile_frame_count);
    cJSON_AddNumberToObject(perf, "min_cpu_usage", min_u);
    cJSON_AddNumberToObject(perf, "max_cpu_usage", max_u);

    cJSON *histogram = cJSON_CreateObject();
    cJSON_AddNumberToObject(histogram, "0-50%", hist[0]);
    cJSON_AddNumberToObject(histogram, "50-75%", hist[1]);
    cJSON_AddNumberToObject(histogram, "75-90%", hist[2]);
    cJSON_AddNumberToObject(histogram, "90-95%", hist[3]);
    cJSON_AddNumberToObject(histogram, "95-100%", hist[4]);
    cJSON_AddItemToObject(perf, "cpu_usage_histogram", histogram);

    /* Slowdown events */
    cJSON *events = cJSON_CreateArray();
    bool in_slow = false;
    unsigned int s_start = 0;
    double s_sum = 0;
    unsigned int s_unchanged = 0;
    for (unsigned int i = 0; i <= profile_frame_count; i++) {
        bool is_slow = (i < profile_frame_count) && profile_data[i].cpu_usage >= 0.95;
        if (is_slow) {
            if (!in_slow) { in_slow = true; s_start = i; s_sum = 0; s_unchanged = 0; }
            s_sum += profile_data[i].cpu_usage;
            if (!profile_data[i].screen_changed) s_unchanged++;
        }
        if ((!is_slow || i == profile_frame_count) && in_slow) {
            unsigned int dur = i - s_start;
            if (dur >= 2 && s_unchanged >= 1) {
                cJSON *ev = cJSON_CreateObject();
                cJSON_AddNumberToObject(ev, "start_frame", s_start);
                cJSON_AddNumberToObject(ev, "end_frame", i);
                cJSON_AddNumberToObject(ev, "duration_frames", dur);
                cJSON_AddNumberToObject(ev, "avg_cpu_usage", s_sum / dur);
                cJSON_AddNumberToObject(ev, "screen_unchanged_frames", s_unchanged);
                cJSON_AddItemToArray(events, ev);
            }
            in_slow = false;
        }
    }
    cJSON_AddItemToObject(perf, "slowdown_events", events);
    return perf;
}

/* Script mode runner */
static int run_script(const char *script_path, const char *output_path)
{
    /* Read script file */
    FILE *f = fopen(script_path, "r");
    if (!f) {
        fprintf(stderr, "ERR: cannot open script: %s\n", script_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *script_text = malloc(fsize + 1);
    fread(script_text, 1, fsize, f);
    script_text[fsize] = '\0';
    fclose(f);

    cJSON *script = cJSON_Parse(script_text);
    free(script_text);
    if (!script) {
        fprintf(stderr, "ERR: invalid JSON in script\n");
        return 1;
    }

    const char *rom = cJSON_GetStringValue(cJSON_GetObjectItem(script, "rom"));
    cJSON *model_item = cJSON_GetObjectItem(script, "model");
    if (model_item && cJSON_IsString(model_item)) {
        current_model = parse_model(model_item->valuestring);
    }

    cJSON *tests = cJSON_GetObjectItem(script, "tests");
    if (!cJSON_IsArray(tests)) {
        fprintf(stderr, "ERR: script must contain a 'tests' array\n");
        cJSON_Delete(script);
        return 1;
    }

    /* Results */
    cJSON *results_root = cJSON_CreateObject();
    if (rom) cJSON_AddStringToObject(results_root, "rom", rom);
    cJSON *results_arr = cJSON_CreateArray();
    bool all_pass = true;

    cJSON *test;
    cJSON_ArrayForEach(test, tests) {
        const char *test_name = cJSON_GetStringValue(cJSON_GetObjectItem(test, "name"));
        if (!test_name) test_name = "unnamed";

        /* Per-test model override */
        cJSON *test_model = cJSON_GetObjectItem(test, "model");
        if (test_model && cJSON_IsString(test_model)) {
            current_model = parse_model(test_model->valuestring);
        }

        /* Load ROM (reload for each test for isolation) */
        if (rom && !init_and_load(rom)) {
            fprintf(stderr, "ERR: failed to load ROM '%s' for test '%s'\n", rom, test_name);
            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "name", test_name);
            cJSON_AddBoolToObject(result, "pass", false);
            cJSON_AddStringToObject(result, "error", "failed to load ROM");
            cJSON_AddItemToArray(results_arr, result);
            all_pass = false;
            continue;
        }

        /* Load save state if specified */
        cJSON *save_state_item = cJSON_GetObjectItem(test, "save_state");
        if (save_state_item && cJSON_IsString(save_state_item)) {
            if (GB_load_state(&gb, save_state_item->valuestring) != 0) {
                fprintf(stderr, "Warning: failed to load state '%s' for test '%s'\n",
                        save_state_item->valuestring, test_name);
            }
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "name", test_name);
        cJSON *screenshots = cJSON_CreateArray();
        cJSON *assertions = cJSON_CreateArray();
        bool test_pass = true;
        bool had_perf = false;

        /* Execute actions */
        cJSON *actions = cJSON_GetObjectItem(test, "actions");
        cJSON *action;
        cJSON_ArrayForEach(action, actions) {
            /* run N */
            cJSON *run_item = cJSON_GetObjectItem(action, "run");
            if (run_item && cJSON_IsNumber(run_item)) {
                run_frames((unsigned int)run_item->valuedouble);
                continue;
            }

            /* press BUTTON frames */
            cJSON *press_item = cJSON_GetObjectItem(action, "press");
            if (press_item && cJSON_IsString(press_item)) {
                GB_key_t key;
                if (parse_button(press_item->valuestring, &key)) {
                    unsigned int frames = 1;
                    cJSON *frames_item = cJSON_GetObjectItem(action, "frames");
                    if (frames_item && cJSON_IsNumber(frames_item))
                        frames = (unsigned int)frames_item->valuedouble;
                    GB_set_key_state(&gb, key, true);
                    run_frames(frames);
                    GB_set_key_state(&gb, key, false);
                }
                continue;
            }

            /* screenshot */
            cJSON *ss_item = cJSON_GetObjectItem(action, "screenshot");
            if (ss_item && cJSON_IsString(ss_item)) {
                unsigned w = GB_get_screen_width(&gb);
                unsigned h = GB_get_screen_height(&gb);
                save_screenshot(ss_item->valuestring, w, h, pixel_buffer);
                cJSON_AddItemToArray(screenshots, cJSON_CreateString(ss_item->valuestring));
                continue;
            }

            /* save_state */
            cJSON *ss_save = cJSON_GetObjectItem(action, "save_state");
            if (ss_save && cJSON_IsString(ss_save)) {
                GB_save_state(&gb, ss_save->valuestring);
                continue;
            }

            /* load_state */
            cJSON *ss_load = cJSON_GetObjectItem(action, "load_state");
            if (ss_load && cJSON_IsString(ss_load)) {
                GB_load_state(&gb, ss_load->valuestring);
                continue;
            }

            /* perf_start */
            if (cJSON_IsTrue(cJSON_GetObjectItem(action, "perf_start"))) {
                if (!profile_data) {
                    profile_data = malloc(MAX_PROFILE_FRAMES * sizeof(frame_perf_t));
                }
                profile_frame_count = 0;
                prev_hash_valid = false;
                profiling_active = true;
                continue;
            }

            /* perf_stop */
            if (cJSON_IsTrue(cJSON_GetObjectItem(action, "perf_stop"))) {
                profiling_active = false;
                had_perf = true;
                continue;
            }

            /* Assertions */

            /* assert_screen_not_blank */
            if (cJSON_IsTrue(cJSON_GetObjectItem(action, "assert_screen_not_blank"))) {
                bool blank = is_screen_blank();
                cJSON *a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "type", "screen_not_blank");
                cJSON_AddBoolToObject(a, "pass", !blank);
                cJSON_AddItemToArray(assertions, a);
                if (blank) test_pass = false;
                continue;
            }

            /* assert_screen_hash */
            cJSON *hash_item = cJSON_GetObjectItem(action, "assert_screen_hash");
            if (hash_item && cJSON_IsString(hash_item)) {
                char actual[65] = {0};
                screen_hash_hex(actual, sizeof(actual));
                bool match = strcasecmp(actual, hash_item->valuestring) == 0;
                cJSON *a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "type", "screen_hash");
                cJSON_AddStringToObject(a, "expected", hash_item->valuestring);
                cJSON_AddStringToObject(a, "actual", actual);
                cJSON_AddBoolToObject(a, "pass", match);
                cJSON_AddItemToArray(assertions, a);
                if (!match) test_pass = false;
                continue;
            }

            /* assert_max_cpu_usage */
            cJSON *max_cpu = cJSON_GetObjectItem(action, "assert_max_cpu_usage");
            if (max_cpu && cJSON_IsNumber(max_cpu)) {
                double threshold = max_cpu->valuedouble;
                double actual_max = 0;
                for (unsigned int i = 0; i < profile_frame_count; i++) {
                    if (profile_data[i].cpu_usage > actual_max)
                        actual_max = profile_data[i].cpu_usage;
                }
                bool pass = actual_max <= threshold;
                cJSON *a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "type", "max_cpu_usage");
                cJSON_AddNumberToObject(a, "threshold", threshold);
                cJSON_AddNumberToObject(a, "actual", actual_max);
                cJSON_AddBoolToObject(a, "pass", pass);
                cJSON_AddItemToArray(assertions, a);
                if (!pass) test_pass = false;
                continue;
            }

            /* assert_avg_cpu_usage */
            cJSON *avg_cpu = cJSON_GetObjectItem(action, "assert_avg_cpu_usage");
            if (avg_cpu && cJSON_IsNumber(avg_cpu)) {
                double threshold = avg_cpu->valuedouble;
                double sum = 0;
                for (unsigned int i = 0; i < profile_frame_count; i++)
                    sum += profile_data[i].cpu_usage;
                double actual_avg = profile_frame_count > 0 ? sum / profile_frame_count : 0;
                bool pass = actual_avg <= threshold;
                cJSON *a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "type", "avg_cpu_usage");
                cJSON_AddNumberToObject(a, "threshold", threshold);
                cJSON_AddNumberToObject(a, "actual", actual_avg);
                cJSON_AddBoolToObject(a, "pass", pass);
                cJSON_AddItemToArray(assertions, a);
                if (!pass) test_pass = false;
                continue;
            }

            /* assert_no_slowdown_events */
            if (cJSON_IsTrue(cJSON_GetObjectItem(action, "assert_no_slowdown_events"))) {
                /* Count slowdown events */
                unsigned int event_count = 0;
                bool in_slow = false;
                unsigned int s_start_local = 0, s_unchanged_local = 0;
                for (unsigned int i = 0; i <= profile_frame_count; i++) {
                    bool is_slow = (i < profile_frame_count) && profile_data[i].cpu_usage >= 0.95;
                    if (is_slow) {
                        if (!in_slow) { in_slow = true; s_start_local = i; s_unchanged_local = 0; }
                        if (!profile_data[i].screen_changed) s_unchanged_local++;
                    }
                    if ((!is_slow || i == profile_frame_count) && in_slow) {
                        if ((i - s_start_local) >= 2 && s_unchanged_local >= 1) event_count++;
                        in_slow = false;
                    }
                }
                cJSON *a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "type", "no_slowdown_events");
                cJSON_AddNumberToObject(a, "count", event_count);
                cJSON_AddBoolToObject(a, "pass", event_count == 0);
                cJSON_AddItemToArray(assertions, a);
                if (event_count > 0) test_pass = false;
                continue;
            }

            /* assert_memory */
            cJSON *mem_assert = cJSON_GetObjectItem(action, "assert_memory");
            if (mem_assert && cJSON_IsObject(mem_assert)) {
                const char *addr_str = cJSON_GetStringValue(cJSON_GetObjectItem(mem_assert, "address"));
                const char *expected_str = cJSON_GetStringValue(cJSON_GetObjectItem(mem_assert, "expected"));
                if (addr_str && expected_str) {
                    unsigned int addr = (unsigned int)strtoul(addr_str, NULL, 16);
                    size_t expected_len = strlen(expected_str) / 2;
                    char actual_hex[513] = {0};
                    bool match = true;
                    for (size_t j = 0; j < expected_len && j < 256; j++) {
                        uint8_t byte = GB_read_memory(&gb, (uint16_t)(addr + j));
                        snprintf(actual_hex + j * 2, 3, "%02X", byte);
                        unsigned int expected_byte;
                        sscanf(expected_str + j * 2, "%2x", &expected_byte);
                        if (byte != (uint8_t)expected_byte) match = false;
                    }
                    cJSON *a = cJSON_CreateObject();
                    cJSON_AddStringToObject(a, "type", "memory");
                    cJSON_AddStringToObject(a, "address", addr_str);
                    cJSON_AddStringToObject(a, "expected", expected_str);
                    cJSON_AddStringToObject(a, "actual", actual_hex);
                    cJSON_AddBoolToObject(a, "pass", match);
                    cJSON_AddItemToArray(assertions, a);
                    if (!match) test_pass = false;
                }
                continue;
            }
        }

        cJSON_AddBoolToObject(result, "pass", test_pass);
        if (cJSON_GetArraySize(screenshots) > 0)
            cJSON_AddItemToObject(result, "screenshots", screenshots);
        else
            cJSON_Delete(screenshots);
        if (cJSON_GetArraySize(assertions) > 0)
            cJSON_AddItemToObject(result, "assertions", assertions);
        else
            cJSON_Delete(assertions);
        if (had_perf)
            cJSON_AddItemToObject(result, "perf", build_perf_summary());
        cJSON_AddItemToArray(results_arr, result);
        if (!test_pass) all_pass = false;

        fprintf(stderr, "Test '%s': %s\n", test_name, test_pass ? "PASS" : "FAIL");
    }

    cJSON_AddBoolToObject(results_root, "pass", all_pass);
    cJSON_AddItemToObject(results_root, "results", results_arr);

    /* Write output */
    char *json_out = cJSON_Print(results_root);
    if (output_path) {
        FILE *out = fopen(output_path, "w");
        if (out) {
            fputs(json_out, out);
            fclose(out);
            fprintf(stderr, "Results written to %s\n", output_path);
        } else {
            fprintf(stderr, "ERR: cannot write to %s, printing to stdout\n", output_path);
            puts(json_out);
        }
    } else {
        puts(json_out);
    }

    free(json_out);
    cJSON_Delete(results_root);
    cJSON_Delete(script);
    return all_pass ? 0 : 1;
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
        } else if (strcmp(cmd, "perf_start") == 0) {
            cmd_perf_start();
        } else if (strcmp(cmd, "perf_stop") == 0) {
            cmd_perf_stop();
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
    const char *script_path = NULL;
    const char *output_path = NULL;

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
        if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            script_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
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
        fprintf(stderr, "Usage: %s [--interactive] [--script <path.json>] [--output <path.json>] [--model <DMG|CGB|AGB|SGB>] [--boot <path>] [rom]\n", argv[0]);
        return 1;
    }

    /* Script mode */
    if (script_path) {
        int ret = run_script(script_path, output_path);
        if (gb_inited) GB_free(&gb);
        return ret;
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
