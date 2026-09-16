/*
 * WS2812 GPIO DMA Driver for WB32FQ95xx (Bridge75 Custom Implementation)
 *
 * Non-circular sequential block DMA with double-buffer architecture.
 * Timer-triggered DMA writes GPIO BSRR values using 3-phase bit encoding.
 *
 * Why GPIO+DMA works where PWM+DMA fails:
 * - PWM+DMA: Timer keeps running during ISR chunk transitions, generating
 *   extra PWM cycles with stale CCR values (data corruption)
 * - GPIO+DMA: GPIO pin holds its last state during ISR pause. Since we
 *   always end each bit LOW, the pause is just an extended low period
 *   which is within WS2812 spec (< 9µs reset threshold)
 *
 * Architecture:
 * - Double buffer: 2 × 504 phases, ISR chains next block on TFR interrupt
 * - One immutable RGB snapshot protects the in-flight frame from later writes
 * - A high-priority ChibiOS worker refills the just-released DMA buffer
 * - The ISR only chains the prepared buffer and wakes the worker; encoding
 *   never runs in interrupt context
 * - Block size 504: ≤511 hardware max, divisible by 72 (LED alignment)
 * - Reset: 300µs sleep in the worker after final DMA block (pin already LOW)
 *
 * Timing (3 phases per bit at 72MHz, PSC=0, ARR=WS2812_PHASE_TICKS-1):
 *   Default 25 ticks = 347ns per phase (configurable via WS2812_PHASE_TICKS).
 *   Phase 1: Always SET (go high)
 *   Phase 2: Bit 0: RESET (go low), Bit 1: NOP (stay high)
 *   Phase 3: Bit 0: NOP (stay low), Bit 1: RESET (go low)
 *
 * Copyright 2026 emolitor (github.com/emolitor)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ws2812.h"
#include "gpio.h"
#include "timer.h"
#include "chibios_config.h"

#include <string.h>

#if defined(WB32F3G71xx) || defined(WB32FQ95xx)

#include "hal.h"

/* Validate required configuration */
#ifndef WS2812_DI_PIN
#    error "WS2812_GPIO_DMA driver requires WS2812_DI_PIN to be defined"
#endif

/* Guard against exceeding double-buffer capacity.
 * 168 LEDs × 72 phases = 12096 = 24 blocks of 504. Beyond this,
 * the fill-time margin may be insufficient. */
#if WS2812_LED_COUNT > 168
#    error "WS2812_LED_COUNT exceeds double-buffer capacity (max 168)"
#endif

/* GPIO BSRR configuration - extract port and pin from QMK PAL line definition */
#ifndef WS2812_GPIO_PORT
#    define WS2812_GPIO_PORT PAL_PORT(WS2812_DI_PIN)
#endif

#ifndef WS2812_GPIO_PIN_NUM
#    define WS2812_GPIO_PIN_NUM PAL_PAD(WS2812_DI_PIN)
#endif

/* BSRR values for SET (bits 0-15) and RESET (bits 16-31) */
#define BSRR_SET   (1U << WS2812_GPIO_PIN_NUM)
#define BSRR_RESET (1U << (WS2812_GPIO_PIN_NUM + 16))
#define BSRR_NOP   0x00000000U

/* Timer configuration */
#ifndef WS2812_GPIO_DMA_TIMER
#    define WS2812_GPIO_DMA_TIMER GPTD3
#endif

#ifndef WS2812_GPIO_DMA_TIM_HWHIF
#    define WS2812_GPIO_DMA_TIM_HWHIF WB32_DMAC_HWHIF_TIM3_UP
#endif

/* DMA configuration */
#ifndef WS2812_GPIO_DMA_STREAM
#    define WS2812_GPIO_DMA_STREAM WB32_DMA1_STREAM1
#endif

/* Timing constants at 72MHz.
 * WS2812_PHASE_TICKS controls T0H (one phase HIGH for a zero-bit).
 * Some WS2812 batches reject T0H > ~420ns as a 1-bit (all-white symptom).
 * 25 ticks = 347ns matches the proven bitbang WS2812_T0H=350 timing. */
#define WS2812_TIMER_FREQ    72000000U
#ifndef WS2812_PHASE_TICKS
#    define WS2812_PHASE_TICKS   27U    /* 27 ticks × 13.89ns = 375ns at 72MHz */
#endif

/* Color channel configuration */
#ifdef WS2812_RGBW
#    define WS2812_CHANNELS 4
#else
#    define WS2812_CHANNELS 3
#endif

/* Buffer size calculations */
#define WS2812_BITS_PER_LED    (WS2812_CHANNELS * 8)
#define WS2812_PHASES_PER_BIT  3
#define WS2812_PHASES_PER_LED  (WS2812_BITS_PER_LED * WS2812_PHASES_PER_BIT)  /* 72 phases per LED */

/* Block DMA configuration:
 * - Block size 504: ≤511 hardware max (9-bit BLOCK_TS), divisible by 72 (LED-aligned)
 * - Double buffer: 2 × 504 phases = 4032 bytes
 * - Total LED phases: WS2812_LED_COUNT × 72
 * - Block count: ceil(LED_PHASES / 504)
 */
#define WS2812_BLOCK_SIZE    504U
#define WS2812_LEDS_PER_BLOCK  (WS2812_BLOCK_SIZE / WS2812_PHASES_PER_LED)  /* 7 */
#define WS2812_LED_PHASES    (WS2812_LED_COUNT * WS2812_PHASES_PER_LED)
#define WS2812_BLOCK_COUNT   ((WS2812_LED_PHASES + WS2812_BLOCK_SIZE - 1) / WS2812_BLOCK_SIZE)

/* Reset pulse: 300µs sleep in thread context after final DMA block */
#define WS2812_RESET_US      300U

/* Safety timeout: abort DMA if transfer takes longer than this.
 * Normal 82-LED frame takes ~3ms. Must be shorter than the wireless
 * ACK timeout (MD_SNED_PKT_TIMEOUT = 10ms) to avoid triggering
 * wireless retry/drop logic during DMA stall recovery. */
#define WS2812_TIMEOUT_MS    5U

/* The worker only owns a small call chain (wait -> fill -> encode). 512 bytes
 * of stack is deliberately conservative while still keeping the whole async
 * experiment below ~1 KiB of additional SRAM including the RGB snapshot. */
#define WS2812_WORKER_STACK_SIZE 512U

/* Double buffer for block DMA — 2 buffers give 1-block-period runway
 * (189µs at 27 ticks) which comfortably exceeds fill time (~40µs),
 * eliminating CPU/DMA data race */
static uint32_t ws2812_buf[2][WS2812_BLOCK_SIZE];

/* LED color storage for ws2812_set_color API */
ws2812_led_t ws2812_leds[WS2812_LED_COUNT];

/* Immutable source for the frame currently being transmitted. QMK can update
 * ws2812_leds[] for the next frame while DMA/worker still consume this copy. */
static ws2812_led_t ws2812_frame_leds[WS2812_LED_COUNT];

/* Worker synchronization. The DMA IRQ priority used by this driver is kernel
 * safe, so it can signal the block semaphore through the ChibiOS I-class API. */
static semaphore_t ws2812_start_sem;
static semaphore_t ws2812_block_sem;
static THD_WORKING_AREA(ws2812_worker_wa, WS2812_WORKER_STACK_SIZE);
static thread_t *ws2812_worker_thread;
static volatile bool ws2812_worker_busy;

/* Block DMA state */
static volatile uint32_t ws2812_block_idx;          /* Current block being DMA'd */
static volatile bool     ws2812_transfer_active;
static volatile uint32_t ws2812_buf_block[2];       /* Which block each buffer contains */
static const wb32_dma_stream_t *ws2812_dma_stream;

#ifdef WS2812_DEBUG
static volatile uint32_t ws2812_dma_error_count;    /* DMA error counter for debugging */
static volatile uint32_t ws2812_dma_underrun_count; /* Worker missed one block deadline */
#endif

/* GPT timer driver instance */
static GPTDriver *ws2812_gpt = &WS2812_GPIO_DMA_TIMER;

/* Forward declarations */
static void ws2812_dma_callback(void *p, uint32_t flags);
static void ws2812_fill_block(uint32_t *buf, uint32_t start_phase, uint32_t count);
static void ws2812_abort_transfer(void);
static THD_FUNCTION(ws2812_worker, arg);

/* Timer configuration - no callback, DMA handles transfers */
static const GPTConfig ws2812_gpt_config = {
    .frequency = WS2812_TIMER_FREQ,
    .callback  = NULL,
    .cr2       = 0,
    .dier      = WB32_TIM_DIER_UDE,  /* Enable DMA request on update */
};

/**
 * @brief Get the number of phases in a given block
 */
static inline uint32_t ws2812_get_block_size(uint32_t block) {
    uint32_t remaining = WS2812_LED_PHASES - (block * WS2812_BLOCK_SIZE);
    return (remaining > WS2812_BLOCK_SIZE) ? WS2812_BLOCK_SIZE : remaining;
}

/**
 * @brief Encode one color byte into 24 phases (8 bits × 3 phases)
 *
 * Branchless: uses arithmetic (RSB+MUL on Cortex-M3, both 1 cycle)
 * instead of if/else branches to avoid pipeline stalls under -Os.
 *
 * @param p Pointer to output buffer (24 uint32_t written)
 * @param byte_val Color byte to encode (MSB first)
 * @return Pointer past the 24 written phases
 */
static inline uint32_t *ws2812_encode_byte(uint32_t *p, uint8_t byte_val) {
    for (int bit = 7; bit >= 0; bit--) {
        uint32_t bv = (byte_val >> bit) & 1;
        /* Phase 1: always SET (go high)
         * Phase 2: RESET if bit=0, NOP(0) if bit=1  → (1-bv) * RESET
         * Phase 3: NOP(0) if bit=0, RESET if bit=1  → bv * RESET */
        p[0] = BSRR_SET;
        p[1] = (1 - bv) * BSRR_RESET;
        p[2] = bv * BSRR_RESET;
        p += 3;
    }
    return p;
}

/**
 * @brief Fill a buffer with encoded LED phase data (per-LED, branchless)
 *
 * Block size (504) is always a multiple of phases-per-LED (72), so every
 * block starts on an LED boundary. This allows simple per-LED iteration
 * without mid-LED state tracking or modular arithmetic.
 *
 * Accesses LED data as raw bytes in memory order, which automatically
 * respects WS2812_BYTE_ORDER (the ws2812_led_t struct layout changes
 * with the byte order setting).
 *
 * @param buf Pointer to buffer to fill (WS2812_BLOCK_SIZE elements)
 * @param start_phase Global phase index to start from (LED-aligned)
 * @param count Number of phases to fill (multiple of WS2812_PHASES_PER_LED)
 */
static void ws2812_fill_block(uint32_t *buf, uint32_t start_phase, uint32_t count) {
    uint32_t led_idx  = start_phase / WS2812_PHASES_PER_LED;
    uint32_t num_leds = count / WS2812_PHASES_PER_LED;
    uint32_t *p = buf;

    for (uint32_t n = 0; n < num_leds && led_idx < WS2812_LED_COUNT; n++, led_idx++) {
        /* Access LED data as raw bytes — automatically sends bytes in
         * the correct wire order for any WS2812_BYTE_ORDER. */
        const uint8_t *led_data = (const uint8_t *)&ws2812_frame_leds[led_idx];
        for (uint32_t ch = 0; ch < WS2812_CHANNELS; ch++) {
            p = ws2812_encode_byte(p, led_data[ch]);
        }
    }
}

/**
 * @brief Force-abort an in-progress DMA transfer (thread context only)
 *
 * Called from ws2812_flush() when a timeout is detected.
 * Stops DMA, stops timer, forces pin LOW, clears transfer flag.
 */
static void ws2812_abort_transfer(void) {
    dmaStreamDisable(ws2812_dma_stream);
    /* Clear stale raw status (TFR/ERR) from the aborted transfer.
     * Without this, re-enabling interrupt masks on the next frame would
     * fire a spurious TFR ISR that corrupts block sequencing.
     * (Normal completion doesn't need this — dmaServeInterrupt() clears
     * status at line 470 of wb32_dma.c after the callback returns.) */
    dmaStreamClearInterrupt(ws2812_dma_stream);
    gptStopTimer(ws2812_gpt);
    WS2812_GPIO_PORT->BSRR = BSRR_RESET;
    ws2812_transfer_active = false;
}

/**
 * @brief Wake the refill worker from DMA ISR context
 */
static inline void ws2812_signal_block_worker_from_isr(void) {
    chSysLockFromISR();
    chSemSignalI(&ws2812_block_sem);
    chSysUnlockFromISR();
}

/**
 * @brief DMA callback — chain next block or stop
 *
 * Called from ISR context on TFR (transfer complete) or ERR.
 * On TFR: advance block index, start next DMA block if more remain.
 * On ERR or final block: stop timer, clear transfer_active.
 */
static void ws2812_dma_callback(void *p, uint32_t flags) {
    (void)p;

    if (ws2812_dma_stream == NULL) {
        return;
    }

    /* NOTE: Do NOT call dmaStreamClearInterrupt() here.
     * dmaServeInterrupt() checks each interrupt type individually and
     * calls this callback per-type, then clears ALL status at the end.
     * Clearing early would wipe ERR status before dmaServeInterrupt
     * checks it, silently losing simultaneous DMA errors.
     */

    /* Error — abort */
    if (flags & WB32_DMAC_IT_STATE_ERR) {
#ifdef WS2812_DEBUG
        ws2812_dma_error_count++;
#endif
        dmaStreamDisable(ws2812_dma_stream);
        gptStopTimerI(ws2812_gpt);
        WS2812_GPIO_PORT->BSRR = BSRR_RESET;
        ws2812_transfer_active = false;
        ws2812_signal_block_worker_from_isr();
        return;
    }

    /* TFR — block complete.
     * Guard: dmaServeInterrupt() checks TFR before ERR and calls
     * separately, so if both fired for the same block the ERR handler
     * above already aborted. Skip TFR processing in that case. */
    if ((flags & WB32_DMAC_IT_STATE_TFR) && ws2812_transfer_active) {
        ws2812_block_idx++;

        if (ws2812_block_idx < WS2812_BLOCK_COUNT) {
            /* More blocks — the worker must already have prepared this slot.
             * Abort instead of transmitting stale data if it ever misses the
             * ~189µs refill deadline. */
            const uint32_t buf_sel  = ws2812_block_idx % 2U;
            const uint32_t blk_size = ws2812_get_block_size(ws2812_block_idx);

            if (ws2812_buf_block[buf_sel] != ws2812_block_idx) {
#ifdef WS2812_DEBUG
                ws2812_dma_underrun_count++;
#endif
                dmaStreamDisable(ws2812_dma_stream);
                gptStopTimerI(ws2812_gpt);
                WS2812_GPIO_PORT->BSRR = BSRR_RESET;
                ws2812_transfer_active = false;
                ws2812_signal_block_worker_from_isr();
                return;
            }

            dmaStreamSetSource(ws2812_dma_stream, ws2812_buf[buf_sel]);
            dmaStreamSetTransactionSize(ws2812_dma_stream, blk_size);
            /* Prevent stale timer UIF from triggering an immediate DMA
             * transfer with wrong phase timing at block boundaries.
             * Interrupt-protected: CNT reset → SR clear → arm DMA → UDE enable
             * must complete within one timer period to prevent
             * a timer overflow from setting UIF before UDE is re-enabled.
             *
             * NOTE: WB32 timer SR is rw (not rc_w0 like STM32). Writing
             * SR = ~FLAG can SET other bits. Always use SR = 0.
             */
            ws2812_gpt->tim->DIER &= ~WB32_TIM_DIER_UDE;
            __disable_irq();
            ws2812_gpt->tim->CNT = 0;
            ws2812_gpt->tim->SR = 0;
            dmaStreamEnable(ws2812_dma_stream);
            ws2812_gpt->tim->DIER |= WB32_TIM_DIER_UDE;
            __enable_irq();
        } else {
            /* All blocks sent — stop */
            dmaStreamDisable(ws2812_dma_stream);
            gptStopTimerI(ws2812_gpt);
            WS2812_GPIO_PORT->BSRR = BSRR_RESET;
            ws2812_transfer_active = false;
        }

        /* Wake the high-priority worker only after the next DMA block has
         * already been chained. It can now refill the released buffer while
         * hardware transmits the current block. */
        ws2812_signal_block_worker_from_isr();
    }
}

/**
 * @brief Initialize the GPIO DMA WS2812 driver
 */
void ws2812_init(void) {
    /* Configure GPIO pin as push-pull output */
    palSetLineMode(WS2812_DI_PIN, PAL_MODE_OUTPUT_PUSHPULL);
    palClearLine(WS2812_DI_PIN);  /* Start LOW */

    /* Start GPT driver */
    gptStart(ws2812_gpt, &ws2812_gpt_config);

    /* Allocate DMA with callback */
    ws2812_dma_stream = dmaStreamAlloc(WS2812_GPIO_DMA_STREAM - WB32_DMA_STREAM(0),
                                       3,  /* IRQ priority — must be >= CORTEX_MAX_KERNEL_PRIORITY (3) */
                                       ws2812_dma_callback, NULL);

    if (ws2812_dma_stream == NULL) {
        return;
    }

    /* Configure DMA mode: Memory -> Peripheral (GPIO BSRR)
     * Non-circular, TFR interrupt for block chaining, error interrupt.
     */
    dmaStreamSetDestination(ws2812_dma_stream, &WS2812_GPIO_PORT->BSRR);
    dmaStreamSetMode(ws2812_dma_stream,
                     WB32_DMA_CHCFG_HWHIF(WS2812_GPIO_DMA_TIM_HWHIF) |
                     WB32_DMA_CHCFG_DIR_M2P |
                     WB32_DMA_CHCFG_PSIZE_WORD |
                     WB32_DMA_CHCFG_MSIZE_WORD |
                     WB32_DMA_CHCFG_MINC |
                     WB32_DMA_CHCFG_TCIE |    /* TFR interrupt for block chaining */
                     WB32_DMA_CHCFG_TEIE |    /* Error detection */
                     WB32_DMA_CHCFG_PL(3));   /* Highest DMA priority */
    /* NO CIRC — non-circular */
    /* NO HTIE — no half-transfer / BLOCK interrupt (doesn't work on WB32) */

    /* Workaround: dmaStreamSetMode() leaves HS_SEL_SRC=0 (hardware) in CFGL
     * for M2P transfers. The source is memory — it has no hardware handshake.
     * Without this fix, the DMA waits forever for a source request from
     * HWHIF 0 (TIM1_CH1) that never fires, causing an immediate lockup.
     */
    ws2812_dma_stream->dmac->Ch[ws2812_dma_stream->channel].CFGL |= WB32_DMAC_SRC_HIFS_SW;

    ws2812_transfer_active = false;
    ws2812_worker_busy     = false;
    ws2812_buf_block[0]    = 0xFFFFFFFFU;
    ws2812_buf_block[1]    = 0xFFFFFFFFU;

    chSemObjectInit(&ws2812_start_sem, 0);
    chSemObjectInit(&ws2812_block_sem, 0);

    /* Run one priority above the thread that initializes the driver. This is
     * intentional: after a DMA boundary the refill (~40µs) must complete well
     * inside the next ~189µs block period, then the worker sleeps again. */
    ws2812_worker_thread = chThdCreateStatic(
        ws2812_worker_wa, sizeof(ws2812_worker_wa),
        chThdGetPriorityX() + 1, ws2812_worker, NULL);
}

/**
 * @brief Check if a DMA transfer is currently in progress
 * @return true if transfer is active, false if idle
 */
bool ws2812_is_transfer_active(void) {
    return ws2812_transfer_active;
}

/**
 * @brief Set color for a single LED
 */
void ws2812_set_color(int index, uint8_t red, uint8_t green, uint8_t blue) {
    if (index >= 0 && index < WS2812_LED_COUNT) {
        ws2812_leds[index].r = red;
        ws2812_leds[index].g = green;
        ws2812_leds[index].b = blue;
#ifdef WS2812_RGBW
        ws2812_rgb_to_rgbw(&ws2812_leds[index]);
#endif
    }
}

/**
 * @brief Set color for all LEDs
 */
void ws2812_set_color_all(uint8_t red, uint8_t green, uint8_t blue) {
    for (int i = 0; i < WS2812_LED_COUNT; i++) {
        ws2812_set_color(i, red, green, blue);
    }
}

/**
 * @brief Background refill worker for one immutable RGB frame
 *
 * The worker owns all phase encoding after flush() snapshots ws2812_leds[].
 * It starts the transfer, then sleeps on a semaphore between block boundaries.
 * The DMA ISR remains short: chain the already prepared block, signal worker,
 * return. This keeps encoding out of interrupt context while allowing the QMK
 * main thread to run during most of the WS2812 wire time.
 */
static THD_FUNCTION(ws2812_worker, arg) {
    (void)arg;

    while (true) {
        chSemWait(&ws2812_start_sem);

        /* Discard any stale boundary wakeup left by an aborted/finished frame. */
        chSemReset(&ws2812_block_sem, 0);

        /* Prepare the two-block runway before starting DMA. */
        const uint32_t blk0_size = ws2812_get_block_size(0);
        ws2812_fill_block(ws2812_buf[0], 0, blk0_size);
        __DMB();
        ws2812_buf_block[0] = 0;

        if (WS2812_BLOCK_COUNT > 1) {
            const uint32_t blk1_size = ws2812_get_block_size(1);
            ws2812_fill_block(ws2812_buf[1], WS2812_BLOCK_SIZE, blk1_size);
            __DMB();
            ws2812_buf_block[1] = 1;
        }

        ws2812_block_idx       = 0;
        ws2812_transfer_active = true;

        dmaStreamSetSource(ws2812_dma_stream, ws2812_buf[0]);
        dmaStreamSetTransactionSize(ws2812_dma_stream, blk0_size);

        /* WB32/ChibiOS quirk documented by the upstream driver: disable() can
         * clear these masks, therefore they must be restored every frame. */
        dmaStreamEnableInterrupt(ws2812_dma_stream, WB32_DMAC_IT_TFR);
        dmaStreamEnableInterrupt(ws2812_dma_stream, WB32_DMAC_IT_ERR);

        /* Preserve the proven WB32 start sequence: UDE stays off across UG,
         * then CNT/SR/DMA/UDE are armed atomically within one timer period. */
        ws2812_gpt->tim->DIER &= ~WB32_TIM_DIER_UDE;
        gptStartContinuous(ws2812_gpt, WS2812_PHASE_TICKS);
        chSysDisable();
        ws2812_gpt->tim->CNT = 0;
        ws2812_gpt->tim->SR = 0;
        dmaStreamEnable(ws2812_dma_stream);
        ws2812_gpt->tim->DIER |= WB32_TIM_DIER_UDE;
        chSysEnable();

        uint32_t next_fill = 2; /* blocks 0 and 1 are already prepared */

        while (ws2812_transfer_active) {
            const msg_t msg = chSemWaitTimeout(
                &ws2812_block_sem, TIME_MS2I(WS2812_TIMEOUT_MS));

            if (msg == MSG_TIMEOUT) {
                ws2812_abort_transfer();
                break;
            }

            if (!ws2812_transfer_active) {
                break;
            }

            const uint32_t current = ws2812_block_idx;

            /* Normally exactly one block is filled per wakeup. The while form
             * also catches up if a semaphore signal was already pending. */
            while (next_fill < WS2812_BLOCK_COUNT &&
                   current >= (next_fill - 1U)) {
                const uint32_t slot        = next_fill % 2U;
                const uint32_t start_phase = next_fill * WS2812_BLOCK_SIZE;
                const uint32_t size        = ws2812_get_block_size(next_fill);

                ws2812_fill_block(ws2812_buf[slot], start_phase, size);
                __DMB();
                ws2812_buf_block[slot] = next_fill;
                next_fill++;
            }
        }

        /* Final/aborted transfer leaves the pin LOW. Keep the reset interval in
         * this sleeping worker instead of stalling the QMK main thread. */
        chThdSleepMicroseconds(WS2812_RESET_US);
        ws2812_worker_busy = false;
    }
}

/**
 * @brief Queue the current LED frame for asynchronous DMA transmission
 *
 * Normal RGB cadence (~16ms) is much slower than one WS2812 frame (~3ms), so
 * the previous worker is normally already idle. If a caller does arrive early,
 * preserve the old serialized semantics with the existing timeout rather than
 * overwriting the snapshot of an in-flight frame.
 */
void ws2812_flush(void) {
    if (ws2812_dma_stream == NULL || ws2812_worker_thread == NULL) {
        return;
    }

    uint32_t start = timer_read32();
    while (ws2812_worker_busy) {
        if (timer_elapsed32(start) >= (WS2812_TIMEOUT_MS + 1U)) {
            return;
        }
    }

    memcpy(ws2812_frame_leds, ws2812_leds, sizeof(ws2812_frame_leds));

    /* Publish the snapshot before waking the worker. chSemSignal() will allow
     * the higher-priority worker to preempt immediately and build block 0/1. */
    __DMB();
    ws2812_worker_busy = true;
    chSemSignal(&ws2812_start_sem);
}

#endif /* WB32F3G71xx || WB32FQ95xx */
