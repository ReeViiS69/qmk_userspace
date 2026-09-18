#pragma once

/*
 * Optional diagnostics / performance instrumentation.
 * Production builds keep all of these undefined.
 *
 * If SHARKOON_PERF_BENCHMARK is enabled for measurements, also switch
 * CONSOLE_ENABLE to yes in users/sharkoon_sgk50_s2/rules.mk.
 */
#define SHARKOON_PERF_BENCHMARK
#define DEBUG_MATRIX_SCAN_RATE
// #define WS2812_DIAGNOSTICS
// #define WS2812_DEBUG
/*
 * Storage layout for this confirmed SGK50 S2 hardware:
 * - WB32FQ95xC: 256 KiB internal flash / 36 KiB SRAM
 * - W25Q32:     4 MiB external SPI NOR flash
 *
 * Eight dynamic layers use 8 * 6 * 19 * 2 = 1824 bytes.
 * A 4096-byte logical EEPROM leaves 2272 bytes outside the dynamic keymap,
 * exactly twice the old 2048-byte / 4-layer remainder of 1136 bytes.
 * Wear leveling keeps its existing 2:1 backing/logical ratio.
 */
#undef EXTERNAL_FLASH_SIZE
#define EXTERNAL_FLASH_SIZE (4 * 1024 * 1024)

#undef WEAR_LEVELING_BACKING_SIZE
#define WEAR_LEVELING_BACKING_SIZE 8192

#undef WEAR_LEVELING_LOGICAL_SIZE
#define WEAR_LEVELING_LOGICAL_SIZE 4096

#undef DYNAMIC_KEYMAP_LAYER_COUNT
#define DYNAMIC_KEYMAP_LAYER_COUNT 8

#undef DYNAMIC_KEYMAP_EEPROM_MAX_ADDR
#define DYNAMIC_KEYMAP_EEPROM_MAX_ADDR 4095

/*
 * Keep NKRO enabled by default
 */
#define NKRO_DEFAULT_ON true

//enable socd for mousekeys aswell, custom setting, not a socd default
#define SOCD_CLEANER_MOUSEKEY_ENABLE

/*
 * Unicode typing uses WinCompose on Windows.
 * WinCompose must be installed on the host for Unicode output to work.
 */
#define UNICODE_SELECTED_MODES UNICODE_MODE_WINCOMPOSE
//#define TAP_CODE_DELAY 20
#define UNICODE_TYPE_DELAY 25

// ─────────────────────────────────────────────
// Space Cadet – deutsches Tastaturlayout
// Format:
// HOLD_MODIFIER, TAP_MODIFIER, TAP_KEY
// ─────────────────────────────────────────────

// Shift
// Tap: ( / )
// Hold: Left / Right Shift
#define LSPO_KEYS KC_LSFT, KC_LSFT, KC_8
#define RSPC_KEYS KC_RSFT, KC_RSFT, KC_9

// Ctrl
// Tap: { / }
// Hold: Left / Right Ctrl
#define LCPO_KEYS KC_LCTL, KC_RALT, KC_7
#define RCPC_KEYS KC_RCTL, KC_RALT, KC_0

// Alt / AltGr
// Tap: [ / ]
// Hold: Left Alt / Right Alt (AltGr)
#define LAPO_KEYS KC_LALT, KC_RALT, KC_8
#define RAPC_KEYS KC_RALT, KC_RALT, KC_9

/*
 * Replace QMK's stock Solid Reactive renderer while preserving the official
 * RGB_MATRIX_SOLID_REACTIVE mode ID.
 */
#define SOLID_REACTIVE SHARKOON_ORIGINAL_SOLID_REACTIVE
/*
 * Add and disable RGB effects to my preference.
 */

#define ENABLE_RGB_MATRIX_FLOWER_BLOOMING
#define ENABLE_RGB_MATRIX_STARLIGHT
#define ENABLE_RGB_MATRIX_STARLIGHT_SMOOTH
#define ENABLE_RGB_MATRIX_STARLIGHT_DUAL_HUE
#define ENABLE_RGB_MATRIX_STARLIGHT_DUAL_SAT
#define ENABLE_RGB_MATRIX_RIVERFLOW

#define PALETTEFX_PHOSPHOR_ENABLE

#define GRADIENT_UP_DOWN    SHARKOON_ORIGINAL_GRADIENT_UP_DOWN
#define GRADIENT_LEFT_RIGHT SHARKOON_ORIGINAL_GRADIENT_LEFT_RIGHT
#define TYPING_HEATMAP      SHARKOON_ORIGINAL_TYPING_HEATMAP
#define CYCLE_PINWHEEL      SHARKOON_ORIGINAL_CYCLE_PINWHEEL
#define CYCLE_SPIRAL        SHARKOON_ORIGINAL_CYCLE_SPIRAL
#define PIXEL_RAIN          SHARKOON_ORIGINAL_PIXEL_RAIN

