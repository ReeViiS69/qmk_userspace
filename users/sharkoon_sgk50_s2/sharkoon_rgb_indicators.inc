/*
 * Sharkoon RGB indicators and macro lighting.
 *
 * Owns LED discovery/cache, Caps/Unicode overlays and macro-selection/recording
 * lighting. The macro recorder itself remains in sharkoon_keymap.inc.
 */

#define SHARKOON_MACRO_LED_BLINK_MS 400
#define SHARKOON_NO_LAYER           0xFF
#define SHARKOON_LED_BITSET_WORDS   ((RGB_MATRIX_LED_COUNT + SHARKOON_BITSET_WORD_BITS - 1u) / SHARKOON_BITSET_WORD_BITS)

static bool     sharkoon_macro_blink_on       = false;
static uint16_t sharkoon_macro_blink_timer    = 0;
static uint8_t  sharkoon_recording_led        = NO_LED;
static uint8_t  sharkoon_caps_lock_led        = NO_LED;
static uint8_t  sharkoon_unicode_typing_led   = NO_LED;
static bool     sharkoon_unicode_led_active   = false;
static rgb_t    sharkoon_unicode_led_color    = {0, 0, 0};
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

static void sharkoon_render_unicode_typing_led(void) {
    if (!sharkoon_unicode_led_active || sharkoon_unicode_typing_led == NO_LED) {
        return;
    }

    const uint8_t v = rgb_matrix_config.hsv.v;
    rgb_matrix_set_color(
        sharkoon_unicode_typing_led,
        sharkoon_unicode_led_color.r ? v : 0,
        sharkoon_unicode_led_color.g ? v : 0,
        sharkoon_unicode_led_color.b ? v : 0
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
    return sharkoon_recording ||
           (sharkoon_macro_select_layer < DYNAMIC_KEYMAP_LAYER_COUNT &&
            sharkoon_macro_layer_led_count[sharkoon_macro_select_layer] > 0);
}

#include "sharkoon_darkmode.inc"

static void sharkoon_start_macro_blink(void) {
    sharkoon_macro_blink_on    = true;
    sharkoon_macro_blink_timer = timer_read();
}

static void sharkoon_render_macro_leds(void) {
    if (!sharkoon_macro_lighting_active()) {
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

    if (sharkoon_recording) {
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

    if (sharkoon_macro_select_layer != SHARKOON_NO_LAYER && !sharkoon_recording) {
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
            sharkoon_unicode_led_active = true;
            sharkoon_unicode_led_color  = (rgb_t){255, 0, 0};
            break;

        case UCTM_ZALGO:
            sharkoon_unicode_led_active = true;
            sharkoon_unicode_led_color  = (rgb_t){0, 255, 0};
            break;

        case UCTM_SUPER:
            sharkoon_unicode_led_active = true;
            sharkoon_unicode_led_color  = (rgb_t){0, 0, 255};
            break;

        case UCTM_NO_MODE:
        default:
            sharkoon_unicode_led_active = false;
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
