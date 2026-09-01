// Copyright 2024 JoyLee (@itarze)
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H

#include "action_layer.h"
#include "color.h"
#include "dynamic_keymap.h"
#include "keycode.h"
#include "send_string_keycodes.h"
#include "timer.h"

#include <string.h>

#include "sharkoon_sgk50_s2.h"
#ifdef VIALRGB_ENABLE
#    include "via.h"
#    include "vialrgb.h"
#endif

#include "socd_cleaner.h"
socd_cleaner_t socd_opposing_pairs[] = {
    {{KC_A, KC_D}, SOCD_CLEANER_LAST},
};
#ifdef VIALRGB_DIRECT_ENABLE

static uint32_t sharkoon_direct_last_packet = 0;
static bool sharkoon_direct_seen = false;

bool via_command_kb(uint8_t *data, uint8_t length) {
    (void)length;

    if (data[0] == id_custom_set_value &&
        data[1] == vialrgb_direct_fastset) {

        sharkoon_direct_last_packet = timer_read32();
        sharkoon_direct_seen = true;
    }

    // Wichtig: Paket NICHT abfangen.
    // Vial soll es danach normal verarbeiten.
    return false;
}

bool sharkoon_direct_stream_active(void) {
    return sharkoon_direct_seen &&
           timer_elapsed32(sharkoon_direct_last_packet) < 250;
}

#endif
#define SHARKOON_MACRO_MAX_ACTIONS      16
#define SHARKOON_MACRO_EVENT_SIZE       3
#define SHARKOON_MACRO_RECORDING_SIZE   (SHARKOON_MACRO_MAX_ACTIONS * 2 * SHARKOON_MACRO_EVENT_SIZE)
#define SHARKOON_VIA_MACRO_BUFFER_MAX   4096

#define SHARKOON_MACRO_SELECT_LAYER     2
#define SHARKOON_MACRO_LED_BLINK_MS     400

enum custom_keycodes {
    REC_M0 = QK_USER_0,
    REC_M1 = QK_USER_1,
    REC_STOP = QK_USER_2,
};

static bool    sharkoon_recording              = false;
static bool    sharkoon_accept_new_presses     = false;
static uint8_t sharkoon_recording_slot          = 0;
static uint8_t sharkoon_action_count            = 0;
static uint8_t sharkoon_recording_length        = 0;
static uint8_t sharkoon_pressed_count           = 0;
static uint8_t sharkoon_recording_data[SHARKOON_MACRO_RECORDING_SIZE];
static uint8_t sharkoon_recorded_key[MATRIX_ROWS][MATRIX_COLS];
static bool    sharkoon_suppress_release[MATRIX_ROWS][MATRIX_COLS];
static uint8_t sharkoon_via_macro_buffer[SHARKOON_VIA_MACRO_BUFFER_MAX];

#ifdef RGB_MATRIX_ENABLE
static bool          sharkoon_macro_select_led_active    = false;
static bool          sharkoon_macro_select_blink_on      = false;
static uint16_t      sharkoon_macro_select_led_timer     = 0;
static layer_state_t sharkoon_macro_select_cached_layers = 0;
static uint8_t       sharkoon_macro_select_led_count     = 0;
static uint8_t       sharkoon_macro_select_leds[MATRIX_ROWS * MATRIX_COLS];
static uint8_t       sharkoon_recording_led              = NO_LED;
static uint8_t       sharkoon_caps_lock_led              = NO_LED;
static bool          sharkoon_caps_lock_active           = false;

void keyboard_post_init_user(void) {
    sharkoon_apply_disabled_led_flags();
}
#endif

static bool sharkoon_append_event(uint8_t event_type, uint8_t keycode) {
    if ((uint16_t)(sharkoon_recording_length + SHARKOON_MACRO_EVENT_SIZE) > (uint16_t)sizeof(sharkoon_recording_data)) {
        return false;
    }

    sharkoon_recording_data[sharkoon_recording_length++] = SS_QMK_PREFIX;
    sharkoon_recording_data[sharkoon_recording_length++] = event_type;
    sharkoon_recording_data[sharkoon_recording_length++] = keycode;
    return true;
}

static void sharkoon_start_recording(uint8_t slot, uint8_t row, uint8_t col) {
    sharkoon_recording          = true;
    sharkoon_accept_new_presses = true;
    sharkoon_recording_slot     = slot;
    sharkoon_action_count       = 0;
    sharkoon_recording_length   = 0;
    sharkoon_pressed_count      = 0;

#ifdef RGB_MATRIX_ENABLE
    // Cache the LED that actually started the recording. This follows REC_M0/
    // REC_M1 when they are moved in Vial and avoids row/column lookup per frame.
    sharkoon_recording_led          = g_led_config.matrix_co[row][col];
    sharkoon_macro_select_blink_on  = true;
    sharkoon_macro_select_led_timer = timer_read();
#endif

    memset(sharkoon_recorded_key, 0, sizeof(sharkoon_recorded_key));
}

static bool sharkoon_find_macro_bounds(uint8_t slot, uint16_t buffer_size, uint16_t *macro_start, uint16_t *macro_end, uint16_t *used_end) {
    const uint8_t macro_count = dynamic_keymap_macro_get_count();

    if (slot >= macro_count || buffer_size == 0) {
        return false;
    }

    uint8_t  current_macro   = 0;
    uint16_t current_start   = 0;
    uint16_t last_terminator = 0;
    bool     found_target    = false;

    for (uint16_t i = 0; i < buffer_size; ++i) {
        if (sharkoon_via_macro_buffer[i] != 0) {
            continue;
        }

        last_terminator = i + 1;

        if (current_macro == slot) {
            *macro_start = current_start;
            *macro_end   = i;
            found_target = true;
        }

        ++current_macro;
        current_start = i + 1;

        if (current_macro >= macro_count) {
            break;
        }
    }

    if (!found_target) {
        return false;
    }

    *used_end = last_terminator;
    return true;
}

static void sharkoon_save_recording(void) {
    const uint16_t buffer_size = dynamic_keymap_macro_get_buffer_size();

    if (buffer_size == 0 || buffer_size > SHARKOON_VIA_MACRO_BUFFER_MAX) {
        return;
    }

    dynamic_keymap_macro_get_buffer(0, buffer_size, sharkoon_via_macro_buffer);

    // QMK/VIA uses the last byte as a valid flag. Do not overwrite a buffer
    // that is already marked as being in the middle of an interrupted write.
    if (sharkoon_via_macro_buffer[buffer_size - 1] != 0) {
        return;
    }

    uint16_t macro_start = 0;
    uint16_t macro_end   = 0;
    uint16_t used_end    = 0;

    if (!sharkoon_find_macro_bounds(sharkoon_recording_slot, buffer_size, &macro_start, &macro_end, &used_end)) {
        return;
    }

    const uint16_t old_macro_size = (macro_end - macro_start) + 1; // includes terminating NUL
    const uint16_t new_macro_size = (uint16_t)sharkoon_recording_length + 1;
    const int32_t  size_delta     = (int32_t)new_macro_size - (int32_t)old_macro_size;
    const uint16_t tail_start     = macro_end + 1;
    const uint16_t tail_size      = used_end - tail_start;

    if (size_delta > 0 && (uint32_t)used_end + (uint32_t)size_delta > buffer_size) {
        return;
    }

    const uint16_t new_tail_start = (uint16_t)((int32_t)tail_start + size_delta);

    if (tail_size > 0 && new_tail_start != tail_start) {
        memmove(&sharkoon_via_macro_buffer[new_tail_start], &sharkoon_via_macro_buffer[tail_start], tail_size);
    }

    if (sharkoon_recording_length > 0) {
        memcpy(&sharkoon_via_macro_buffer[macro_start], sharkoon_recording_data, sharkoon_recording_length);
    }
    sharkoon_via_macro_buffer[macro_start + sharkoon_recording_length] = 0;

    const uint16_t new_used_end = (uint16_t)((int32_t)used_end + size_delta);
    const uint16_t write_end    = new_used_end > used_end ? new_used_end : used_end;

    if (new_used_end < used_end) {
        memset(&sharkoon_via_macro_buffer[new_used_end], 0, used_end - new_used_end);
    }

    // Follow dynamic_keymap.h's validity protocol: invalidate first, write the
    // changed macro area, then mark the complete buffer valid again.
    uint8_t valid_flag = 0xFF;
    dynamic_keymap_macro_set_buffer(buffer_size - 1, 1, &valid_flag);

    const uint16_t data_write_end = write_end < (buffer_size - 1) ? write_end : (buffer_size - 1);
    if (data_write_end > macro_start) {
        dynamic_keymap_macro_set_buffer(macro_start, data_write_end - macro_start, &sharkoon_via_macro_buffer[macro_start]);
    }

    valid_flag = 0;
    dynamic_keymap_macro_set_buffer(buffer_size - 1, 1, &valid_flag);
}

static void sharkoon_release_recorded_keys(void) {
    for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
        for (uint8_t col = 0; col < MATRIX_COLS; ++col) {
            const uint8_t keycode = sharkoon_recorded_key[row][col];
            if (keycode == 0) {
                continue;
            }

            sharkoon_append_event(SS_UP_CODE, keycode);
            sharkoon_recorded_key[row][col] = 0;
        }
    }

    sharkoon_pressed_count = 0;
}

static void sharkoon_finish_recording(bool force_release) {
    if (!sharkoon_recording) {
        return;
    }

    if (force_release) {
        sharkoon_release_recorded_keys();
    }

    sharkoon_save_recording();

    sharkoon_recording          = false;
    sharkoon_accept_new_presses = false;
#ifdef RGB_MATRIX_ENABLE
    // Effects ignore flags==0 LEDs, so explicitly clear an ignored recording
    // LED before dropping the cached index. Normal effect LEDs redraw themselves.
    if (sharkoon_recording_led != NO_LED && g_led_config.flags[sharkoon_recording_led] == 0) {
        rgb_matrix_set_color(sharkoon_recording_led, 0, 0, 0);
    }
    sharkoon_recording_led = NO_LED;
#endif
}

static void sharkoon_consume_active_oneshot_layer(void) {
    if (!is_oneshot_layer_active()) {
        return;
    }

    // reset_oneshot_layer() only clears QMK's one-shot bookkeeping.
    // The actual layer bit must be switched off separately, while the
    // one-shot layer number is still available.
    const uint8_t oneshot_layer = get_oneshot_layer();
    layer_off(oneshot_layer);
    reset_oneshot_layer();
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    const uint8_t row = record->event.key.row;
    const uint8_t col = record->event.key.col;

#ifdef RGB_MATRIX_ENABLE
    // Follow KC_CAPS when it is moved in Vial. Cache the LED from the actual
    // physical key event so the RGB indicator never has to scan the matrix.
    if (record->event.pressed && keycode == KC_CAPS) {
        const uint8_t new_caps_led = g_led_config.matrix_co[row][col];

        // Effects never redraw flags==0 LEDs. If KC_CAPS was moved while an
        // ignored LED was still used as the indicator, clear that old LED once.
        if (sharkoon_caps_lock_led != NO_LED && sharkoon_caps_lock_led != new_caps_led && g_led_config.flags[sharkoon_caps_lock_led] == 0) {
            rgb_matrix_set_color(sharkoon_caps_lock_led, 0, 0, 0);
        }

        sharkoon_caps_lock_led = new_caps_led;
    }
#endif

    // A custom recorder key can resolve to a different keycode on release after
    // its one-shot/layer state has changed. Suppress that physical release by
    // matrix position so no stray F9/F10/Caps release can escape to the host.
    if (!record->event.pressed && sharkoon_suppress_release[row][col]) {
        sharkoon_suppress_release[row][col] = false;
        return false;
    }

    switch (keycode) {
        case REC_M0:
        case REC_M1:
            sharkoon_suppress_release[row][col] = true;
            if (record->event.pressed) {
                // REC_M0/REC_M1 may be placed on any layer. If the keycode
                // was reached through an active one-shot layer (our default is
                // OSL(2)), consume that one-shot completely before recording.
                sharkoon_consume_active_oneshot_layer();

                if (!sharkoon_recording) {
                    sharkoon_start_recording(keycode == REC_M0 ? 0 : 1, row, col);
                }
            }
            return false;

        case REC_STOP:
            sharkoon_suppress_release[row][col] = true;
            if (record->event.pressed && sharkoon_recording) {
                sharkoon_finish_recording(true);
            }
            return false;

        default:
            break;
    }

    if (!sharkoon_recording) {
        return true;
    }

    // VIA macro playback keys are controls, not recordable input. Swallow them
    // while recording so a stored macro cannot recursively play into a capture.
    if (keycode >= QK_MACRO && keycode <= QK_MACRO_MAX) {
        if (record->event.pressed) {
            sharkoon_suppress_release[row][col] = true;
        }
        return false;
    }

    if (record->event.pressed) {
        // Only 8-bit keycodes can be represented by QMK's send-string DOWN/UP
        // commands. Layer keys and other 16-bit QMK controls therefore pass
        // through normally but are intentionally not recorded.
        if (sharkoon_accept_new_presses && IS_ANY(keycode) && sharkoon_recorded_key[row][col] == 0) {
            const uint8_t basic_keycode = (uint8_t)keycode;

            if (sharkoon_append_event(SS_DOWN_CODE, basic_keycode)) {
                sharkoon_recorded_key[row][col] = basic_keycode;
                ++sharkoon_pressed_count;
                ++sharkoon_action_count;

                if (sharkoon_action_count >= SHARKOON_MACRO_MAX_ACTIONS) {
                    // No 17th action is accepted. The recording is saved as soon
                    // as every key belonging to the first 16 actions is released.
                    sharkoon_accept_new_presses = false;
                }
            }
        }

        return true;
    }

    const uint8_t recorded_keycode = sharkoon_recorded_key[row][col];
    if (recorded_keycode != 0) {
        sharkoon_append_event(SS_UP_CODE, recorded_keycode);
        sharkoon_recorded_key[row][col] = 0;

        --sharkoon_pressed_count;

        if (!sharkoon_accept_new_presses && sharkoon_pressed_count == 0) {
            sharkoon_finish_recording(false);
        }
    }

    return true;
}

#ifdef RGB_MATRIX_ENABLE
static void sharkoon_clear_ignored_select_leds(void) {
    for (uint8_t i = 0; i < sharkoon_macro_select_led_count; ++i) {
        const uint8_t led = sharkoon_macro_select_leds[i];
        if (g_led_config.flags[led] == 0) {
            rgb_matrix_set_color(led, 0, 0, 0);
        }
    }
}

static void sharkoon_refresh_macro_select_leds(layer_state_t active_layers) {
    // Clear only previously touched LEDs that normal effects intentionally ignore.
    // This avoids scanning every flags==0 LED on every RGB frame.
    sharkoon_clear_ignored_select_leds();
    sharkoon_macro_select_led_count = 0;

    for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
        for (uint8_t col = 0; col < MATRIX_COLS; ++col) {
            const keypos_t key   = {.row = row, .col = col};
            const uint8_t  layer = layer_switch_get_layer(key);
            const uint16_t kc    = dynamic_keymap_get_keycode(layer, row, col);

            if (kc != REC_M0 && kc != REC_M1) {
                continue;
            }

            const uint8_t led = g_led_config.matrix_co[row][col];
            if (led != NO_LED) {
                sharkoon_macro_select_leds[sharkoon_macro_select_led_count++] = led;
            }
        }
    }

    sharkoon_macro_select_cached_layers = active_layers;
}

void oneshot_layer_changed_user(uint8_t layer) {
    if (layer == SHARKOON_MACRO_SELECT_LAYER) {
        // Enter the macro selector once, when QMK activates OSL(2).
        sharkoon_macro_select_led_active = true;
        sharkoon_macro_select_blink_on   = true;
        sharkoon_macro_select_led_timer  = timer_read();
        sharkoon_refresh_macro_select_leds(layer_state | default_layer_state);
        return;
    }

    if (sharkoon_macro_select_led_active) {
        // QMK reports layer 0 when the one-shot layer is consumed/reset. LEDs
        // ignored by normal effects must be cleared explicitly once on exit.
        sharkoon_clear_ignored_select_leds();
        sharkoon_macro_select_led_active = false;
        sharkoon_macro_select_led_count  = 0;
    }
}

bool led_update_user(led_t led_state) {
    // Cache the host Caps state only when QMK reports a lock-LED change.
    sharkoon_caps_lock_active = led_state.caps_lock;

    // Normal RGB effects restore regular LEDs automatically. LEDs with flags==0
    // are intentionally ignored, so clear a Caps overlay there once Caps turns off.
    if (!sharkoon_caps_lock_active) {
        const uint8_t caps_led = sharkoon_caps_lock_led != NO_LED ? sharkoon_caps_lock_led : g_led_config.matrix_co[3][0];
        if (caps_led != NO_LED && g_led_config.flags[caps_led] == 0) {
            rgb_matrix_set_color(caps_led, 0, 0, 0);
        }
    }

    return true;
}

bool rgb_matrix_indicators_user(void) {
    sharkoon_clear_disabled_leds();
    // While selection is active, only re-scan when the effective layer stack
    // actually changed. The expensive matrix walk remains outside normal idle.
    if (sharkoon_macro_select_led_active) {
        const layer_state_t active_layers = layer_state | default_layer_state;
        if (sharkoon_macro_select_cached_layers != active_layers) {
            sharkoon_refresh_macro_select_leds(active_layers);
        }
    }

    // Caps follows the physical KC_CAPS key. Until it is pressed once after
    // startup, fall back to the default Caps position from this keymap.
    if (sharkoon_caps_lock_active) {
        const uint8_t caps_led = sharkoon_caps_lock_led != NO_LED ? sharkoon_caps_lock_led : g_led_config.matrix_co[3][0];
        if (caps_led != NO_LED) {
            const uint8_t brightness = rgb_matrix_config.hsv.v;
            rgb_matrix_set_color(caps_led, brightness, brightness, brightness);
        }
    }

    if (!sharkoon_macro_select_led_active && !sharkoon_recording) {
        return true;
    }

    if (timer_elapsed(sharkoon_macro_select_led_timer) >= SHARKOON_MACRO_LED_BLINK_MS) {
        sharkoon_macro_select_led_timer = timer_read();
        sharkoon_macro_select_blink_on  = !sharkoon_macro_select_blink_on;
    }

    const rgb_t status_rgb = hsv_to_rgb(rgb_matrix_config.hsv);

    // During recording only the REC_M0/REC_M1 key that started the recording
    // blinks. Because the LED was resolved from the physical key event, the
    // indicator follows the recorder key when it is moved in Vial.
    if (sharkoon_recording) {
        if (sharkoon_recording_led != NO_LED) {
            if (sharkoon_macro_select_blink_on) {
                rgb_matrix_set_color(sharkoon_recording_led, status_rgb.r, status_rgb.g, status_rgb.b);
            } else {
                rgb_matrix_set_color(sharkoon_recording_led, 0, 0, 0);
            }
        }
        return true;
    }

    // On the macro-selection layer both recorder keys blink. Their LEDs are
    // discovered from the active dynamic keymap, so moving REC_M0/REC_M1 in
    // Vial also moves the indicator without hard-coded LED indices.
    for (uint8_t i = 0; i < sharkoon_macro_select_led_count; ++i) {
        const uint8_t led = sharkoon_macro_select_leds[i];
        if (sharkoon_macro_select_blink_on) {
            rgb_matrix_set_color(led, status_rgb.r, status_rgb.g, status_rgb.b);
        } else {
            rgb_matrix_set_color(led, 0, 0, 0);
        }
    }

    return true;
}
#endif

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

    [0] = LAYOUT_all(
        KC_ESC,  KC_F1,   KC_F2,   KC_F3,   KC_F4,   KC_F5,   KC_F6,   KC_F7,   KC_F8,   KC_F9,   KC_F10,  KC_F11,  KC_F12,  KC_NO,   KC_NO,   KC_NO,    KC_NO,    KC_PAUS,  RM_TOGG,
        KC_GRV,  KC_1,    KC_2,    KC_3,    KC_4,    KC_5,    KC_6,    KC_7,    KC_8,    KC_9,    KC_0,    KC_MINS, KC_EQL,  KC_BSPC,          KC_DEL,   KC_HOME,  KC_PGUP,  KC_NO,
        KC_TAB,  KC_Q,    KC_W,    KC_E,    KC_R,    KC_T,    KC_Y,    KC_U,    KC_I,    KC_O,    KC_P,    KC_LBRC, KC_RBRC, KC_BSLS,          KC_INS,   KC_END,   KC_PGDN,  MS_BTN3,
        KC_CAPS, KC_A,    KC_S,    KC_D,    KC_F,    KC_G,    KC_H,    KC_J,    KC_K,    KC_L,    KC_SCLN, KC_QUOT,          KC_ENT,           MS_BTN1, MS_UP,    MS_BTN2,
        KC_LSFT, KC_NUBS, KC_Z,    KC_X,    KC_C,    KC_V,    KC_B,    KC_N,    KC_M,    KC_COMM, KC_DOT,  KC_SLSH,          KC_RSFT, KC_UP,   MS_LEFT,  MS_DOWN,  MS_RGHT,  KC_MPLY,
        KC_LCTL, KC_LGUI, KC_LALT,                            KC_SPC,                             KC_RALT, MO(1),   KC_RCTL, KC_LEFT, KC_DOWN,  KC_RGHT, KC_NO,    KC_NO
    ),

    [1] = LAYOUT_all(
        _______, KC_MPLY, KC_MPRV, KC_MNXT, KC_MUTE,  KC_VOLD, KC_VOLU, RM_SPDD,  RM_SPDU,  QK_MACRO_0, QK_MACRO_1, KC_PSCR, KC_SCRL, _______, EE_CLR,   _______,  _______,  _______,  _______,
        _______, _______, _______, _______, _______,  _______, _______, _______,  _______,  _______,   _______,   _______, _______,    _______,          _______,  _______,  _______,  _______,
        OSL(2),  _______, _______, _______, _______,  _______, _______, _______,  _______,  _______,   _______,   _______, _______,    _______,          _______,  _______,  _______,  _______,
        REC_STOP,_______, _______, _______, _______,  _______, _______, _______,  _______,  _______,   _______,   _______,             _______,          QK_MACRO_2, QK_MACRO_3, QK_MACRO_4,
        _______, _______, _______, _______, _______,  _______, _______, _______,  _______,  _______,   _______,   _______,             _______, RM_VALU, QK_MACRO_5, QK_MACRO_6, QK_MACRO_7, _______,
        _______, GU_TOGG, _______,                             _______,                                _______,   _______,   _______, RM_HUEU, RM_VALD,  RM_NEXT,   _______,   _______
    ),

    [2] = LAYOUT_all(
        _______, _______, _______, _______, _______, _______, _______, _______, _______, REC_M0,  REC_M1,  _______, _______, _______, _______, _______, _______, SOCDTOG, _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,          _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,          _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,          _______,          _______, _______, _______,
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,          _______, _______, _______, _______, _______, _______,
        _______, _______, _______,                            _______,                            _______, _______, _______, _______, _______, _______, _______, _______
    ),
};
