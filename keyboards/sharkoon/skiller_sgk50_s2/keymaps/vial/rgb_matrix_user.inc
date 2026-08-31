/*
 * VialRGB Direct remains a normal modern QMK custom-user RGB Matrix effect.
 */
#include "rgb_matrix/animations/vialrgb_direct_anim.h"

#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS

/*
 * config.h renamed QMK's original SOLID_REACTIVE() while the stock effects
 * were compiled. The stock effect has already been parsed at this point.
 *
 * Restore the original symbol name now so the RGB Matrix dispatcher calls
 * this Sharkoon-specific implementation for RGB_MATRIX_SOLID_REACTIVE.
 */
#undef SOLID_REACTIVE

#include "matrix.h"
#include "sync_timer.h"

#include <stdint.h>
#include <string.h>

#define SHARKOON_NSR_IDLE 0u
#define SHARKOON_NSR_HELD UINT32_MAX

/*
 * Per-LED state:
 *
 * SHARKOON_NSR_IDLE
 *     No reactive animation is active.
 *
 * SHARKOON_NSR_HELD
 *     The physical key belonging to this LED is currently held.
 *
 * Any other value
 *     Timestamp at which the key was released. The fade starts from this
 *     exact point in time.
 */
static uint32_t sharkoon_nsr_state[RGB_MATRIX_LED_COUNT];

/*
 * Snapshot of the currently held physical keys.
 *
 * The old implementation searched the entire keyboard matrix separately for
 * every rendered LED. Instead, we scan the matrix only once at the beginning
 * of each RGB frame and cache the result by LED index.
 */
static bool sharkoon_nsr_held[RGB_MATRIX_LED_COUNT];

/*
 * One timestamp for the complete RGB frame so LEDs rendered in different
 * RGB_MATRIX_LED_PROCESS_LIMIT chunks receive the same release timestamp.
 */
static uint32_t sharkoon_nsr_frame_time;

static void sharkoon_nsr_update_held_leds(void) {
    memset(sharkoon_nsr_held, 0, sizeof(sharkoon_nsr_held));

    for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
        for (uint8_t col = 0; col < MATRIX_COLS; ++col) {
            if (!matrix_is_on(row, col)) {
                continue;
            }

            const uint8_t led = g_led_config.matrix_co[row][col];

            if (led != NO_LED && led < RGB_MATRIX_LED_COUNT) {
                sharkoon_nsr_held[led] = true;
            }
        }
    }
}

/*
 * Sharkoon replacement for QMK's stock SOLID_REACTIVE renderer.
 *
 * Vial still sees and controls the normal RGB_MATRIX_SOLID_REACTIVE mode.
 *
 * Behaviour:
 *   press   -> LED immediately becomes black
 *   hold    -> LED remains black indefinitely
 *   release -> fade starts now
 *   fade    -> black -> configured HSV color
 */
bool SOLID_REACTIVE(effect_params_t *params) {
    RGB_MATRIX_USE_LIMITS(led_min, led_max);

    /*
     * QMK can split one RGB frame over several iterations depending on
     * RGB_MATRIX_LED_PROCESS_LIMIT. Do the keyboard-matrix scan only once
     * per complete frame.
     */
    if (params->iter == 0) {
        if (params->init) {
            memset(sharkoon_nsr_state, 0, sizeof(sharkoon_nsr_state));
        }

        sharkoon_nsr_frame_time = sync_timer_read32();
        sharkoon_nsr_update_held_leds();
    }

    const hsv_t base_hsv = rgb_matrix_get_hsv();

    /*
     * Use the same speed scaling principle as QMK's reactive effects.
     * qadd8 guarantees a minimum multiplier of 1.
     */
    const uint8_t speed = qadd8(rgb_matrix_get_speed(), 1);

    /*
     * Maximum elapsed time before scale16by8() reaches the end of the
     * 0..255 fade range.
     */
    const uint16_t max_tick = UINT16_MAX / speed;

    for (uint8_t i = led_min; i < led_max; ++i) {
        RGB_MATRIX_TEST_LED_FLAGS();

        hsv_t hsv = base_hsv;

        /*
         * KEY HELD
         *
         * Keep the key completely black for as long as it remains physically
         * pressed. No animation timer runs while the key is held.
         */
        if (sharkoon_nsr_held[i]) {
            sharkoon_nsr_state[i] = SHARKOON_NSR_HELD;
            hsv.v                  = 0;
        }

        /*
         * JUST RELEASED
         *
         * The previous frame saw the key held and the current frame does not.
         * Start the fade at this exact RGB-frame timestamp.
         */
        else if (sharkoon_nsr_state[i] == SHARKOON_NSR_HELD) {
            uint32_t released_at = sharkoon_nsr_frame_time;

            /*
             * 0 and UINT32_MAX are reserved as state markers.
             */
            if (released_at == SHARKOON_NSR_IDLE) {
                released_at = 1;
            } else if (released_at == SHARKOON_NSR_HELD) {
                released_at = UINT32_MAX - 1;
            }

            sharkoon_nsr_state[i] = released_at;
            hsv.v                  = 0;
        }

        /*
         * RELEASE FADE
         *
         * Fade linearly from black back to the currently configured Vial/QMK
         * brightness. Hue and saturation always follow the current settings.
         */
        else if (sharkoon_nsr_state[i] != SHARKOON_NSR_IDLE) {
            const uint32_t elapsed = sync_timer_elapsed32(sharkoon_nsr_state[i]);

            const uint16_t tick =
                elapsed >= max_tick ? max_tick : (uint16_t)elapsed;

            const uint16_t offset = scale16by8(tick, speed);

            if (offset >= 255) {
                sharkoon_nsr_state[i] = SHARKOON_NSR_IDLE;
                hsv.v                  = base_hsv.v;
            } else {
                hsv.v = scale8((uint8_t)offset, base_hsv.v);
            }
        }

        const rgb_t rgb = rgb_matrix_hsv_to_rgb(hsv);

        rgb_matrix_set_color(i, rgb.r, rgb.g, rgb.b);
    }

    return rgb_matrix_check_finished_leds(led_max);
}

#endif // RGB_MATRIX_CUSTOM_EFFECT_IMPLS