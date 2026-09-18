RGB_MATRIX_EFFECT(VIALRGB_DIRECT)

#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS
#    ifdef SHARKOON_PERF_BENCHMARK
#        include "sharkoon_sgk50_s2.h"

/* Cortex-M3 DWT cycle counter, used only by the temporary benchmark build. */
#        define SHARKOON_DWT_CYCCNT (*(volatile uint32_t *)UINT32_C(0xE0001004))

static sharkoon_perf_rgb_stats_t sharkoon_perf_rgb_stats;

static inline uint32_t sharkoon_perf_cycle_now(void) {
    return SHARKOON_DWT_CYCCNT;
}

static inline void sharkoon_perf_accumulate(uint32_t *sum, uint32_t *max_value, uint32_t *count, uint32_t cycles) {
    *sum += cycles;
    if (cycles > *max_value) {
        *max_value = cycles;
    }
    ++(*count);
}

static inline void sharkoon_perf_accumulate_sample(uint32_t *sum, uint32_t *max_value, uint32_t cycles) {
    *sum += cycles;
    if (cycles > *max_value) {
        *max_value = cycles;
    }
}

void sharkoon_perf_rgb_take_snapshot(sharkoon_perf_rgb_stats_t *out) {
    *out = sharkoon_perf_rgb_stats;
    sharkoon_perf_rgb_stats = (sharkoon_perf_rgb_stats_t){0};
}
#    endif
#endif

#include "sharkoon_solid_reactive.inc"
#include "sharkoon_palettefx.inc"

#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS

#include "led_tables.h"

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
 * QMK includes this custom-effect implementation before rgb_matrix.c's own
 * tentative definition of last_hit_buffer. Repeat the compatible tentative
 * declaration here so this effect can release only its own finished reactive
 * history without modifying QMK core source. Both declarations name the same
 * file-local object in the rgb_matrix.c translation unit.
 */
static last_hit_t last_hit_buffer;

#include "sharkoon_hybrid_luts.inc"

#define SHARKOON_PALETTE_COLOR_COUNT 16u
#define SHARKOON_FLOW_COLOR_COUNT    128u

/*
 * =============================================================
 * VIAL DIRECT RGB CACHE
 * =============================================================
 *
 * DirectRGB hosts normally stream continuously. Convert each packet's HSV
 * values to RGB immediately in the existing VIA observation hook so the RGB
 * render hotpath only has to load the cached RGB value.
 *
 * No separate cache initialization is needed: both g_direct_mode_colors[] and
 * this cache start as zero-initialized BSS. Every Direct packet updates both
 * states with the same clamped HSV value, so untouched LEDs remain synchronized
 * even when the first packet is only a partial update.
 *
 * Keep the existing public hook/counter names intentionally. Besides minimizing
 * unrelated LTO churn in this benchmark round, direct_cache_misses continues to
 * count the same event as before: one HSV->RGB conversion caused by a Direct
 * update.
 */
static rgb_t sharkoon_direct_rgb_cache[RGB_MATRIX_LED_COUNT];

/*
 * DirectRGB-specialized HSV -> RGB conversion. This is intentionally kept
 * local to the packet hotpath; normal QMK/Hybrid effects continue to use
 * rgb_matrix_hsv_to_rgb() unchanged.
 *
 * QMK computes region = h * 6 / 255. For h in [0,255], division by 255 is
 * exactly equivalent to:
 *
 *   x = h * 6;
 *   region = (x + 1 + (x >> 8)) >> 8;
 *
 * That removes the UDIV emitted by GCC on every streamed LED while preserving
 * the exact QMK region/remainder and CIE1931 brightness semantics.
 */
static inline __attribute__((always_inline)) rgb_t sharkoon_direct_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v) {
#    ifdef USE_CIE1931_CURVE
    v = pgm_read_byte(&CIE1931_CURVE[v]);
#    endif

    if (s == 0) {
        return (rgb_t){v, v, v};
    }

    const uint16_t h6 = (uint16_t)h * 6u;
    const uint8_t region = (uint8_t)((h6 + 1u + (h6 >> 8)) >> 8);
    const uint8_t remainder = (uint8_t)(h6 - ((uint16_t)region * 255u));

    const uint8_t p = (uint8_t)(((uint16_t)v * (255u - s)) >> 8);
    const uint8_t q = (uint8_t)(((uint16_t)v * (255u - (((uint16_t)s * remainder) >> 8))) >> 8);
    const uint8_t t = (uint8_t)(((uint16_t)v * (255u - (((uint16_t)s * (255u - remainder)) >> 8))) >> 8);

    switch (region) {
        case 6:
        case 0:
            return (rgb_t){v, t, p};
        case 1:
            return (rgb_t){q, v, p};
        case 2:
            return (rgb_t){p, v, t};
        case 3:
            return (rgb_t){p, q, v};
        case 4:
            return (rgb_t){t, p, v};
        default:
            return (rgb_t){v, p, q};
    }
}

/*
 * Called from via_command_kb() before Vial processes the packet. The packet
 * format is: command, subcommand, first LED lo/hi, count, then count * HSV.
 * Rendering cannot run until the VIA command returns, and Vial writes the same
 * clamped HSV values to g_direct_mode_colors[] immediately afterwards.
 */
void sharkoon_direct_mark_dirty_packet(const uint8_t *data, uint8_t length) {
    if (length < 5) {
        return;
    }

    const uint16_t first_index = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    const uint8_t  num_leds    = data[4];

    if ((uint16_t)num_leds * 3u > (uint16_t)(length - 5u) ||
        first_index >= RGB_MATRIX_LED_COUNT) {
        return;
    }

    uint16_t end = first_index + num_leds;
    if (end > RGB_MATRIX_LED_COUNT) {
        end = RGB_MATRIX_LED_COUNT;
    }

    const uint8_t *src = &data[5];

    for (uint16_t led = first_index; led < end; ++led, src += 3) {
        const uint8_t raw_v = src[2];
        const uint8_t value = raw_v > RGB_MATRIX_MAXIMUM_BRIGHTNESS
                                  ? RGB_MATRIX_MAXIMUM_BRIGHTNESS
                                  : raw_v;

        sharkoon_direct_rgb_cache[led] =
            sharkoon_direct_hsv_to_rgb(src[0], src[1], value);

#ifdef SHARKOON_PERF_BENCHMARK
        ++sharkoon_perf_rgb_stats.direct_cache_misses;
#endif
    }
}

static inline rgb_t sharkoon_direct_get_rgb(uint8_t led) {
    return sharkoon_direct_rgb_cache[led];
}

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

static inline hsv_t sharkoon_unpack_hsv16(uint16_t packed) {
    return (hsv_t) {
        .h = (uint8_t)packed,
        .s = ((uint8_t)(packed >> 8) & 0x0F) * 17,
        .v = ((uint8_t)(packed >> 12) & 0x0F) * 17,
    };
}

static inline hsv_t sharkoon_palette_interp_raw(const hsv_t *palette, uint8_t x) {
    /*
     * Gleiche Positionierung wie PaletteFx.
     */
    x =
        (x <= 8)
            ? 0
            : ((x < 247)
                   ? (x - 8)
                   : 239);

    const uint8_t i    = x >> 4;
    const uint8_t frac = x << 4;
    const hsv_t   a    = palette[i];
    const hsv_t   b    = palette[i + 1];

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
 * The folded flow function below can only produce even values 0..254.
 * Therefore the complete palette/interpolation/phosphor correction stage has
 * exactly 128 possible RGB results. Cache those results until either the
 * selected PaletteFx palette or the low-brightness hue floor changes.
 *
 * This removes palette interpolation and HSV -> RGB conversion from the
 * per-LED render hot path without approximating the effect.
 */
static rgb_t           sharkoon_flow_rgb_cache[SHARKOON_FLOW_COLOR_COUNT];
static const uint16_t *sharkoon_flow_cache_palette;
static uint8_t         sharkoon_flow_cache_minimum_hue;
static bool            sharkoon_flow_cache_valid;

static void sharkoon_update_flow_rgb_cache(const uint16_t *palette_data, uint8_t minimum_hue) {
    if (sharkoon_flow_cache_valid &&
        sharkoon_flow_cache_palette == palette_data &&
        sharkoon_flow_cache_minimum_hue == minimum_hue) {
        return;
    }

    hsv_t palette[SHARKOON_PALETTE_COLOR_COUNT];

    for (uint8_t i = 0; i < SHARKOON_PALETTE_COLOR_COUNT; ++i) {
        palette[i] =
            sharkoon_unpack_hsv16(
                pgm_read_word(&palette_data[i])
            );
    }

    for (uint8_t i = 0; i < SHARKOON_FLOW_COLOR_COUNT; ++i) {
        const uint8_t flow_value = i << 1;

        hsv_t hsv =
            sharkoon_palette_interp_raw(
                palette,
                flow_value
            );

        if (hsv.v > 220) {
            hsv.v =
                220 +
                scale8(
                    hsv.v - 220,
                    128
                );
        }

        if (hsv.h < minimum_hue) {
            hsv.h = minimum_hue;
        }

        sharkoon_flow_rgb_cache[i] = rgb_matrix_hsv_to_rgb(hsv);
    }

    sharkoon_flow_cache_palette     = palette_data;
    sharkoon_flow_cache_minimum_hue = minimum_hue;
    sharkoon_flow_cache_valid       = true;
}

/*
 * RGB-Farbe Richtung Weiß mischen.
 *
 * amount == 0 ist der häufigste Fall außerhalb von Sparkles/Reactive-Zonen
 * und kann ohne drei scale8/qadd8-Paare sofort zurückkehren.
 */
static inline rgb_t sharkoon_mix_white(rgb_t rgb, uint8_t amount) {
    if (amount == 0) {
        return rgb;
    }

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
 * Alles, was innerhalb eines vollständigen RGB-Frames konstant bleibt, wird
 * einmal bei params->iter == 0 vorbereitet. QMK kann denselben Frame über
 * mehrere RGB_MATRIX_LED_PROCESS_LIMIT-Chunks verteilen; diese Chunks lesen
 * danach nur noch den Snapshot.
 */
typedef struct {
    bool    direct_stream_active;
    uint16_t flow_time;
    int8_t  rot_c;
    int8_t  rot_s;
    uint8_t omega;
    uint8_t sparkle_time;
    uint8_t effective_value;
    uint8_t hit_count;
    uint8_t reactive_value[RGB_MATRIX_LED_COUNT];
} sharkoon_hybrid_frame_t;

static sharkoon_hybrid_frame_t sharkoon_hybrid_frame;
static bool                    sharkoon_hybrid_reactive_nonzero;
static bool                    sharkoon_hybrid_reactive_wake_pending;

/*
 * Event-side race fence only. QMK's g_last_hit_tracker remains the sole source
 * of hit identity/age. If a matching key event arrives after QMK snapshotted
 * last_hit_buffer for the current RGB frame, source-buffer compaction waits for
 * the next source-bearing frame so that newly appended hits cannot be discarded.
 */
void sharkoon_hybrid_reactive_process_key_event(keyrecord_t *record) {
#    if defined(RGB_MATRIX_KEYRELEASES)
    if (record->event.pressed) {
        return;
    }
#    elif defined(RGB_MATRIX_KEYPRESSES)
    if (!record->event.pressed) {
        return;
    }
#    else
    (void)record;
    return;
#    endif

    sharkoon_hybrid_reactive_wake_pending = true;
}

/*
 * Remove an already-finished oldest prefix from QMK's source hit buffer.
 *
 * The source is compacted only when its count still matches the frame snapshot
 * and no matching key event was observed after that snapshot. This keeps the
 * mutation local to the Hybrid effect while preserving any hit that QMK may
 * have appended for the next frame.
 */
static void sharkoon_hybrid_drop_source_prefix(uint8_t drop_count, uint8_t snapshot_count) {
    if (drop_count == 0 || last_hit_buffer.count != snapshot_count) {
        return;
    }

    if (drop_count >= snapshot_count) {
        last_hit_buffer.count = 0;
        return;
    }

    const uint8_t remaining = snapshot_count - drop_count;

    memmove(&last_hit_buffer.x[0], &last_hit_buffer.x[drop_count], remaining);
    memmove(&last_hit_buffer.y[0], &last_hit_buffer.y[drop_count], remaining);
    memmove(&last_hit_buffer.index[0], &last_hit_buffer.index[drop_count], remaining);
    memmove(
        &last_hit_buffer.tick[0],
        &last_hit_buffer.tick[drop_count],
        remaining * sizeof(last_hit_buffer.tick[0])
    );

    last_hit_buffer.count = remaining;
}

static void sharkoon_prepare_hybrid_frame(void) {
    sharkoon_hybrid_frame.direct_stream_active = sharkoon_direct_stream_active();

    /*
     * Im echten Direct-Modus wird der gesamte restliche Hybrid-State nicht
     * gebraucht. Den Snapshot erst vorbereiten, wenn tatsächlich gerendert wird.
     */
    if (sharkoon_hybrid_frame.direct_stream_active) {
        return;
    }

    const uint16_t *palette = palettefx_get_palette_data();
    const uint8_t   speed   = rgb_matrix_config.speed;
    const uint8_t global_value = rgb_matrix_config.hsv.v;

    /*
     * =========================================================
     * FLOW STATE
     * =========================================================
     */
    sharkoon_hybrid_frame.flow_time =
        palettefx_scaled_time(
            g_rgb_timer,
            1 + speed / 8
        );

    /*
     * Reuse the exact 256-entry sin8 LUT that the per-LED flow path already
     * uses. QMK's cos8(theta) is exactly sin8(theta + 64), so all three
     * frame-level trig calls can become direct flash reads with identical
     * uint8_t wrap semantics.
     */
    const uint8_t flow_phase_quarter =
        (uint8_t)(sharkoon_hybrid_frame.flow_time / 4);

    sharkoon_hybrid_frame.rot_c =
        (int8_t)(
            pgm_read_byte(
                &sharkoon_sin8_lut[(uint8_t)(flow_phase_quarter + 64u)]
            ) - 128
        );

    sharkoon_hybrid_frame.rot_s =
        (int8_t)(
            pgm_read_byte(
                &sharkoon_sin8_lut[flow_phase_quarter]
            ) - 128
        );

    sharkoon_hybrid_frame.omega =
        32 +
        pgm_read_byte(
            &sharkoon_sin8_lut[(uint8_t)sharkoon_hybrid_frame.flow_time]
        ) / 4;

    sharkoon_hybrid_frame.sparkle_time =
        (uint8_t)sharkoon_hybrid_frame.flow_time;

    /*
     * =========================================================
     * LOW-BRIGHTNESS / GLOBAL BRIGHTNESS STATE
     * =========================================================
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

    sharkoon_update_flow_rgb_cache(palette, minimum_hue);

    sharkoon_hybrid_frame.effective_value =
        scale8(
            global_value,
            64 + scale8(global_value, 191)
        );

    /*
     * =========================================================
     * REACTIVE STATE
     * =========================================================
     *
     * QMK stores hits oldest -> newest. Hit age therefore increases while
     * walking the snapshot backwards. Once an old hit has reached amplitude 0,
     * every hit before it is older and can never contribute either.
     *
     * Build the complete sparse reactive field directly in that reverse walk.
     * This removes the former active_hits[] staging buffer and its second hit
     * pass. The first finished hit also identifies a dead oldest prefix that
     * can be compacted out of QMK's source buffer when the frame snapshot is
     * known to be current.
     */
    sharkoon_hybrid_frame.hit_count = 0;

    const uint8_t source_hit_count = g_last_hit_tracker.count;

    /*
     * True idle needs no fence traffic. If an event raced a still-empty
     * snapshot, keep the fence armed until the first source-bearing frame;
     * that is the first frame where source compaction could matter anyway.
     */
    if (source_hit_count == 0) {
        if (sharkoon_hybrid_reactive_nonzero) {
            memset(
                sharkoon_hybrid_frame.reactive_value,
                0,
                sizeof(sharkoon_hybrid_frame.reactive_value)
            );
            sharkoon_hybrid_reactive_nonzero = false;
        }
        return;
    }

    const bool source_mutation_fenced = sharkoon_hybrid_reactive_wake_pending;
    sharkoon_hybrid_reactive_wake_pending = false;

    const uint8_t reactive_scale = 1 + speed / 4;
    uint8_t       dead_prefix    = 0;
    bool          field_cleared  = false;

    for (uint8_t hit = source_hit_count; hit > 0; --hit) {
        const uint8_t j = hit - 1u;

        const uint16_t tick =
            scale16by8(
                g_last_hit_tracker.tick[j],
                reactive_scale
            );

        uint8_t amplitude = 0;

        if (tick <= 255) {
            const uint8_t t = (uint8_t)tick;

            if (t <= 55) {
                amplitude =
                    (t < 32)
                        ? (4 + 8 * t)
                        : 255;
            } else {
                const uint8_t decay =
                    (
                        ((uint16_t)(255 - t)) *
                        UINT16_C(164)
                    ) >> 7;

                amplitude =
                    scale8(
                        decay,
                        decay
                    );
            }
        }

        /*
         * Newer hits were already processed. Because this hit is now finished,
         * all still-older entries form one removable prefix.
         */
        if (amplitude == 0) {
            dead_prefix = hit;
            break;
        }

        if (!field_cleared) {
            memset(
                sharkoon_hybrid_frame.reactive_value,
                0,
                sizeof(sharkoon_hybrid_frame.reactive_value)
            );
            field_cleared = true;
        }

        ++sharkoon_hybrid_frame.hit_count;

        const uint8_t source_led = g_last_hit_tracker.index[j];

        const uint16_t pair_begin =
            pgm_read_word(
                &sharkoon_reactive_sparse_offsets[source_led]
            );

        const uint16_t pair_end =
            pgm_read_word(
                &sharkoon_reactive_sparse_offsets[source_led + 1u]
            );

        for (uint16_t pair_index = pair_begin; pair_index < pair_end; ++pair_index) {
            const uint16_t pair =
                pgm_read_word(
                    &sharkoon_reactive_sparse_pairs[pair_index]
                );

            const uint8_t target_led = (uint8_t)(pair >> 8);
            const uint8_t current    = sharkoon_hybrid_frame.reactive_value[target_led];

            if (current == 255) {
                continue;
            }

            sharkoon_hybrid_frame.reactive_value[target_led] =
                qadd8(
                    current,
                    scale8(
                        (uint8_t)pair,
                        amplitude
                    )
                );
        }
    }

    if (sharkoon_hybrid_frame.hit_count != 0) {
        sharkoon_hybrid_reactive_nonzero = true;
    } else if (sharkoon_hybrid_reactive_nonzero) {
        memset(
            sharkoon_hybrid_frame.reactive_value,
            0,
            sizeof(sharkoon_hybrid_frame.reactive_value)
        );
        sharkoon_hybrid_reactive_nonzero = false;
    }

    /*
     * A matching event after the snapshot fences source mutation for exactly
     * this frame. Otherwise remove every finished oldest hit immediately so
     * QMK's rgb_task_timers() only ages hits that can still affect Hybrid.
     */
    if (!source_mutation_fenced && dead_prefix != 0) {
        sharkoon_hybrid_drop_source_prefix(dead_prefix, source_hit_count);
    }

}

/*
 * =============================================================
 * VIALRGB DIRECT / SHARKOON HYBRID
 * =============================================================
 */

static bool VIALRGB_DIRECT(effect_params_t *params) {
    RGB_MATRIX_USE_LIMITS(led_min, led_max);

#ifdef SHARKOON_PERF_BENCHMARK
    const uint32_t perf_chunk_start = sharkoon_perf_cycle_now();
#endif

    if (params->iter == 0) {
#ifdef SHARKOON_PERF_BENCHMARK
        const uint32_t perf_prepare_start = sharkoon_perf_cycle_now();
#endif
        sharkoon_prepare_hybrid_frame();
#ifdef SHARKOON_PERF_BENCHMARK
        const uint32_t perf_prepare_cycles = sharkoon_perf_cycle_now() - perf_prepare_start;
        sharkoon_perf_accumulate(
            &sharkoon_perf_rgb_stats.hybrid_prepare_sum,
            &sharkoon_perf_rgb_stats.hybrid_prepare_max,
            &sharkoon_perf_rgb_stats.hybrid_prepare_count,
            perf_prepare_cycles
        );

        if (!sharkoon_hybrid_frame.direct_stream_active) {
            ++sharkoon_perf_rgb_stats.hybrid_frame_count;
            sharkoon_perf_rgb_stats.active_hit_sum += sharkoon_hybrid_frame.hit_count;
            if (sharkoon_hybrid_frame.hit_count > sharkoon_perf_rgb_stats.active_hit_max) {
                sharkoon_perf_rgb_stats.active_hit_max = sharkoon_hybrid_frame.hit_count;
            }
        }
#endif
    }

    /*
     * =========================================================
     * REAL DIRECT RGB
     * =========================================================
     *
     * Direct/Hybrid wird einmal pro vollständigem Frame entschieden. Dadurch
     * kann ein 250-ms-Timeout nicht mehr mitten zwischen zwei LED-Chunks einen
     * halben Direct-/halben Hybrid-Frame erzeugen.
     */
    if (sharkoon_hybrid_frame.direct_stream_active) {
        for (uint8_t i = led_min; i < led_max; ++i) {
            const rgb_t rgb = sharkoon_direct_get_rgb(i);

            rgb_matrix_set_color(
                i,
                rgb.r,
                rgb.g,
                rgb.b
            );
        }

#ifdef SHARKOON_PERF_BENCHMARK
        const uint32_t perf_direct_cycles = sharkoon_perf_cycle_now() - perf_chunk_start;
        sharkoon_perf_accumulate(
            &sharkoon_perf_rgb_stats.direct_chunk_sum,
            &sharkoon_perf_rgb_stats.direct_chunk_max,
            &sharkoon_perf_rgb_stats.direct_chunk_count,
            perf_direct_cycles
        );
#endif
        return rgb_matrix_check_finished_leds(led_max);
    }

    const uint16_t flow_time         = sharkoon_hybrid_frame.flow_time;
    const int8_t   rot_c             = sharkoon_hybrid_frame.rot_c;
    const int8_t   rot_s             = sharkoon_hybrid_frame.rot_s;
    const uint8_t  omega             = sharkoon_hybrid_frame.omega;
    const uint8_t  sparkle_time      = sharkoon_hybrid_frame.sparkle_time;
    const uint8_t  effective_value   = sharkoon_hybrid_frame.effective_value;
    const bool     reactive_active   = sharkoon_hybrid_frame.hit_count != 0;

    /*
     * Sparkle only needs a cheap, visually irregular per-LED phase stream.
     * Keep the state in a native 32-bit register and only use its low byte as
     * the phase. The five normal render chunks start far apart on the full
     * 256-state low-byte cycle, avoiding short repeated subsequences between
     * neighboring chunks without any LUT or chunk-size dependency.
     */
    uint32_t sparkle_state =
        (uint8_t)(1u - ((uint8_t)params->iter << 4));

    /*
     * =========================================================
     * RENDER LEDs
     * =========================================================
     */
    for (uint8_t i = led_min; i < led_max; ++i) {
        RGB_MATRIX_TEST_LED_FLAGS();

#ifdef SHARKOON_PERF_BENCHMARK
        const uint32_t perf_t0 = sharkoon_perf_cycle_now();
#endif

        /*
         * =====================================================
         * 1. FLOW
         * =====================================================
         */
        const uint8_t x = g_led_config.point[i].x;
        const uint8_t y = g_led_config.point[i].y;

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
                pgm_read_byte(
                    &sharkoon_sin8_lut[(uint8_t)(x1 - 2 * flow_time)]
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
         * FLOW RGB FAST PATH
         *
         * flow_value is always even after the fold above. Palette
         * interpolation, highlight compression, the low-brightness hue floor
         * and HSV -> RGB conversion are therefore represented exactly by one
         * of 128 cached RGB entries. Global brightness is still applied later.
         */
        rgb_t rgb = sharkoon_flow_rgb_cache[flow_value >> 1];

#ifdef SHARKOON_PERF_BENCHMARK
        const uint32_t perf_t1 = sharkoon_perf_cycle_now();
#endif

        /*
         * =====================================================
         * 2. SPARKLE
         * =====================================================
         */
        sparkle_state = sparkle_state * 17u + 29u;

        const uint8_t sparkle_phase =
            (uint8_t)sparkle_state;

        const uint8_t sparkle_white =
            pgm_read_byte(
                &sharkoon_sparkle_white_lut[(uint8_t)(2 * sparkle_time + sparkle_phase)]
            );

        if (sparkle_white != 0) {
            rgb =
                sharkoon_mix_white(
                    rgb,
                    sparkle_white
                );
        }

#ifdef SHARKOON_PERF_BENCHMARK
        const uint32_t perf_t2 = sharkoon_perf_cycle_now();
#endif

        /*
         * =====================================================
         * 3. REACTIVE
         * =====================================================
         */
        /*
         * hit_count is frame-constant. In true idle there is no reactive field
         * to inspect, so avoid one array load plus the per-LED zero test.
         * Active frames keep the exact same reactive_value path as before.
         */
        if (reactive_active) {
            const uint8_t reactive_value =
                sharkoon_hybrid_frame.reactive_value[i];

            if (reactive_value != 0) {
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
            }
        }

#ifdef SHARKOON_PERF_BENCHMARK
        const uint32_t perf_t3 = sharkoon_perf_cycle_now();
#endif

        /*
         * =====================================================
         * 4. GLOBAL BRIGHTNESS
         * =====================================================
         *
         * effective_value wurde einmal pro Frame berechnet. Nur bei
         * FASTLED_SCALE8_FIXED == 1 ist scale8(channel, 255) exakt die
         * Identitaet; dann koennen wir diesen Sonderfall komplett sparen.
         */
#if FASTLED_SCALE8_FIXED == 1
        if (effective_value != 255) {
#endif
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
#if FASTLED_SCALE8_FIXED == 1
        }
#endif

#ifdef SHARKOON_PERF_BENCHMARK
        const uint32_t perf_t4 = sharkoon_perf_cycle_now();
#endif

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

#ifdef SHARKOON_PERF_BENCHMARK
        const uint32_t perf_t5 = sharkoon_perf_cycle_now();

        sharkoon_perf_accumulate_sample(&sharkoon_perf_rgb_stats.flow_sum,       &sharkoon_perf_rgb_stats.flow_max,       perf_t1 - perf_t0);
        sharkoon_perf_accumulate_sample(&sharkoon_perf_rgb_stats.sparkle_sum,    &sharkoon_perf_rgb_stats.sparkle_max,    perf_t2 - perf_t1);
        sharkoon_perf_accumulate_sample(&sharkoon_perf_rgb_stats.reactive_sum,   &sharkoon_perf_rgb_stats.reactive_max,   perf_t3 - perf_t2);
        sharkoon_perf_accumulate_sample(&sharkoon_perf_rgb_stats.brightness_sum, &sharkoon_perf_rgb_stats.brightness_max, perf_t4 - perf_t3);
        sharkoon_perf_accumulate_sample(&sharkoon_perf_rgb_stats.output_sum,     &sharkoon_perf_rgb_stats.output_max,     perf_t5 - perf_t4);
        ++sharkoon_perf_rgb_stats.led_samples;
#endif
    }

#ifdef SHARKOON_PERF_BENCHMARK
    const uint32_t perf_hybrid_cycles = sharkoon_perf_cycle_now() - perf_chunk_start;
    sharkoon_perf_accumulate(
        &sharkoon_perf_rgb_stats.hybrid_chunk_sum,
        &sharkoon_perf_rgb_stats.hybrid_chunk_max,
        &sharkoon_perf_rgb_stats.hybrid_chunk_count,
        perf_hybrid_cycles
    );
#endif

    return rgb_matrix_check_finished_leds(led_max);
}

#endif
