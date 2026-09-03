// Copyright 2024 JoyLee (@itarze)
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H

#include "action_layer.h"
#include "color.h"
#include "dynamic_keymap.h"
#include "keycode.h"
#include "send_string_keycodes.h"
#include "timer.h"

#include <stdint.h>
#include <string.h>

#include "sharkoon_sgk50_s2.h"
#ifdef VIA_ENABLE
#    include "via.h"
#endif
#ifdef VIALRGB_ENABLE
#    include "vialrgb.h"
#endif

#include "socd_cleaner.h"
#include "unicode_typing.h"
socd_cleaner_t socd_opposing_pairs[] = {
    {{KC_A, KC_D}, SOCD_CLEANER_LAST},
};
#define SHARKOON_MACRO_MAX_ACTIONS      16
#define SHARKOON_MACRO_EVENT_SIZE       3
#define SHARKOON_MACRO_RECORDING_SIZE   (SHARKOON_MACRO_MAX_ACTIONS * 2 * SHARKOON_MACRO_EVENT_SIZE)
// The recorder needs a RAM mirror of the dynamic macro area while replacing a
// slot. The macro area can never be larger than the logical EEPROM minus the
// dynamic keymap itself. VIA/Vial reserve additional NVM before the macro area,
// so this intentionally remains a safe upper bound for Default, VIA and Vial.
#define SHARKOON_VIA_MACRO_BUFFER_MAX \
    (WEAR_LEVELING_LOGICAL_SIZE - (DYNAMIC_KEYMAP_LAYER_COUNT * MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t)))

#define SHARKOON_MACRO_LED_BLINK_MS     400
#define SHARKOON_NO_LAYER               0xFF

#define SHARKOON_BITSET_WORD_BITS       32u
#define SHARKOON_MATRIX_KEY_COUNT       (MATRIX_ROWS * MATRIX_COLS)
#define SHARKOON_MATRIX_BITSET_WORDS    ((SHARKOON_MATRIX_KEY_COUNT + SHARKOON_BITSET_WORD_BITS - 1u) / SHARKOON_BITSET_WORD_BITS)
#ifdef RGB_MATRIX_ENABLE
#    define SHARKOON_LED_BITSET_WORDS   ((RGB_MATRIX_LED_COUNT + SHARKOON_BITSET_WORD_BITS - 1u) / SHARKOON_BITSET_WORD_BITS)
#endif

enum custom_keycodes {
    REC_M0 = QK_USER_0,
    REC_M1 = QK_USER_1,
    REC_STOP = QK_USER_2,
    MACRO_SELECT = QK_USER_3,
    UNICODE_TYPING_CYCLE = QK_USER_4,
};

static bool    sharkoon_recording              = false;
static bool    sharkoon_accept_new_presses     = false;
static uint8_t sharkoon_recording_slot          = 0;
static uint8_t sharkoon_action_count            = 0;
static uint8_t sharkoon_recording_length        = 0;
static uint8_t sharkoon_pressed_count           = 0;
static uint8_t  sharkoon_recording_data[SHARKOON_MACRO_RECORDING_SIZE];
static uint8_t  sharkoon_recorded_key[MATRIX_ROWS][MATRIX_COLS];
static uint32_t sharkoon_suppress_release[SHARKOON_MATRIX_BITSET_WORDS];
static uint8_t  sharkoon_via_macro_buffer[SHARKOON_VIA_MACRO_BUFFER_MAX];

static uint16_t sharkoon_matrix_bit_index(uint8_t row, uint8_t col) {
    return ((uint16_t)row * MATRIX_COLS) + col;
}

static void sharkoon_set_suppress_release(uint8_t row, uint8_t col) {
    const uint16_t bit = sharkoon_matrix_bit_index(row, col);
    sharkoon_suppress_release[bit / SHARKOON_BITSET_WORD_BITS] |=
        UINT32_C(1) << (bit % SHARKOON_BITSET_WORD_BITS);
}

static bool sharkoon_take_suppress_release(uint8_t row, uint8_t col) {
    const uint16_t bit  = sharkoon_matrix_bit_index(row, col);
    uint32_t      *word = &sharkoon_suppress_release[bit / SHARKOON_BITSET_WORD_BITS];
    const uint32_t mask = UINT32_C(1) << (bit % SHARKOON_BITSET_WORD_BITS);

    if ((*word & mask) == 0) {
        return false;
    }

    *word &= ~mask;
    return true;
}

#ifdef RGB_MATRIX_ENABLE
static bool     sharkoon_macro_blink_on       = false;
static uint16_t sharkoon_macro_blink_timer    = 0;
static uint8_t  sharkoon_recording_led        = NO_LED;
static uint8_t  sharkoon_caps_lock_led        = NO_LED;
static uint8_t  sharkoon_macro_select_layer   = SHARKOON_NO_LAYER;
static uint8_t  sharkoon_macro_layer_led_count[DYNAMIC_KEYMAP_LAYER_COUNT];
static uint32_t sharkoon_macro_layer_leds[DYNAMIC_KEYMAP_LAYER_COUNT][SHARKOON_LED_BITSET_WORDS];
static bool     sharkoon_keymap_cache_dirty   = false;

static void sharkoon_macro_layer_add_led(uint8_t layer, uint8_t led) {
    if (layer >= DYNAMIC_KEYMAP_LAYER_COUNT || led >= RGB_MATRIX_LED_COUNT) {
        return;
    }

    uint32_t      *word = &sharkoon_macro_layer_leds[layer][led / SHARKOON_BITSET_WORD_BITS];
    const uint32_t mask = UINT32_C(1) << (led % SHARKOON_BITSET_WORD_BITS);

    if ((*word & mask) == 0) {
        *word |= mask;
        ++sharkoon_macro_layer_led_count[layer];
    }
}

static void sharkoon_set_macro_layer_leds(uint8_t layer, uint8_t r, uint8_t g, uint8_t b) {
    if (layer >= DYNAMIC_KEYMAP_LAYER_COUNT) {
        return;
    }

    for (uint8_t word = 0; word < SHARKOON_LED_BITSET_WORDS; ++word) {
        uint32_t bits = sharkoon_macro_layer_leds[layer][word];

        while (bits != 0) {
            const uint8_t bit = (uint8_t)__builtin_ctz(bits);
            const uint8_t led = (uint8_t)(word * SHARKOON_BITSET_WORD_BITS + bit);

            if (led < RGB_MATRIX_LED_COUNT) {
                rgb_matrix_set_color(led, r, g, b);
            }

            bits &= bits - 1u;
        }
    }
}

static void sharkoon_clear_macro_select_leds(void) {
    sharkoon_set_macro_layer_leds(sharkoon_macro_select_layer, 0, 0, 0);
}

static void sharkoon_start_macro_blink(void) {
    sharkoon_macro_blink_on    = true;
    sharkoon_macro_blink_timer = timer_read();
}

static void sharkoon_update_macro_select_layer(layer_state_t state) {
    uint8_t next_layer = SHARKOON_NO_LAYER;

    // Layer 0 by itself must never create permanent recorder blinking.
    // Any non-default layer uses its pre-resolved recorder view from the cache.
    if (state != 0) {
        const uint8_t highest_layer = get_highest_layer(state);
        if (highest_layer < DYNAMIC_KEYMAP_LAYER_COUNT && sharkoon_macro_layer_led_count[highest_layer] > 0) {
            next_layer = highest_layer;
        }
    }

    if (next_layer == sharkoon_macro_select_layer) {
        return;
    }

    // Always end the previous blink explicitly with black. This is intentional
    // for LEDs with LED_FLAG_NONE as well as normal RGB-effect LEDs.
    sharkoon_clear_macro_select_leds();
    sharkoon_macro_select_layer = next_layer;

    if (sharkoon_macro_select_layer != SHARKOON_NO_LAYER && !sharkoon_recording) {
        sharkoon_start_macro_blink();
    }
}

static void sharkoon_rebuild_keymap_led_cache(void) {
    // Stop using the old mapping before replacing it. This also guarantees that
    // a flag==0 LED touched by our overlay is explicitly cleared once.
    sharkoon_clear_macro_select_leds();
    sharkoon_macro_select_layer = SHARKOON_NO_LAYER;

    memset(sharkoon_macro_layer_led_count, 0, sizeof(sharkoon_macro_layer_led_count));
    memset(sharkoon_macro_layer_leds, 0, sizeof(sharkoon_macro_layer_leds));
    sharkoon_caps_lock_led = NO_LED;

    uint8_t layer_count = dynamic_keymap_get_layer_count();
    if (layer_count > DYNAMIC_KEYMAP_LAYER_COUNT) {
        layer_count = DYNAMIC_KEYMAP_LAYER_COUNT;
    }

    for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
        for (uint8_t col = 0; col < MATRIX_COLS; ++col) {
            const uint8_t led = g_led_config.matrix_co[row][col];
            if (led == NO_LED) {
                continue;
            }

            uint16_t keycodes[DYNAMIC_KEYMAP_LAYER_COUNT];
            for (uint8_t layer = 0; layer < layer_count; ++layer) {
                keycodes[layer] = dynamic_keymap_get_keycode(layer, row, col);
            }

            // Caps is intentionally tied to KC_CAPS on the base layer rather
            // than to a hard-coded matrix position.
            if (sharkoon_caps_lock_led == NO_LED && layer_count > 0 && keycodes[0] == KC_CAPS) {
                sharkoon_caps_lock_led = led;
            }

            // Pre-resolve every selectable layer once. Transparent keys walk
            // down through the configured layers, even if an intermediate
            // layer will not itself be active later. The first non-transparent
            // key blocks everything below it.
            for (uint8_t top_layer = 1; top_layer < layer_count; ++top_layer) {
                for (int8_t layer = (int8_t)top_layer; layer >= 0; --layer) {
                    const uint16_t keycode = keycodes[(uint8_t)layer];

                    if (keycode == KC_TRNS) {
                        continue;
                    }

                    if (keycode == REC_M0 || keycode == REC_M1) {
                        sharkoon_macro_layer_add_led(top_layer, led);
                    }
                    break;
                }
            }
        }
    }

    sharkoon_keymap_cache_dirty = false;
}

static void sharkoon_refresh_keymap_led_cache(void) {
    sharkoon_rebuild_keymap_led_cache();
    sharkoon_update_macro_select_layer(layer_state);
}

static void sharkoon_mark_keymap_cache_dirty(void) {
    if (!sharkoon_keymap_cache_dirty) {
        // Do not keep rendering positions from the old layout while VIA/Vial
        // is changing the dynamic keymap.
        sharkoon_clear_macro_select_leds();
        sharkoon_macro_select_layer = SHARKOON_NO_LAYER;
        sharkoon_caps_lock_led      = NO_LED;
    }
    sharkoon_keymap_cache_dirty = true;
}

static bool sharkoon_is_dynamic_keymap_write(uint8_t command_id) {
    switch (command_id) {
        case id_dynamic_keymap_set_keycode:
        case id_dynamic_keymap_reset:
        case id_dynamic_keymap_set_buffer:
#ifdef VIA_EEPROM_ALLOW_RESET
        case id_eeprom_reset:
#endif
            return true;
        default:
            return false;
    }
}

static void sharkoon_observe_via_command(uint8_t *data, uint8_t length) {
    if (length == 0) {
        return;
    }

    const bool keymap_write = sharkoon_is_dynamic_keymap_write(data[0]);

    // VIA calls the keyboard hook before processing the command. Repeated write
    // packets therefore only mark the cache dirty. The first following non-write
    // packet can safely rebuild once because all previous writes are complete.
    if (sharkoon_keymap_cache_dirty && !keymap_write) {
        sharkoon_refresh_keymap_led_cache();
    }

    if (keymap_write) {
        sharkoon_mark_keymap_cache_dirty();
    }
}
#endif

#ifdef VIALRGB_DIRECT_ENABLE
static uint32_t sharkoon_direct_last_packet = 0;
static bool     sharkoon_direct_seen        = false;

bool sharkoon_direct_stream_active(void) {
    return sharkoon_direct_seen && timer_elapsed32(sharkoon_direct_last_packet) < 250;
}
#endif

#ifdef VIA_ENABLE
bool via_command_kb(uint8_t *data, uint8_t length) {
#ifdef RGB_MATRIX_ENABLE
    sharkoon_observe_via_command(data, length);
#endif

#ifdef VIALRGB_DIRECT_ENABLE
    if (length >= 2 && data[0] == id_custom_set_value && data[1] == vialrgb_direct_fastset) {
        sharkoon_direct_last_packet = timer_read32();
        sharkoon_direct_seen        = true;
    }
#endif

    // Never consume VIA/Vial traffic here; the core must process it normally.
    return false;
}
#endif

#ifdef RGB_MATRIX_ENABLE
void keyboard_post_init_user(void) {
    sharkoon_apply_disabled_led_flags();
    sharkoon_refresh_keymap_led_cache();
}

layer_state_t layer_state_set_user(layer_state_t state) {
    // Normal MO/OSL/TG changes only switch the already prepared cache view.
    // A rebuild here can happen only if a preceding VIA/Vial layout write left
    // the cache dirty and the configurator has not triggered the refresh yet.
    if (sharkoon_keymap_cache_dirty) {
        sharkoon_rebuild_keymap_led_cache();
    }

    sharkoon_update_macro_select_layer(state);
    return state;
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
    // Selection is hidden during recording. Clear its last rendered state once,
    // but keep the selected layer cached so it can resume after recording.
    sharkoon_clear_macro_select_leds();

    // The physical event already gives us the exact recorder LED for free.
    sharkoon_recording_led = g_led_config.matrix_co[row][col];
    sharkoon_start_macro_blink();
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

    // No persistent state changes when the slot already contains exactly the
    // same macro. In particular, avoid toggling the macro-buffer valid flag
    // from 0 -> 0xFF -> 0 for a no-op save.
    if (old_macro_size == new_macro_size &&
        memcmp(&sharkoon_via_macro_buffer[macro_start], sharkoon_recording_data, sharkoon_recording_length) == 0) {
        return;
    }

    const int32_t size_delta = (int32_t)new_macro_size - (int32_t)old_macro_size;
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
    // Always end our own blink explicitly with black, independent of LED flags.
    if (sharkoon_recording_led != NO_LED) {
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

    // A custom control key can resolve to a different keycode on release after
    // changing the layer state. Suppress that physical release by matrix
    // position so no release from the newly exposed key can escape to the host.
    if (!record->event.pressed && sharkoon_take_suppress_release(row, col)) {
        return false;
    }

    switch (keycode) {
        case UNICODE_TYPING_CYCLE:
            sharkoon_set_suppress_release(row, col);
            if (record->event.pressed) {
                uint8_t next_mode = get_unicode_typing_mode() + 1;
                if (next_mode >= UNCODES_MODE_END) {
                    next_mode = UCTM_NO_MODE;
                }
                set_unicode_typing_mode(next_mode);
            }
            return false;

        case MACRO_SELECT:
            sharkoon_set_suppress_release(row, col);

            if (record->event.pressed) {
                if (is_layer_locked(1) || layer_state_is(3)) {
                    // Macro-Auswahl abbrechen
                    layer_lock_off(1);
                    layer_off(3);
                } else {
                    // Macro-Auswahl starten
                    layer_lock_on(1);
                    layer_on(3);
                }
            }
            return false;

        case REC_M0:
        case REC_M1:
            sharkoon_set_suppress_release(row, col);
            if (record->event.pressed) {
                // Leave the macro-selection stack deterministically: layer 3
                // must be off and the temporary layer-1 lock must be released.
                layer_lock_off(1);
                layer_off(3);

                // Keep support for recorder keys reached through any unrelated
                // one-shot layer the user may configure elsewhere.
                sharkoon_consume_active_oneshot_layer();

                if (!sharkoon_recording) {
                    sharkoon_start_recording(keycode == REC_M0 ? 0 : 1, row, col);
                }
            }
            return false;

        case REC_STOP:
            sharkoon_set_suppress_release(row, col);
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
            sharkoon_set_suppress_release(row, col);
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
bool rgb_matrix_indicators_user(void) {
    // Normal effects (including Digital Rain) may still touch LED_FLAG_NONE.
    // Keep that policy centralized, then allow our own status overlays to draw
    // on those LEDs intentionally afterwards.
    sharkoon_clear_disabled_leds();

    if (host_keyboard_led_state().caps_lock && sharkoon_caps_lock_led != NO_LED) {
        const uint8_t brightness = rgb_matrix_config.hsv.v;
        rgb_matrix_set_color(sharkoon_caps_lock_led, brightness, brightness, brightness);
    }

    const bool macro_select_active =
        sharkoon_macro_select_layer < DYNAMIC_KEYMAP_LAYER_COUNT &&
        sharkoon_macro_layer_led_count[sharkoon_macro_select_layer] > 0;

    if (!macro_select_active && !sharkoon_recording) {
        return true;
    }

    if (timer_elapsed(sharkoon_macro_blink_timer) >= SHARKOON_MACRO_LED_BLINK_MS) {
        sharkoon_macro_blink_timer = timer_read();
        sharkoon_macro_blink_on    = !sharkoon_macro_blink_on;
    }

    const rgb_t status_rgb = hsv_to_rgb(rgb_matrix_config.hsv);

    // Recording has priority over selection and uses only the physical key that
    // actually started the recording.
    if (sharkoon_recording) {
        if (sharkoon_recording_led != NO_LED) {
            if (sharkoon_macro_blink_on) {
                rgb_matrix_set_color(sharkoon_recording_led, status_rgb.r, status_rgb.g, status_rgb.b);
            } else {
                rgb_matrix_set_color(sharkoon_recording_led, 0, 0, 0);
            }
        }
        return true;
    }

    // The active layer view was already fully resolved when the keymap cache was
    // rebuilt. RGB frames only render the cached LED bitset; no keymap scan occurs.
    if (sharkoon_macro_blink_on) {
        sharkoon_set_macro_layer_leds(sharkoon_macro_select_layer, status_rgb.r, status_rgb.g, status_rgb.b);
    } else {
        sharkoon_set_macro_layer_leds(sharkoon_macro_select_layer, 0, 0, 0);
    }

    return true;
}
#endif

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

    [0] = LAYOUT_all(
        KC_ESC,  KC_F1,   KC_F2,   KC_F3,   KC_F4,   KC_F5,   KC_F6,   KC_F7,   KC_F8,   KC_F9,    KC_F10,   KC_F11,  KC_F12,  KC_NO,   KC_NO,   KC_NO,    UNICODE_TYPING_CYCLE,    KC_PAUS,  RM_TOGG,
        KC_GRV,  KC_1,    KC_2,    KC_3,    KC_4,    KC_5,    KC_6,    KC_7,    KC_8,    KC_9,     KC_0,     KC_MINS, KC_EQL,  KC_BSPC,          KC_DEL,   KC_HOME,  KC_PGUP,  KC_NO,
        KC_TAB,  KC_Q,    KC_W,    KC_E,    KC_R,    KC_T,    KC_Y,    KC_U,    KC_I,    KC_O,     KC_P,     KC_LBRC, KC_RBRC, KC_BSLS,          KC_INS,   KC_END,   KC_PGDN,  MS_BTN3,
        KC_CAPS, KC_A,    KC_S,    KC_D,    KC_F,    KC_G,    KC_H,    KC_J,    KC_K,    KC_L,     KC_SCLN,  KC_QUOT,          KC_ENT,           MS_BTN1, MS_UP,    MS_BTN2,
        SC_LSPO, KC_NUBS, KC_Z,    KC_X,    KC_C,    KC_V,    KC_B,    KC_N,    KC_M,    KC_COMM,  KC_DOT,   KC_SLSH,          SC_RSPC, KC_UP,   MS_LEFT,  MS_DOWN,  MS_RGHT,  KC_MPLY,
        KC_LCTL, KC_LGUI, KC_LALT,                            KC_SPC,                                  KC_RALT, MO(1),   KC_RCTL, KC_LEFT, KC_DOWN,  KC_RGHT, TG(2),    KC_NO
    ),

    [1] = LAYOUT_all(
        _______, KC_MPLY, KC_MPRV, KC_MNXT, KC_MUTE,  KC_VOLD, KC_VOLU, RM_SPDD,  RM_SPDU,  QK_MACRO_0, QK_MACRO_1, KC_PSCR, KC_SCRL, _______, EE_CLR,   _______,  _______,  SOCDTOG,  _______,
        _______, _______, _______, _______, _______,  _______, _______, _______,  _______,  _______,   _______,   _______, _______,    _______,          _______,  _______,  _______,  _______,
        MACRO_SELECT, _______, _______, _______, _______,  _______, _______, _______,  _______,  _______,   _______,   _______, _______,    _______,          _______,  _______,  _______,  _______,
        REC_STOP,_______, _______, _______, _______,  _______, _______, _______,  _______,  _______,   _______,   _______,             _______,          QK_MACRO_2, QK_MACRO_3, QK_MACRO_4,
        _______, _______, _______, _______, _______,  _______, _______, _______,  _______,  _______,   _______,   _______,             _______, RM_VALU, QK_MACRO_5, QK_MACRO_6, QK_MACRO_7, _______,
        _______, GU_TOGG, _______,                             _______,                                _______,   _______,   _______, RM_HUEU, RM_VALD,  RM_NEXT,   _______,   _______
    ),

    // German-on-US raw compatibility.
    // Layer 2 = the unshifted German character positions encoded as US HID.
    // The two physical Shift keys do not send Shift here; they only hold layer 3.
    [2] = LAYOUT_all(
        _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,
        KC_CIRC, KC_1,    KC_2,    KC_3,    KC_4,    KC_5,    KC_6,    KC_7,    KC_8,    KC_9,    KC_0,    KC_MINS, KC_EQL,  _______,          _______, _______, _______, _______,
        _______, KC_Q,    KC_W,    KC_E,    KC_R,    KC_T,    KC_Z,    KC_U,    KC_I,    KC_O,    KC_P,    KC_LBRC, KC_PLUS, KC_HASH,          _______, _______, _______, _______,
        _______, KC_A,    KC_S,    KC_D,    KC_F,    KC_G,    KC_H,    KC_J,    KC_K,    KC_L,    KC_SCLN, KC_QUOT,          _______,          _______, _______, _______,
        MO(3),   KC_LABK, KC_Y,    KC_X,    KC_C,    KC_V,    KC_B,    KC_N,    KC_M,    KC_COMM, KC_DOT,  KC_MINS,          MO(3),   _______, _______, _______, _______, _______,
        _______, _______, _______,                            _______,                            _______, _______, _______, _______, _______, _______, TG(2),    _______
    ),

    // Layer 3 serves two jobs intentionally:
    // 1) shifted half of the raw compatibility layout while either Shift key on
    //    layer 2 is held, and
    // 2) the macro-selection layer activated by MACRO_SELECT.
    // REC_M0/REC_M1 live on the first two keys right of F12. MACRO_SELECT is
    // additionally available on the key immediately left of Pause.
    [3] = LAYOUT_all(
        S(KC_ESC), S(KC_F1), S(KC_F2), S(KC_F3), S(KC_F4), S(KC_F5), S(KC_F6), S(KC_F7), S(KC_F8), S(KC_F9), S(KC_F10), S(KC_F11), S(KC_F12), REC_M0, REC_M1, _______, MACRO_SELECT, S(KC_PAUS), _______,
        KC_CIRC, KC_EXLM, KC_DQUO, KC_HASH, KC_DLR, KC_PERC, KC_AMPR, KC_SLSH, KC_LPRN, KC_RPRN, KC_EQL, KC_QUES, KC_GRV, S(KC_BSPC),          S(KC_DEL), S(KC_HOME), S(KC_PGUP), _______,
        S(KC_TAB), S(KC_Q), S(KC_W), S(KC_E), S(KC_R), S(KC_T), S(KC_Z), S(KC_U), S(KC_I), S(KC_O), S(KC_P), KC_LCBR, KC_ASTR, KC_QUOT,          S(KC_INS), S(KC_END), S(KC_PGDN), _______,
        _______, S(KC_A), S(KC_S), S(KC_D), S(KC_F), S(KC_G), S(KC_H), S(KC_J), S(KC_K), S(KC_L), KC_COLN, KC_DQUO,          S(KC_ENT),          _______, _______, _______,
        _______, KC_RABK, S(KC_Y), S(KC_X), S(KC_C), S(KC_V), S(KC_B), S(KC_N), S(KC_M), KC_SCLN, KC_COLN, KC_UNDS,          _______, S(KC_UP), _______, _______, _______, _______,
        _______, _______, _______,                            S(KC_SPC),                            _______, _______, _______, S(KC_LEFT), S(KC_DOWN), S(KC_RGHT), KC_NO,    _______
    ),
};
