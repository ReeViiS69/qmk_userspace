#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS

/*
 * config.h renamed QMK's original SOLID_REACTIVE() while the stock effects
 * were compiled. The stock effect has already been parsed at this point.
 *
 * Restore the original symbol name now so the RGB Matrix dispatcher calls
 * this Sharkoon-specific implementation for RGB_MATRIX_SOLID_REACTIVE.
 */
#undef SOLID_REACTIVE

#include "sync_timer.h"

#include <stdint.h>
#include <string.h>

#define SHARKOON_NSR_IDLE              0u
#define SHARKOON_NSR_HELD              UINT32_MAX
#define SHARKOON_NSR_HELD_WORD_BITS    32u
#define SHARKOON_NSR_HELD_WORDS        ((RGB_MATRIX_LED_COUNT + SHARKOON_NSR_HELD_WORD_BITS - 1u) / SHARKOON_NSR_HELD_WORD_BITS)

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
 * Event-driven state of the currently held physical keys.
 *
 * live  = updated once when QMK processes a physical press/release
 * frame = copied once at the beginning of a complete RGB frame
 *
 * The tiny bitset copy preserves the old per-frame snapshot semantics while
 * removing every MATRIX_ROWS * MATRIX_COLS poll from the renderer.
 */
static uint32_t sharkoon_nsr_held_live[SHARKOON_NSR_HELD_WORDS];
static uint32_t sharkoon_nsr_held_frame[SHARKOON_NSR_HELD_WORDS];

static inline void sharkoon_nsr_set_held(uint8_t led, bool held) {
    uint32_t      *word = &sharkoon_nsr_held_live[led / SHARKOON_NSR_HELD_WORD_BITS];
    const uint32_t mask = UINT32_C(1) << (led % SHARKOON_NSR_HELD_WORD_BITS);

    if (held) {
        *word |= mask;
    } else {
        *word &= ~mask;
    }
}

static inline bool sharkoon_nsr_is_held(uint8_t led) {
    return (sharkoon_nsr_held_frame[led / SHARKOON_NSR_HELD_WORD_BITS] &
            (UINT32_C(1) << (led % SHARKOON_NSR_HELD_WORD_BITS))) != 0;
}

/*
 * Called from pre_process_record_user(). Keeping this outside the renderer is
 * the important part: physical key state changes are pushed exactly once,
 * instead of being rediscovered by scanning MATRIX_ROWS * MATRIX_COLS every
 * RGB frame.
 */
void sharkoon_solid_reactive_process_key_event(keyrecord_t *record) {
    const uint8_t row = record->event.key.row;
    const uint8_t col = record->event.key.col;

    if (row >= MATRIX_ROWS || col >= MATRIX_COLS) {
        return;
    }

    const uint8_t led = g_led_config.matrix_co[row][col];

    if (led != NO_LED && led < RGB_MATRIX_LED_COUNT) {
        sharkoon_nsr_set_held(led, record->event.pressed);
    }
}

/*
 * Everything below is constant for all chunks of one complete RGB frame.
 * QMK may split a frame via RGB_MATRIX_LED_PROCESS_LIMIT, so calculating
 * these values only at iter == 0 removes repeated HSV conversion, speed
 * lookups and the 16-bit division from every following chunk.
 */
typedef struct {
    uint32_t frame_time;
    hsv_t    base_hsv;
    rgb_t    base_rgb;
    uint8_t  speed;
    uint16_t max_tick;
} sharkoon_nsr_frame_t;

static sharkoon_nsr_frame_t sharkoon_nsr_frame;
static bool                 sharkoon_nsr_frame_ready;

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
     * RGB_MATRIX_LED_PROCESS_LIMIT. Capture one timestamp for the complete
     * frame; held state itself is already maintained by key events.
     */
    if (params->iter == 0 || !sharkoon_nsr_frame_ready) {
        if (params->init) {
            memset(sharkoon_nsr_state, 0, sizeof(sharkoon_nsr_state));
        }

        sharkoon_nsr_frame.frame_time = sync_timer_read32();
        memcpy(sharkoon_nsr_held_frame, sharkoon_nsr_held_live, sizeof(sharkoon_nsr_held_frame));

        sharkoon_nsr_frame.base_hsv = rgb_matrix_get_hsv();
        sharkoon_nsr_frame.base_rgb = rgb_matrix_hsv_to_rgb(sharkoon_nsr_frame.base_hsv);

        /*
         * Use the same speed scaling principle as QMK's reactive effects.
         * qadd8 guarantees a minimum multiplier of 1.
         */
        sharkoon_nsr_frame.speed = qadd8(rgb_matrix_get_speed(), 1);

        /*
         * Maximum elapsed time before scale16by8() reaches the end of the
         * 0..255 fade range. This 16-bit division now happens once per full
         * frame instead of once per render chunk.
         */
        sharkoon_nsr_frame.max_tick = UINT16_MAX / sharkoon_nsr_frame.speed;
        sharkoon_nsr_frame_ready    = true;
    }

    const hsv_t    base_hsv = sharkoon_nsr_frame.base_hsv;
    const rgb_t    base_rgb = sharkoon_nsr_frame.base_rgb;
    const uint8_t  speed    = sharkoon_nsr_frame.speed;
    const uint16_t max_tick = sharkoon_nsr_frame.max_tick;

    for (uint8_t i = led_min; i < led_max; ++i) {
        RGB_MATRIX_TEST_LED_FLAGS();

        /*
         * KEY HELD
         *
         * No HSV conversion is needed: V=0 is exactly black.
         */
        if (sharkoon_nsr_is_held(i)) {
            sharkoon_nsr_state[i] = SHARKOON_NSR_HELD;
            rgb_matrix_set_color(i, 0, 0, 0);
            continue;
        }

        /*
         * JUST RELEASED
         *
         * The previous rendered frame saw the key held and the event-driven
         * held bit is now clear. Start the fade at this RGB-frame timestamp.
         */
        if (sharkoon_nsr_state[i] == SHARKOON_NSR_HELD) {
            uint32_t released_at = sharkoon_nsr_frame.frame_time;

            /*
             * 0 and UINT32_MAX are reserved as state markers.
             */
            if (released_at == SHARKOON_NSR_IDLE) {
                released_at = 1;
            } else if (released_at == SHARKOON_NSR_HELD) {
                released_at = UINT32_MAX - 1;
            }

            sharkoon_nsr_state[i] = released_at;
            rgb_matrix_set_color(i, 0, 0, 0);
            continue;
        }

        /*
         * IDLE FAST PATH
         *
         * Most LEDs spend most frames here. Convert the configured base color
         * once per frame, not once per idle LED.
         */
        if (sharkoon_nsr_state[i] == SHARKOON_NSR_IDLE) {
            rgb_matrix_set_color(i, base_rgb.r, base_rgb.g, base_rgb.b);
            continue;
        }

        /*
         * RELEASE FADE
         *
         * Fade linearly from black back to the currently configured Vial/QMK
         * brightness. Hue and saturation always follow the current settings.
         * Only actively fading LEDs need an HSV -> RGB conversion.
         */
        const uint32_t elapsed = sync_timer_elapsed32(sharkoon_nsr_state[i]);
        const uint16_t tick    = elapsed >= max_tick ? max_tick : (uint16_t)elapsed;
        const uint16_t offset  = scale16by8(tick, speed);

        if (offset >= 255) {
            sharkoon_nsr_state[i] = SHARKOON_NSR_IDLE;
            rgb_matrix_set_color(i, base_rgb.r, base_rgb.g, base_rgb.b);
            continue;
        }

        hsv_t hsv = base_hsv;
        hsv.v     = scale8((uint8_t)offset, base_hsv.v);

        const rgb_t rgb = rgb_matrix_hsv_to_rgb(hsv);
        rgb_matrix_set_color(i, rgb.r, rgb.g, rgb.b);
    }

    return rgb_matrix_check_finished_leds(led_max);
}

#endif // RGB_MATRIX_CUSTOM_EFFECT_IMPLS
