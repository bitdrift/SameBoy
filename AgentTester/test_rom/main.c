/*
 * SameBoy Agent Test Harness — Test ROM
 *
 * A minimal GBDK ROM with 4 screens for testing the harness.
 * Navigate between screens with START.
 * Fully deterministic — no RNG, no RTC.
 */

#include <gb/gb.h>
#include <gbdk/console.h>
#include <gbdk/font.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Fixed RAM address for memory probe test */
#define PROBE_ADDR 0xC0A0

/* Screen IDs */
#define SCREEN_TITLE  0
#define SCREEN_INPUT  1
#define SCREEN_PERF   2
#define SCREEN_MEMORY 3
#define SCREEN_COUNT  4

static uint8_t current_screen = SCREEN_TITLE;
static uint8_t prev_joypad = 0;
static uint8_t perf_stress_on = 0;

/* Wait for a key to be newly pressed (not held from last frame) */
static uint8_t joypad_pressed(void) {
    uint8_t current = joypad();
    uint8_t pressed = current & ~prev_joypad;
    prev_joypad = current;
    return pressed;
}

/* Burn CPU cycles to simulate a heavy workload */
static void burn_cpu(void) {
    /* Tight loop that wastes most of the frame budget */
    volatile uint16_t i;
    for (i = 0; i < 8000; i++) {
        /* Busy work */
    }
}

static void draw_title(void) {
    gotoxy(2, 4);
    printf("HARNESS TEST ROM");
    gotoxy(2, 6);
    printf("v1.0");
    gotoxy(2, 10);
    printf("Press START to");
    gotoxy(2, 11);
    printf("cycle screens");
    gotoxy(2, 14);
    printf("Screen: TITLE");
}

static void draw_input(void) {
    gotoxy(2, 2);
    printf("INPUT ECHO TEST");
    gotoxy(2, 4);
    printf("Buttons held:");
    gotoxy(2, 14);
    printf("Screen: INPUT");
}

static void update_input_display(void) {
    uint8_t j = joypad();
    gotoxy(2, 6);
    printf("A:%c B:%c        ",
           (j & J_A) ? '1' : '0',
           (j & J_B) ? '1' : '0');
    gotoxy(2, 7);
    printf("U:%c D:%c L:%c R:%c",
           (j & J_UP) ? '1' : '0',
           (j & J_DOWN) ? '1' : '0',
           (j & J_LEFT) ? '1' : '0',
           (j & J_RIGHT) ? '1' : '0');
    gotoxy(2, 8);
    printf("SEL:%c STA:%c    ",
           (j & J_SELECT) ? '1' : '0',
           (j & J_START) ? '1' : '0');
}

static void draw_perf(void) {
    gotoxy(2, 2);
    printf("PERF STRESS TEST");
    gotoxy(2, 4);
    printf("Press A to toggle");
    gotoxy(2, 5);
    printf("CPU stress loop");
    gotoxy(2, 14);
    printf("Screen: PERF");
}

static void update_perf_display(void) {
    gotoxy(2, 8);
    if (perf_stress_on) {
        printf("Stress: ON ");
        burn_cpu();
    } else {
        printf("Stress: OFF");
    }
}

static void draw_memory(void) {
    /* Write known probe values */
    *((volatile uint8_t *)(PROBE_ADDR + 0)) = 0xDE;
    *((volatile uint8_t *)(PROBE_ADDR + 1)) = 0xAD;
    *((volatile uint8_t *)(PROBE_ADDR + 2)) = 0xBE;
    *((volatile uint8_t *)(PROBE_ADDR + 3)) = 0xEF;

    gotoxy(2, 2);
    printf("MEMORY PROBE TEST");
    gotoxy(2, 4);
    printf("Wrote DEADBEEF");
    gotoxy(2, 5);
    printf("at addr C0A0");
    gotoxy(2, 8);
    printf("Read back:");
    gotoxy(2, 9);
    printf("%02X %02X %02X %02X",
           *((volatile uint8_t *)(PROBE_ADDR + 0)),
           *((volatile uint8_t *)(PROBE_ADDR + 1)),
           *((volatile uint8_t *)(PROBE_ADDR + 2)),
           *((volatile uint8_t *)(PROBE_ADDR + 3)));
    gotoxy(2, 14);
    printf("Screen: MEMORY");
}

static void switch_screen(void) {
    current_screen = (current_screen + 1) % SCREEN_COUNT;
    cls();
    perf_stress_on = 0;
    switch (current_screen) {
        case SCREEN_TITLE:  draw_title(); break;
        case SCREEN_INPUT:  draw_input(); break;
        case SCREEN_PERF:   draw_perf(); break;
        case SCREEN_MEMORY: draw_memory(); break;
    }
}

void main(void) {
    font_init();
    font_set(font_load(font_ibm));

    cls();
    draw_title();

    while (1) {
        uint8_t pressed = joypad_pressed();

        /* START cycles to next screen */
        if (pressed & J_START) {
            switch_screen();
        }

        /* Per-screen updates */
        switch (current_screen) {
            case SCREEN_INPUT:
                update_input_display();
                break;
            case SCREEN_PERF:
                if (pressed & J_A) {
                    perf_stress_on = !perf_stress_on;
                }
                update_perf_display();
                break;
            default:
                break;
        }

        wait_vbl_done();
    }
}
