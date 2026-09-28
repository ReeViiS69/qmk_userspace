/*
 * Sharkoon RGB runtime.
 *
 * Owns board-specific disabled LEDs, status/macro indicators, Darkmode and
 * VIA/Vial RGB command observation. The custom RGB renderers live separately
 * in sharkoon_rgb_effects.inc.
 */

#ifdef RGB_MATRIX_ENABLE
#include "ws2812.h"

/*
 * =============================================================
 * BOARD-SPECIFIC DISABLED LEDS
 * =============================================================
 */
static const uint8_t sharkoon_disabled_leds[] = {
81, 80,            76,  // QK_MACRO_0, QK_MACRO_1, RM_TOGG
                   75,  // TG(2)
       44, 43,     41,  // KC_WSCH, MS_UP, KC_MPLY
       38, 39, 40,      // MS_LEFT, MS_DOWN, MS_RGHT
           11, 10,  9,  // MS_BTN4, MS_BTN5, MS_BTN2
 4,             8,      // MS_BTN1, MS_BTN3
};

static void sharkoon_apply_disabled_led_flags(void) {
    for (uint8_t i = 0; i < sizeof(sharkoon_disabled_leds) / sizeof(sharkoon_disabled_leds[0]); ++i) {
        const uint8_t led = sharkoon_disabled_leds[i];

        if (led < RGB_MATRIX_LED_COUNT) {
            g_led_config.flags[led] = LED_FLAG_NONE;
        }
    }
}

static void sharkoon_clear_disabled_leds(void) {
    for (uint8_t i = 0; i < sizeof(sharkoon_disabled_leds) / sizeof(sharkoon_disabled_leds[0]); ++i) {
        const uint8_t led = sharkoon_disabled_leds[i];
        ws2812_set_color(led, 0, 0, 0);
    }
}

/*
 * =============================================================
 * STATUS / MACRO INDICATORS + DARKMODE
 * =============================================================
 */
/*
 * Sharkoon RGB indicators and macro lighting.
 *
 * Owns LED discovery/cache, Caps/Unicode overlays and macro-selection/recording
 * lighting. The recorder itself lives in sharkoon_macro_recorder.inc.
 */

#define SHARKOON_MACRO_LED_BLINK_MS 400
#define SHARKOON_NO_LAYER           0xFF
#define SHARKOON_BITSET_WORD_BITS     32u
#define SHARKOON_LED_BITSET_WORDS     ((RGB_MATRIX_LED_COUNT + SHARKOON_BITSET_WORD_BITS - 1u) / SHARKOON_BITSET_WORD_BITS)

static bool     sharkoon_macro_blink_on       = false;
static uint16_t sharkoon_macro_blink_timer    = 0;
static uint8_t  sharkoon_recording_led        = NO_LED;
static uint8_t  sharkoon_caps_lock_led        = NO_LED;
static uint8_t  sharkoon_unicode_typing_led   = NO_LED;
enum {
    SHARKOON_UNICODE_LED_OFF = 0,
    SHARKOON_UNICODE_LED_RED,
    SHARKOON_UNICODE_LED_GREEN,
    SHARKOON_UNICODE_LED_BLUE,
};

static uint8_t  sharkoon_unicode_led_channel  = SHARKOON_UNICODE_LED_OFF;
static bool     sharkoon_caps_lock_active     = false;
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
    for (uint8_t word = 0; word < SHARKOON_LED_BITSET_WORDS; ++word) {
        uint32_t bits = sharkoon_macro_layer_leds[layer][word];

        while (bits != 0) {
            const uint8_t bit = (uint8_t)__builtin_ctz(bits);
            const uint8_t led = (uint8_t)(word * SHARKOON_BITSET_WORD_BITS + bit);

            rgb_matrix_set_color(led, r, g, b);

            bits &= bits - 1u;
        }
    }
}

static void sharkoon_clear_macro_select_leds(void) {
    if (sharkoon_macro_select_layer == SHARKOON_NO_LAYER) {
        return;
    }

    sharkoon_set_macro_layer_leds(sharkoon_macro_select_layer, 0, 0, 0);
}

static void sharkoon_render_unicode_typing_led(void) {
    if (sharkoon_unicode_led_channel == SHARKOON_UNICODE_LED_OFF || sharkoon_unicode_typing_led == NO_LED) {
        return;
    }

    const uint8_t v = rgb_matrix_config.hsv.v;
    rgb_matrix_set_color(
        sharkoon_unicode_typing_led,
        sharkoon_unicode_led_channel == SHARKOON_UNICODE_LED_RED ? v : 0,
        sharkoon_unicode_led_channel == SHARKOON_UNICODE_LED_GREEN ? v : 0,
        sharkoon_unicode_led_channel == SHARKOON_UNICODE_LED_BLUE ? v : 0
    );
}

static void sharkoon_render_status_leds(void) {
    sharkoon_render_unicode_typing_led();

    if (sharkoon_caps_lock_active && sharkoon_caps_lock_led != NO_LED) {
        const uint8_t brightness = rgb_matrix_config.hsv.v;
        rgb_matrix_set_color(sharkoon_caps_lock_led, brightness, brightness, brightness);
    }
}

static bool sharkoon_macro_lighting_active(void) {
    return sharkoon_macro_recording_active() || sharkoon_macro_select_layer != SHARKOON_NO_LAYER;
}

/*
 * =============================================================
 * DARKMODE / SEMI-OFF CONTROL
 * =============================================================
 */
/*
 * Sharkoon RGB Darkmode / semi-off control.
 *
 * Idle Darkmode keeps RGB Matrix disabled. It wakes RGB only while macro
 * lighting needs the normal indicator render loop, then returns to semi-off.
 */

static bool sharkoon_rgb_darkmode = false;

static bool sharkoon_darkmode_is_active(void) {
    return sharkoon_rgb_darkmode;
}

static void sharkoon_darkmode_idle(void) {
    rgb_matrix_disable_noeeprom();
    rgb_matrix_task(); // STARTING -> RENDERING
    rgb_matrix_task(); // RENDERING -> FLUSHING, buffer is black
    sharkoon_render_status_leds();
    rgb_matrix_task(); // flush black + status LEDs once
}

static void sharkoon_darkmode_refresh(void) {
    if (!sharkoon_rgb_darkmode || rgb_matrix_is_enabled()) {
        return;
    }

    // RGB Matrix is sleeping; update only the event-driven status LEDs.
    if (sharkoon_caps_lock_led != NO_LED) {
        rgb_matrix_set_color(sharkoon_caps_lock_led, 0, 0, 0);
    }
    if (sharkoon_unicode_typing_led != NO_LED) {
        rgb_matrix_set_color(sharkoon_unicode_typing_led, 0, 0, 0);
    }

    sharkoon_render_status_leds();
    rgb_matrix_update_pwm_buffers();
}

static void sharkoon_update_darkmode_rgb_state(void) {
    if (!sharkoon_rgb_darkmode) {
        return;
    }

    if (sharkoon_macro_lighting_active()) {
        if (!rgb_matrix_is_enabled()) {
            rgb_matrix_enable_noeeprom();
        }
    } else if (rgb_matrix_is_enabled()) {
        sharkoon_darkmode_idle();
    }
}

static void sharkoon_toggle_rgb_darkmode(void) {
    if (sharkoon_rgb_darkmode) {
        sharkoon_rgb_darkmode = false;
        if (!rgb_matrix_is_enabled()) {
            rgb_matrix_enable_noeeprom();
        }
        return;
    }

    if (!rgb_matrix_is_enabled()) {
        rgb_matrix_enable_noeeprom();
        return;
    }

    sharkoon_rgb_darkmode = true;
    if (!sharkoon_macro_lighting_active()) {
        sharkoon_darkmode_idle();
    }
}

/*
 * RM_TOGG while Darkmode is active must always leave RGB enabled, including
 * the temporary macro-blink state where RGB Matrix is already awake.
 * Return true when the key event is fully handled here.
 */
static bool sharkoon_darkmode_handle_rm_togg(void) {
    if (!sharkoon_rgb_darkmode) {
        return false;
    }

    sharkoon_rgb_darkmode = false;
    if (rgb_matrix_is_enabled()) {
        return true;
    }

    return false;
}

static void sharkoon_start_macro_blink(void) {
    sharkoon_macro_blink_on    = true;
    sharkoon_macro_blink_timer = timer_read();
}

static void sharkoon_render_macro_leds(void) {
    const bool recording = sharkoon_macro_recording_active();
    if (!recording && sharkoon_macro_select_layer == SHARKOON_NO_LAYER) {
        return;
    }

    if (timer_elapsed(sharkoon_macro_blink_timer) >= SHARKOON_MACRO_LED_BLINK_MS) {
        sharkoon_macro_blink_timer = timer_read();
        sharkoon_macro_blink_on    = !sharkoon_macro_blink_on;
    }

    rgb_t color = {0, 0, 0};
    if (sharkoon_macro_blink_on) {
        color = hsv_to_rgb(rgb_matrix_config.hsv);
    }

    if (recording) {
        if (sharkoon_recording_led != NO_LED) {
            rgb_matrix_set_color(sharkoon_recording_led, color.r, color.g, color.b);
        }
        return;
    }

    sharkoon_set_macro_layer_leds(sharkoon_macro_select_layer, color.r, color.g, color.b);
}

static void sharkoon_update_macro_select_layer(layer_state_t state) {
    uint8_t next_layer = SHARKOON_NO_LAYER;

    if (state != 0) {
        const uint8_t highest_layer = get_highest_layer(state);
        if (highest_layer < DYNAMIC_KEYMAP_LAYER_COUNT && sharkoon_macro_layer_led_count[highest_layer] > 0) {
            next_layer = highest_layer;
        }
    }

    if (next_layer == sharkoon_macro_select_layer) {
        return;
    }

    sharkoon_clear_macro_select_leds();
    sharkoon_macro_select_layer = next_layer;

    if (sharkoon_macro_select_layer != SHARKOON_NO_LAYER && !sharkoon_macro_recording_active()) {
        sharkoon_start_macro_blink();
    }

    sharkoon_update_darkmode_rgb_state();
}

static void sharkoon_rebuild_keymap_led_cache(void) {
    sharkoon_clear_macro_select_leds();
    sharkoon_macro_select_layer = SHARKOON_NO_LAYER;

    memset(sharkoon_macro_layer_led_count, 0, sizeof(sharkoon_macro_layer_led_count));
    memset(sharkoon_macro_layer_leds, 0, sizeof(sharkoon_macro_layer_leds));
    sharkoon_caps_lock_led      = NO_LED;
    sharkoon_unicode_typing_led = NO_LED;

    uint8_t layer_count = dynamic_keymap_get_layer_count();
    if (layer_count > DYNAMIC_KEYMAP_LAYER_COUNT) {
        layer_count = DYNAMIC_KEYMAP_LAYER_COUNT;
    }

    for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
        for (uint8_t col = 0; col < MATRIX_COLS; ++col) {
            const uint8_t led = g_led_config.matrix_co[row][col];
            if (led == NO_LED || layer_count == 0) {
                continue;
            }

            uint16_t effective_keycode = dynamic_keymap_get_keycode(0, row, col);

            if (sharkoon_caps_lock_led == NO_LED && effective_keycode == KC_CAPS) {
                sharkoon_caps_lock_led = led;
            }

            for (uint8_t layer = 1; layer < layer_count; ++layer) {
                const uint16_t keycode = dynamic_keymap_get_keycode(layer, row, col);

                if (keycode != KC_TRNS) {
                    effective_keycode = keycode;
                }

                if (sharkoon_unicode_typing_led == NO_LED && effective_keycode == UNICODE_TYPING_CYCLE) {
                    sharkoon_unicode_typing_led = led;
                }

                if (effective_keycode == REC_M0 || effective_keycode == REC_M1) {
                    sharkoon_macro_layer_add_led(layer, led);
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
        sharkoon_clear_macro_select_leds();
        sharkoon_macro_select_layer = SHARKOON_NO_LAYER;
        sharkoon_caps_lock_led       = NO_LED;
        sharkoon_unicode_typing_led  = NO_LED;
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

static void sharkoon_rgb_observe_via_command(uint8_t *data, uint8_t length) {
    if (length == 0) {
        return;
    }

    const bool keymap_write = sharkoon_is_dynamic_keymap_write(data[0]);

    if (sharkoon_keymap_cache_dirty && !keymap_write) {
        sharkoon_refresh_keymap_led_cache();
    }

    if (keymap_write) {
        sharkoon_mark_keymap_cache_dirty();
    }
}

static void sharkoon_rgb_indicators_init(void) {
    sharkoon_caps_lock_active = host_keyboard_led_state().caps_lock;
    sharkoon_apply_disabled_led_flags();
    sharkoon_refresh_keymap_led_cache();
}

static void sharkoon_rgb_caps_lock_changed(bool active) {
    sharkoon_caps_lock_active = active;
    sharkoon_darkmode_refresh();
}

static void sharkoon_rgb_layer_state_changed(layer_state_t state) {
    if (sharkoon_keymap_cache_dirty) {
        sharkoon_rebuild_keymap_led_cache();
    }

    sharkoon_update_macro_select_layer(state);
}

static void sharkoon_rgb_macro_recording_started(uint8_t row, uint8_t col) {
    sharkoon_clear_macro_select_leds();
    sharkoon_recording_led = g_led_config.matrix_co[row][col];
    sharkoon_start_macro_blink();
    sharkoon_update_darkmode_rgb_state();
}

static void sharkoon_rgb_macro_recording_finished(void) {
    if (sharkoon_recording_led != NO_LED) {
        rgb_matrix_set_color(sharkoon_recording_led, 0, 0, 0);
    }

    sharkoon_recording_led = NO_LED;
    sharkoon_update_darkmode_rgb_state();
}

static void sharkoon_rgb_unicode_mode_changed(uint8_t mode) {
    switch (mode) {
        case UCTM_AUSSIE:
            sharkoon_unicode_led_channel = SHARKOON_UNICODE_LED_RED;
            break;

        case UCTM_ZALGO:
            sharkoon_unicode_led_channel = SHARKOON_UNICODE_LED_GREEN;
            break;

        case UCTM_SUPER:
            sharkoon_unicode_led_channel = SHARKOON_UNICODE_LED_BLUE;
            break;

        case UCTM_NO_MODE:
        default:
            sharkoon_unicode_led_channel = SHARKOON_UNICODE_LED_OFF;
            break;
    }

    sharkoon_darkmode_refresh();
}

bool rgb_matrix_indicators_user(void) {
    if (sharkoon_darkmode_is_active()) {
        rgb_matrix_set_color_all(0, 0, 0);
    } else {
        sharkoon_clear_disabled_leds();
    }

    sharkoon_render_status_leds();
    sharkoon_render_macro_leds();
    return true;
}

#endif // RGB_MATRIX_ENABLE

/*
 * =============================================================
 * VIA / VIAL RGB COMMAND OBSERVATION
 * =============================================================
 */
#ifdef VIALRGB_DIRECT_ENABLE
void sharkoon_direct_mark_dirty_packet(const uint8_t *data, uint8_t length);

static uint32_t sharkoon_direct_last_packet = 0;
static bool     sharkoon_direct_seen        = false;

bool sharkoon_direct_stream_active(void) {
    if (!sharkoon_direct_seen) {
        return false;
    }

    if (timer_elapsed32(sharkoon_direct_last_packet) >= 250) {
        sharkoon_direct_seen = false;
        return false;
    }

    return true;
}
#endif

#ifdef VIA_ENABLE
bool via_command_kb(uint8_t *data, uint8_t length) {
#ifdef RGB_MATRIX_ENABLE
    sharkoon_rgb_observe_via_command(data, length);
#endif

#ifdef VIALRGB_DIRECT_ENABLE
    if (length >= 2 && data[0] == id_custom_set_value && data[1] == vialrgb_direct_fastset) {
        sharkoon_direct_mark_dirty_packet(data, length);
        sharkoon_direct_last_packet = timer_read32();
        sharkoon_direct_seen        = true;
    }
#endif

    // Never consume VIA/Vial traffic here; the core must process it normally.
    return false;
}
#endif
