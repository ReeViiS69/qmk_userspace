#pragma once

#include "quantum.h"

#ifdef RGB_MATRIX_ENABLE
void sharkoon_apply_disabled_led_flags(void);
void sharkoon_clear_disabled_leds(void);
void sharkoon_solid_reactive_process_key_event(keyrecord_t *record);
#endif

#ifdef SHARKOON_PERF_BENCHMARK
typedef struct {
    uint32_t solid_chunk_sum;
    uint32_t solid_chunk_max;
    uint32_t solid_chunk_count;

    uint32_t direct_chunk_sum;
    uint32_t direct_chunk_max;
    uint32_t direct_chunk_count;
    uint32_t direct_cache_misses;

    uint32_t hybrid_prepare_sum;
    uint32_t hybrid_prepare_max;
    uint32_t hybrid_prepare_count;

    uint32_t hybrid_chunk_sum;
    uint32_t hybrid_chunk_max;
    uint32_t hybrid_chunk_count;

    uint32_t flow_sum;
    uint32_t flow_max;
    uint32_t sparkle_sum;
    uint32_t sparkle_max;
    uint32_t reactive_sum;
    uint32_t reactive_max;
    uint32_t brightness_sum;
    uint32_t brightness_max;
    uint32_t output_sum;
    uint32_t output_max;
    uint32_t led_samples;

    uint32_t hybrid_frame_count;
    uint32_t active_hit_sum;
    uint32_t active_hit_max;
} sharkoon_perf_rgb_stats_t;

void sharkoon_perf_rgb_take_snapshot(sharkoon_perf_rgb_stats_t *out);
#endif
