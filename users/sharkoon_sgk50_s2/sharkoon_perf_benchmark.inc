/*
 * Cortex-M3 debug/trace registers. The WB32FQ95 contains the architectural
 * DWT cycle counter, which gives us exact on-device CPU-cycle timing without
 * routing the measured code through USB logging.
 */
#    define SHARKOON_DEMCR      (*(volatile uint32_t *)UINT32_C(0xE000EDFC))
#    define SHARKOON_DWT_CTRL   (*(volatile uint32_t *)UINT32_C(0xE0001000))
#    define SHARKOON_DWT_CYCCNT (*(volatile uint32_t *)UINT32_C(0xE0001004))
#    define SHARKOON_DEMCR_TRCENA UINT32_C(0x01000000)
#    define SHARKOON_DWT_CYCCNTENA UINT32_C(0x00000001)
#    define SHARKOON_PERF_REPORT_SCANS 2048u
#    define SHARKOON_PERF_COLD_IDLE_MS 500u

static uint32_t sharkoon_perf_last_scan_cycle;
static uint32_t sharkoon_perf_last_scan_gap;
static uint32_t sharkoon_perf_last_key_event_ms;

static uint32_t sharkoon_perf_cold_gap_sum;
static uint32_t sharkoon_perf_cold_gap_max;
static uint32_t sharkoon_perf_cold_process_sum;
static uint32_t sharkoon_perf_cold_process_max;
static uint16_t sharkoon_perf_cold_count;

static void sharkoon_perf_enable_dwt(void) {
    SHARKOON_DEMCR |= SHARKOON_DEMCR_TRCENA;
    SHARKOON_DWT_CYCCNT = 0;
    SHARKOON_DWT_CTRL |= SHARKOON_DWT_CYCCNTENA;
}

static inline uint32_t sharkoon_perf_average(uint32_t sum, uint32_t count) {
    return count != 0 ? sum / count : 0;
}

/*
 * Record the first physical key press after a genuinely quiet period.
 *
 * This intentionally runs only on key events; there is no extra idle polling.
 * The two measurements answer slightly different questions:
 *
 *   gap          = matrix-scan interval in which the cold key was observed
 *   scan2process = cycles from the most recent matrix_scan_user() timestamp
 *                  until QMK reaches pre_process_record_user() for that key
 *
 * It is not end-to-end USB latency, but it lets us compare the firmware path
 * for the first press after idle with identical instrumentation on both builds.
 */
static void sharkoon_perf_note_key_event(keyrecord_t *record) {
    const uint32_t now_ms = timer_read32();

    if (record->event.pressed &&
        sharkoon_perf_last_key_event_ms != 0 &&
        (uint32_t)(now_ms - sharkoon_perf_last_key_event_ms) >= SHARKOON_PERF_COLD_IDLE_MS &&
        sharkoon_perf_last_scan_cycle != 0 &&
        sharkoon_perf_last_scan_gap != 0) {
        const uint32_t now_cycle       = SHARKOON_DWT_CYCCNT;
        const uint32_t scan_to_process = now_cycle - sharkoon_perf_last_scan_cycle;
        const uint32_t gap             = sharkoon_perf_last_scan_gap;

        sharkoon_perf_cold_gap_sum += gap;
        if (gap > sharkoon_perf_cold_gap_max) {
            sharkoon_perf_cold_gap_max = gap;
        }

        sharkoon_perf_cold_process_sum += scan_to_process;
        if (scan_to_process > sharkoon_perf_cold_process_max) {
            sharkoon_perf_cold_process_max = scan_to_process;
        }

        ++sharkoon_perf_cold_count;
    }

    /* Releases count as activity too: cold means no physical key event at all. */
    sharkoon_perf_last_key_event_ms = now_ms;
}

void matrix_scan_user(void) {
    static uint32_t previous_cycle;
    static uint32_t scan_gap_sum;
    static uint32_t scan_gap_max;
    static uint16_t scan_gap_count;

    const uint32_t now = SHARKOON_DWT_CYCCNT;

    if (previous_cycle != 0) {
        const uint32_t gap = now - previous_cycle;
        scan_gap_sum += gap;
        if (gap > scan_gap_max) {
            scan_gap_max = gap;
        }
        ++scan_gap_count;
        sharkoon_perf_last_scan_gap = gap;
    }

    previous_cycle                 = now;
    sharkoon_perf_last_scan_cycle = now;

    if (scan_gap_count < SHARKOON_PERF_REPORT_SCANS) {
        return;
    }

    sharkoon_perf_rgb_stats_t rgb;
    sharkoon_perf_rgb_take_snapshot(&rgb);

    uprintf(
        "PERF scan cyc avg/max=%lu/%lu n=%u\r\n",
        (unsigned long)sharkoon_perf_average(scan_gap_sum, scan_gap_count),
        (unsigned long)scan_gap_max,
        scan_gap_count
    );

    uprintf(
        "PERF cold-key idle>=%ums gap avg/max=%lu/%lu scan2process avg/max=%lu/%lu n=%u\r\n",
        SHARKOON_PERF_COLD_IDLE_MS,
        (unsigned long)sharkoon_perf_average(sharkoon_perf_cold_gap_sum, sharkoon_perf_cold_count),
        (unsigned long)sharkoon_perf_cold_gap_max,
        (unsigned long)sharkoon_perf_average(sharkoon_perf_cold_process_sum, sharkoon_perf_cold_count),
        (unsigned long)sharkoon_perf_cold_process_max,
        sharkoon_perf_cold_count
    );

    uprintf(
        "PERF chunks cyc solid=%lu/%lu n=%lu direct=%lu/%lu n=%lu hybrid=%lu/%lu n=%lu prep=%lu/%lu n=%lu misses=%lu\r\n",
        (unsigned long)sharkoon_perf_average(rgb.solid_chunk_sum, rgb.solid_chunk_count),
        (unsigned long)rgb.solid_chunk_max,
        (unsigned long)rgb.solid_chunk_count,
        (unsigned long)sharkoon_perf_average(rgb.direct_chunk_sum, rgb.direct_chunk_count),
        (unsigned long)rgb.direct_chunk_max,
        (unsigned long)rgb.direct_chunk_count,
        (unsigned long)sharkoon_perf_average(rgb.hybrid_chunk_sum, rgb.hybrid_chunk_count),
        (unsigned long)rgb.hybrid_chunk_max,
        (unsigned long)rgb.hybrid_chunk_count,
        (unsigned long)sharkoon_perf_average(rgb.hybrid_prepare_sum, rgb.hybrid_prepare_count),
        (unsigned long)rgb.hybrid_prepare_max,
        (unsigned long)rgb.hybrid_prepare_count,
        (unsigned long)rgb.direct_cache_misses
    );

    uprintf(
        "PERF hybrid-led cyc flow=%lu/%lu sparkle=%lu/%lu reactive=%lu/%lu bright=%lu/%lu out=%lu/%lu samples=%lu hits/frame=%lu max=%lu\r\n",
        (unsigned long)sharkoon_perf_average(rgb.flow_sum, rgb.led_samples),
        (unsigned long)rgb.flow_max,
        (unsigned long)sharkoon_perf_average(rgb.sparkle_sum, rgb.led_samples),
        (unsigned long)rgb.sparkle_max,
        (unsigned long)sharkoon_perf_average(rgb.reactive_sum, rgb.led_samples),
        (unsigned long)rgb.reactive_max,
        (unsigned long)sharkoon_perf_average(rgb.brightness_sum, rgb.led_samples),
        (unsigned long)rgb.brightness_max,
        (unsigned long)sharkoon_perf_average(rgb.output_sum, rgb.led_samples),
        (unsigned long)rgb.output_max,
        (unsigned long)rgb.led_samples,
        (unsigned long)sharkoon_perf_average(rgb.active_hit_sum, rgb.hybrid_frame_count),
        (unsigned long)rgb.active_hit_max
    );

    scan_gap_sum   = 0;
    scan_gap_max   = 0;
    scan_gap_count = 0;

    sharkoon_perf_cold_gap_sum     = 0;
    sharkoon_perf_cold_gap_max     = 0;
    sharkoon_perf_cold_process_sum = 0;
    sharkoon_perf_cold_process_max = 0;
    sharkoon_perf_cold_count       = 0;

    /* Console output itself must not become the next measured scan gap. */
    previous_cycle                 = SHARKOON_DWT_CYCCNT;
    sharkoon_perf_last_scan_cycle = previous_cycle;
    sharkoon_perf_last_scan_gap   = 0;
}
