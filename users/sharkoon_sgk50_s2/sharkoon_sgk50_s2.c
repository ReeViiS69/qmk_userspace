#include "sharkoon_sgk50_s2.h"

#ifdef RGB_MATRIX_ENABLE

static const uint8_t sharkoon_disabled_leds[] = {
    7, 8, 9, 10, 11, 12,
    38, 39, 40, 41,
    75, 76,
    78, 79, 80, 81,
};

void sharkoon_apply_disabled_led_flags(void) {
    for (uint8_t i = 0; i < sizeof(sharkoon_disabled_leds) / sizeof(sharkoon_disabled_leds[0]); ++i) {
        const uint8_t led = sharkoon_disabled_leds[i];

        if (led < RGB_MATRIX_LED_COUNT) {
            g_led_config.flags[led] = LED_FLAG_NONE;
        }
    }
}

void sharkoon_clear_disabled_leds(void) {
    for (uint8_t i = 0; i < sizeof(sharkoon_disabled_leds) / sizeof(sharkoon_disabled_leds[0]); ++i) {
        const uint8_t led = sharkoon_disabled_leds[i];

        if (led < RGB_MATRIX_LED_COUNT) {
            rgb_matrix_set_color(led, 0, 0, 0);
        }
    }
}

#endif