RGB_MATRIX_EFFECT(VIALRGB_DIRECT)

#include "sharkoon_solid_reactive.inc"
#include "sharkoon_palettefx.inc"

#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS

extern hsv_t g_direct_mode_colors[RGB_MATRIX_LED_COUNT];
bool sharkoon_direct_stream_active(void);

/*
 * Wenn das nicht definiert ist, kann unser Effekt niemals
 * auf Tastendrücke reagieren. Lieber beim Compile sofort
 * auffallen lassen, statt stillschweigend nicht zu funktionieren.
 */
#ifndef RGB_MATRIX_KEYREACTIVE_ENABLED
#    error "SHARKOON HYBRID requires RGB_MATRIX_KEYREACTIVE_ENABLED"
#endif


/*
 * =============================================================
 * RAW PALETTE INTERPOLATION
 * =============================================================
 *
 * Im Gegensatz zu palettefx_interp_color() wird hier die globale
 * RGB-Matrix-Helligkeit NICHT angewendet.
 *
 * Das ist entscheidend:
 *
 * Flow
 *   ↓
 * Sparkle
 *   ↓
 * Reactive
 *   ↓
 * globale Helligkeit GANZ AM ENDE
 */

static hsv_t sharkoon_unpack_hsv16(uint16_t packed) {
    return (hsv_t) {
        .h = (uint8_t)packed,
        .s = ((uint8_t)(packed >> 8) & 0x0F) * 17,
        .v = ((uint8_t)(packed >> 12) & 0x0F) * 17,
    };
}


static hsv_t sharkoon_palette_interp_raw(
    const uint16_t *palette,
    uint8_t x
) {
    /*
     * Gleiche Positionierung wie PaletteFx.
     */
    x =
        (x <= 8)
            ? 0
            : ((x < 247)
                   ? (x - 8)
                   : 239);

    const uint8_t i =
        x >> 4;

    const uint8_t frac =
        x << 4;

    hsv_t a =
        sharkoon_unpack_hsv16(
            pgm_read_word(&palette[i])
        );

    hsv_t b =
        sharkoon_unpack_hsv16(
            pgm_read_word(&palette[i + 1])
        );

    const uint8_t hue_wrap =
        128 &
        (
            (a.h >= b.h)
                ? (a.h - b.h)
                : (b.h - a.h)
        );

    return (hsv_t) {
        .h =
            lerp8by8(
                a.h ^ hue_wrap,
                b.h ^ hue_wrap,
                frac
            ) ^ hue_wrap,

        .s =
            lerp8by8(
                a.s,
                b.s,
                frac
            ),

        .v =
            lerp8by8(
                a.v,
                b.v,
                frac
            ),
    };
}


/*
 * RGB-Farbe Richtung Weiß mischen.
 *
 * Genau das brauchen Sparkle und Reactive:
 *
 * - heller
 * - weniger gesättigt
 * - gleicher Grundfarbton
 */
static rgb_t sharkoon_mix_white(
    rgb_t rgb,
    uint8_t amount
) {
    rgb.r =
        qadd8(
            rgb.r,
            scale8(
                255 - rgb.r,
                amount
            )
        );

    rgb.g =
        qadd8(
            rgb.g,
            scale8(
                255 - rgb.g,
                amount
            )
        );

    rgb.b =
        qadd8(
            rgb.b,
            scale8(
                255 - rgb.b,
                amount
            )
        );

    return rgb;
}


/*
 * =============================================================
 * VIALRGB DIRECT / SHARKOON HYBRID
 * =============================================================
 */

static bool VIALRGB_DIRECT(effect_params_t *params) {
    RGB_MATRIX_USE_LIMITS(led_min, led_max);

    /*
     * =========================================================
     * REAL DIRECT RGB
     * =========================================================
     */

    if (sharkoon_direct_stream_active()) {
        for (uint8_t i = led_min; i < led_max; ++i) {
            const rgb_t rgb =
                rgb_matrix_hsv_to_rgb(
                    g_direct_mode_colors[i]
                );

            rgb_matrix_set_color(
                i,
                rgb.r,
                rgb.g,
                rgb.b
            );
        }

        return rgb_matrix_check_finished_leds(led_max);
    }


    /*
     * =========================================================
     * HYBRID STATE
     * =========================================================
     */

    const uint16_t *palette =
        palettefx_get_palette_data();

    /*
     * Globale Helligkeit.
     *
     * WICHTIG:
     * Bis zum endgültigen RGB-Output wird dieser Wert
     * überhaupt nicht angewendet.
     */
    const uint8_t global_value =
        rgb_matrix_config.hsv.v;


    /*
     * =========================================================
     * FLOW STATE
     * =========================================================
     */

    const uint16_t flow_time =
        palettefx_scaled_time(
            g_rgb_timer,
            1 + rgb_matrix_config.speed / 8
        );

    const int8_t rot_c =
        cos8(flow_time / 4) - 128;

    const int8_t rot_s =
        sin8(flow_time / 4) - 128;

    const uint8_t omega =
        32 + sin8(flow_time) / 4;


    /*
     * =========================================================
     * SPARKLE STATE
     * =========================================================
     */

    const uint8_t sparkle_time =
        (uint8_t)flow_time;

    uint16_t rand_state =
        1 + params->iter;


    /*
     * =========================================================
     * REACTIVE STATE
     * =========================================================
     */

    const uint8_t hit_count =
        g_last_hit_tracker.count;

    uint8_t hit_amplitude[LED_HITS_TO_REMEMBER] = {0};

    /*
     * Exakt die Attack/Decay-Grundidee des
     * originalen PaletteFx Reactive.
     */
    for (uint8_t j = 0; j < hit_count; ++j) {
        const uint16_t tick =
            scale16by8(
                g_last_hit_tracker.tick[j],
                1 + rgb_matrix_config.speed / 4
            );

        if (tick <= 255) {
            const uint8_t t =
                (uint8_t)tick;

            if (t <= 55) {
                hit_amplitude[j] =
                    (t < 32)
                        ? (4 + 8 * t)
                        : 255;
            } else {
                const uint8_t decay =
                    (
                        ((uint16_t)(255 - t)) *
                        UINT16_C(164)
                    ) >> 7;

                hit_amplitude[j] =
                    scale8(
                        decay,
                        decay
                    );
            }
        }
    }


    /*
     * =========================================================
     * RENDER LEDs
     * =========================================================
     */

    for (uint8_t i = led_min; i < led_max; ++i) {
        RGB_MATRIX_TEST_LED_FLAGS();


        /*
         * =====================================================
         * 1. FLOW
         * =====================================================
         */

        const uint8_t x =
            g_led_config.point[i].x;

        const uint8_t y =
            g_led_config.point[i].y;

        const uint8_t x1 =
            (uint8_t)(
                ((((int16_t)rot_c) *
                  ((int16_t)x)) / 128) -

                ((((int16_t)rot_s) *
                  ((int16_t)y)) / 128)
            );

        const uint8_t y1 =
            (uint8_t)(
                ((((int16_t)rot_s) *
                  ((int16_t)x)) / 128) +

                ((((int16_t)rot_c) *
                  ((int16_t)y)) / 128)
            );

        uint8_t flow_value =
            scale8(
                sin8(
                    x1 - 2 * flow_time
                ),
                omega
            ) +
            y1 +
            flow_time / 4;

        flow_value =
            2 *
            (
                (flow_value <= 127)
                    ? flow_value
                    : (255 - flow_value)
            );


        /*
         * Rohe Phosphor-Farbe.
         *
         * KEINE globale Helligkeit hier.
         */
        hsv_t hsv =
            sharkoon_palette_interp_raw(
                palette,
                flow_value
            );
        /*
         * Sehr helle Stellen des Flow-Ribbons leicht komprimieren.
         *
         * Sparkle und Reactive passieren erst später und bleiben
         * deshalb vollständig unangetastet.
         */
        if (hsv.v > 220) {
            hsv.v =
                220 +
                scale8(
                    hsv.v - 220,
                    128
                );
        }

        /*
         * =====================================================
         * LOW-BRIGHTNESS PHOSPHOR CORRECTION
         * =====================================================
         *
         * Phosphor endet bei Hue ~40.
         *
         * Bei geringer Helligkeit sieht genau dieser warme
         * Gelbton auf deiner Hardware offenbar rötlich aus.
         *
         * Deshalb wird NUR bei geringer Gesamt-Helligkeit
         * der wärmste Bereich minimal Richtung Grün verschoben.
         *
         * Hohe Helligkeit:
         *     Palette bleibt komplett original.
         *
         * Niedrige Helligkeit:
         *     unterste Hue wandert ungefähr 40 -> 53..60.
         */

        uint8_t minimum_hue = 40;

        if (global_value < 80) {
            minimum_hue =
                40 +
                scale8(
                    80 - global_value,
                    64
                );
        }

        if (hsv.h < minimum_hue) {
            hsv.h =
                minimum_hue;
        }


        /*
         * Normale RGB-Matrix-Sättigung weiterhin respektieren.
         */
        hsv.s =
            scale8(
                hsv.s,
                rgb_matrix_config.hsv.s
            );


        /*
         * Vorerst volle interne RGB-Farbe.
         */
        rgb_t rgb =
            rgb_matrix_hsv_to_rgb(hsv);


        /*
         * =====================================================
         * 2. SPARKLE
         * =====================================================
         *
         * Diesmal KEIN PaletteFx-Palettenversatz.
         *
         * Stattdessen entstehen echte kurze Lichtblitze:
         *
         *       aktuelle Flow-Farbe
         *               ↓
         *           Richtung Weiß
         *
         * Nur das obere Viertel der Sinuswelle erzeugt
         * überhaupt einen Sparkle.
         */

        rand_state *= UINT16_C(36563);

        const uint8_t sparkle_phase =
            (uint8_t)(
                rand_state >> 8
            );

        const uint8_t sparkle_wave =
            sin8(
                2 * sparkle_time +
                sparkle_phase
            );

        uint8_t sparkle_strength = 0;

        if (sparkle_wave > 192) {
            /*
             * 193..255
             * wird ungefähr
             * 4..252.
             */
            sparkle_strength =
                (sparkle_wave - 192) << 2;
        }


        /*
         * Maximal ungefähr 35 % Richtung Weiß.
         *
         * Das sollte klar sichtbar schimmern/blitzen,
         * aber nicht das komplette Flow-Bild zerstören.
         */
        const uint8_t sparkle_white =
            scale8(
                sparkle_strength,
                90
            );

        rgb =
            sharkoon_mix_white(
                rgb,
                sparkle_white
            );


        /*
         * =====================================================
         * 3. REACTIVE
         * =====================================================
         */

        uint8_t reactive_value = 0;

        for (uint8_t j = 0; j < hit_count; ++j) {
            if (hit_amplitude[j] == 0) {
                continue;
            }

            const uint8_t dx =
                abs8(
                    (
                        g_led_config.point[i].x -
                        g_last_hit_tracker.x[j]
                    ) / 2
                );

            const uint8_t dy =
                abs8(
                    (
                        g_led_config.point[i].y -
                        g_last_hit_tracker.y[j]
                    ) / 2
                );

            /*
         * Kleinerer Reactive-Radius.
         *
         * Vorher: 21
         * Jetzt:  16
         */
        if (dx < 16 && dy < 16) {
            const uint16_t dist_sqr =
                dx * dx +
                dy * dy;

            if (dist_sqr < 16 * 16) {
                const uint8_t dist =
                    sqrt16(
                        dist_sqr
                    );

                /*
                 * Linearer Abstand:
                 *
                 * Mitte -> 255
                 * Rand  -> 0
                 */
                const uint8_t distance_falloff =
                    255 -
                    (
                        ((uint16_t)dist * 255) /
                        16
                    );

                /*
                 * Quadratisches Falloff.
                 *
                 * Dadurch bleibt der Kern deutlich sichtbar,
                 * aber der äußere Bereich wird sehr viel sanfter.
                 */
                const uint8_t feathered_falloff =
                    scale8(
                        distance_falloff,
                        distance_falloff
                    );

                reactive_value =
                    qadd8(
                        reactive_value,
                        scale8(
                            feathered_falloff,
                            hit_amplitude[j]
                        )
                    );

                if (reactive_value == 255) {
                    break;
                }
            }
        }
        }


        /*
         * Reactive ist absichtlich deutlich stärker
         * als Sparkle.
         *
         * Maximal ungefähr 63 % Mischung Richtung Weiß.
         *
         * Das bedeutet gleichzeitig:
         *
         * - sichtbar heller
         * - sichtbar weniger gesättigt
         * - KEINE Änderung des Flow-Farbtons
         */
        const uint8_t reactive_white =
            scale8(
                reactive_value,
                160
            );

        rgb =
            sharkoon_mix_white(
                rgb,
                reactive_white
            );


        /*
         * =====================================================
         * 4. GLOBAL BRIGHTNESS
         * =====================================================
         *
         * Erst JETZT wird RM_VALU / RM_VALD angewendet.
         *
         * Und zwar auf:
         *
         * Flow
         * + Sparkle
         * + Reactive
         *
         * exakt gleich.
         */

        /*
         * Brightness curve
         *
         * Maximale Helligkeit bleibt identisch.
         * Niedrige Stufen werden deutlich weiter heruntergeregelt.
         */
        const uint8_t effective_value =
            scale8(
                global_value,
                64 + scale8(global_value, 191)
            );

        rgb.r =
            scale8(
                rgb.r,
                effective_value
            );

        rgb.g =
            scale8(
                rgb.g,
                effective_value
            );

        rgb.b =
            scale8(
                rgb.b,
                effective_value
            );


        /*
         * =====================================================
         * OUTPUT
         * =====================================================
         */

        rgb_matrix_set_color(
            i,
            rgb.r,
            rgb.g,
            rgb.b
        );
    }

    return rgb_matrix_check_finished_leds(led_max);
}

#endif