/*
 * Sharkoon dynamic macro recorder.
 *
 * Owns recording state, capture/save logic and implementations for the macro
 * control keys. The central keymap switch keeps the case labels together so
 * QMK/GCC can retain the original compact custom-keycode dispatch.
 * RGB feedback is notified only when recording starts or finishes.
 */

#define SHARKOON_MACRO_MAX_ACTIONS    16
#define SHARKOON_MACRO_EVENT_SIZE     3
#define SHARKOON_MACRO_RECORDING_SIZE (SHARKOON_MACRO_MAX_ACTIONS * 2 * SHARKOON_MACRO_EVENT_SIZE)
#define SHARKOON_MACRO_IO_CHUNK_SIZE  64

static bool    sharkoon_recording          = false;
static bool    sharkoon_accept_new_presses = false;
static uint8_t sharkoon_recording_slot     = 0;
static uint8_t sharkoon_action_count       = 0;
static uint8_t sharkoon_recording_length   = 0;
static uint8_t sharkoon_pressed_count      = 0;
static uint8_t sharkoon_recording_data[SHARKOON_MACRO_RECORDING_SIZE];
static uint8_t sharkoon_recorded_key[MATRIX_ROWS][MATRIX_COLS];

#ifdef RGB_MATRIX_ENABLE
static void sharkoon_rgb_macro_recording_started(uint8_t row, uint8_t col);
static void sharkoon_rgb_macro_recording_finished(void);
#endif

static inline __attribute__((always_inline)) bool sharkoon_macro_recording_active(void) {
    return sharkoon_recording;
}

static bool sharkoon_append_event(uint8_t event_type, uint8_t keycode) {
    if ((uint16_t)(sharkoon_recording_length + SHARKOON_MACRO_EVENT_SIZE) > (uint16_t)sizeof(sharkoon_recording_data)) {
        return false;
    }

    sharkoon_recording_data[sharkoon_recording_length++] = SS_QMK_PREFIX;
    sharkoon_recording_data[sharkoon_recording_length++] = event_type;
    sharkoon_recording_data[sharkoon_recording_length++] = keycode;
    return true;
}

static void sharkoon_start_recording(uint8_t slot, uint8_t row, uint8_t col) {
    sharkoon_recording          = true;
    sharkoon_accept_new_presses = true;
    sharkoon_recording_slot     = slot;
    sharkoon_action_count       = 0;
    sharkoon_recording_length   = 0;
    sharkoon_pressed_count      = 0;

#ifdef RGB_MATRIX_ENABLE
    sharkoon_rgb_macro_recording_started(row, col);
#endif

    memset(sharkoon_recorded_key, 0, sizeof(sharkoon_recorded_key));
}

static bool sharkoon_find_macro_bounds(uint8_t slot, uint16_t buffer_size, uint16_t *macro_start, uint16_t *macro_end, uint16_t *used_end) {
    const uint8_t macro_count = dynamic_keymap_macro_get_count();

    if (slot >= macro_count || buffer_size == 0) {
        return false;
    }

    uint8_t  buffer[SHARKOON_MACRO_IO_CHUNK_SIZE];
    uint8_t  current_macro   = 0;
    uint16_t current_start   = 0;
    uint16_t last_terminator = 0;
    bool     found_target    = false;

    for (uint16_t offset = 0; offset < buffer_size;) {
        const uint16_t remaining  = buffer_size - offset;
        const uint16_t chunk_size = remaining < sizeof(buffer) ? remaining : sizeof(buffer);

        dynamic_keymap_macro_get_buffer(offset, chunk_size, buffer);

        for (uint16_t i = 0; i < chunk_size; ++i) {
            if (buffer[i] != 0) {
                continue;
            }

            const uint16_t terminator = offset + i;
            last_terminator           = terminator + 1;

            if (current_macro == slot) {
                *macro_start = current_start;
                *macro_end   = terminator;
                found_target = true;
            }

            ++current_macro;
            current_start = terminator + 1;

            if (current_macro >= macro_count) {
                *used_end = last_terminator;
                return found_target;
            }
        }

        offset += chunk_size;
    }

    if (!found_target) {
        return false;
    }

    *used_end = last_terminator;
    return true;
}

static bool sharkoon_macro_matches_recording(uint16_t macro_start, uint16_t old_macro_size) {
    const uint16_t new_macro_size = (uint16_t)sharkoon_recording_length + 1;

    if (old_macro_size != new_macro_size) {
        return false;
    }

    uint8_t  buffer[SHARKOON_MACRO_IO_CHUNK_SIZE];
    uint16_t compared = 0;

    while (compared < sharkoon_recording_length) {
        const uint16_t remaining  = (uint16_t)sharkoon_recording_length - compared;
        const uint16_t chunk_size = remaining < sizeof(buffer) ? remaining : sizeof(buffer);

        dynamic_keymap_macro_get_buffer(macro_start + compared, chunk_size, buffer);
        if (memcmp(buffer, &sharkoon_recording_data[compared], chunk_size) != 0) {
            return false;
        }

        compared += chunk_size;
    }

    return true;
}

static void sharkoon_move_macro_data(uint16_t source, uint16_t destination, uint16_t size) {
    if (size == 0 || source == destination) {
        return;
    }

    uint8_t buffer[SHARKOON_MACRO_IO_CHUNK_SIZE];

    if (destination > source) {
        // Move backwards so overlapping source bytes are read before they are overwritten.
        while (size > 0) {
            const uint16_t chunk_size = size < sizeof(buffer) ? size : sizeof(buffer);
            size -= chunk_size;

            dynamic_keymap_macro_get_buffer(source + size, chunk_size, buffer);
            dynamic_keymap_macro_set_buffer(destination + size, chunk_size, buffer);
        }
        return;
    }

    // Move forwards when shifting left for the same overlap reason.
    uint16_t moved = 0;
    while (moved < size) {
        const uint16_t remaining  = size - moved;
        const uint16_t chunk_size = remaining < sizeof(buffer) ? remaining : sizeof(buffer);

        dynamic_keymap_macro_get_buffer(source + moved, chunk_size, buffer);
        dynamic_keymap_macro_set_buffer(destination + moved, chunk_size, buffer);
        moved += chunk_size;
    }
}

static void sharkoon_clear_macro_data(uint16_t offset, uint16_t size) {
    uint8_t buffer[SHARKOON_MACRO_IO_CHUNK_SIZE] = {0};

    while (size > 0) {
        const uint16_t chunk_size = size < sizeof(buffer) ? size : sizeof(buffer);

        dynamic_keymap_macro_set_buffer(offset, chunk_size, buffer);
        offset += chunk_size;
        size -= chunk_size;
    }
}

static void sharkoon_save_recording(void) {
    const uint16_t buffer_size = dynamic_keymap_macro_get_buffer_size();

    if (buffer_size == 0) {
        return;
    }

    const uint16_t valid_offset = buffer_size - 1;
    uint8_t        valid_flag   = 0;

    // QMK uses the last macro-buffer byte as a validity flag. Never modify an
    // already-invalid buffer, which may be left behind by an interrupted write.
    dynamic_keymap_macro_get_buffer(valid_offset, 1, &valid_flag);
    if (valid_flag != 0) {
        return;
    }

    uint16_t macro_start = 0;
    uint16_t macro_end   = 0;
    uint16_t used_end    = 0;

    if (!sharkoon_find_macro_bounds(sharkoon_recording_slot, buffer_size, &macro_start, &macro_end, &used_end)) {
        return;
    }

    const uint16_t old_macro_size = (macro_end - macro_start) + 1; // includes terminating NUL
    const uint16_t new_macro_size = (uint16_t)sharkoon_recording_length + 1;

    if (sharkoon_macro_matches_recording(macro_start, old_macro_size)) {
        return;
    }

    const int32_t  size_delta = (int32_t)new_macro_size - (int32_t)old_macro_size;
    const uint16_t tail_start = macro_end + 1;
    const uint16_t tail_size  = used_end - tail_start;

    if (size_delta > 0 && (uint32_t)used_end + (uint32_t)size_delta > buffer_size) {
        return;
    }

    const uint16_t new_tail_start = (uint16_t)((int32_t)tail_start + size_delta);
    const uint16_t new_used_end   = (uint16_t)((int32_t)used_end + size_delta);

    // The last byte of a non-empty tail is always its final NUL terminator.
    // Move only the bytes before it so the macro-buffer valid flag is never
    // touched as ordinary data; restore that terminator separately below.
    const uint16_t tail_move_size = tail_size > 0 ? tail_size - 1 : 0;

    valid_flag = 0xFF;
    dynamic_keymap_macro_set_buffer(valid_offset, 1, &valid_flag);

    sharkoon_move_macro_data(tail_start, new_tail_start, tail_move_size);

    if (tail_size > 0) {
        const uint16_t tail_terminator = new_used_end - 1;
        if (tail_terminator < valid_offset) {
            uint8_t terminator = 0;
            dynamic_keymap_macro_set_buffer(tail_terminator, 1, &terminator);
        }
    }

    if (sharkoon_recording_length > 0) {
        dynamic_keymap_macro_set_buffer(macro_start, sharkoon_recording_length, sharkoon_recording_data);
    }

    const uint16_t terminator_offset = macro_start + sharkoon_recording_length;
    if (terminator_offset < valid_offset) {
        uint8_t terminator = 0;
        dynamic_keymap_macro_set_buffer(terminator_offset, 1, &terminator);
    }

    if (new_used_end < used_end) {
        const uint16_t clear_end = used_end < valid_offset ? used_end : valid_offset;
        if (new_used_end < clear_end) {
            sharkoon_clear_macro_data(new_used_end, clear_end - new_used_end);
        }
    }

    valid_flag = 0;
    dynamic_keymap_macro_set_buffer(valid_offset, 1, &valid_flag);
}

static void sharkoon_release_recorded_keys(void) {
    for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
        for (uint8_t col = 0; col < MATRIX_COLS; ++col) {
            const uint8_t keycode = sharkoon_recorded_key[row][col];
            if (keycode == 0) {
                continue;
            }

            sharkoon_append_event(SS_UP_CODE, keycode);
            sharkoon_recorded_key[row][col] = 0;
        }
    }

    sharkoon_pressed_count = 0;
}

static void sharkoon_finish_recording(bool force_release) {
    if (!sharkoon_recording) {
        return;
    }

    if (force_release) {
        sharkoon_release_recorded_keys();
    }

    sharkoon_save_recording();

    sharkoon_recording          = false;
    sharkoon_accept_new_presses = false;
#ifdef RGB_MATRIX_ENABLE
    sharkoon_rgb_macro_recording_finished();
#endif
}

static void sharkoon_consume_active_oneshot_layer(void) {
    if (!is_oneshot_layer_active()) {
        return;
    }

    const uint8_t oneshot_layer = get_oneshot_layer();
    layer_off(oneshot_layer);
    reset_oneshot_layer();
}

static inline __attribute__((always_inline)) void sharkoon_macro_select_process(keyrecord_t *record, uint8_t row, uint8_t col) {
    sharkoon_set_suppress_release(row, col);

    if (record->event.pressed) {
        if (is_layer_locked(1) || layer_state_is(3)) {
            layer_lock_off(1);
            layer_off(3);
        } else {
            layer_lock_on(1);
            layer_on(3);
        }
    }
}

static inline __attribute__((always_inline)) void sharkoon_macro_record_start_process(uint16_t keycode, keyrecord_t *record, uint8_t row, uint8_t col) {
    sharkoon_set_suppress_release(row, col);

    if (record->event.pressed) {
        layer_lock_off(1);
        layer_off(3);
        sharkoon_consume_active_oneshot_layer();

        if (!sharkoon_recording) {
            sharkoon_start_recording(keycode == REC_M0 ? 0 : 1, row, col);
        }
    }
}

static inline __attribute__((always_inline)) void sharkoon_macro_record_stop_process(keyrecord_t *record, uint8_t row, uint8_t col) {
    sharkoon_set_suppress_release(row, col);

    if (record->event.pressed && sharkoon_recording) {
        sharkoon_finish_recording(true);
    }
}

static inline __attribute__((always_inline)) bool sharkoon_macro_recorder_capture(uint16_t keycode, keyrecord_t *record, uint8_t row, uint8_t col) {
    // VIA macro playback keys are controls, not recordable input. Swallow them
    // while recording so a stored macro cannot recursively play into a capture.
    if (keycode >= QK_MACRO && keycode <= QK_MACRO_MAX) {
        if (record->event.pressed) {
            sharkoon_set_suppress_release(row, col);
        }
        return false;
    }

    if (record->event.pressed) {
        // Only 8-bit keycodes can be represented by QMK's send-string DOWN/UP
        // commands. Layer keys and other 16-bit QMK controls therefore pass
        // through normally but are intentionally not recorded.
        if (sharkoon_accept_new_presses && IS_ANY(keycode) && sharkoon_recorded_key[row][col] == 0) {
            const uint8_t basic_keycode = (uint8_t)keycode;

            if (sharkoon_append_event(SS_DOWN_CODE, basic_keycode)) {
                sharkoon_recorded_key[row][col] = basic_keycode;
                ++sharkoon_pressed_count;
                ++sharkoon_action_count;

                if (sharkoon_action_count >= SHARKOON_MACRO_MAX_ACTIONS) {
                    sharkoon_accept_new_presses = false;
                }
            }
        }

        return true;
    }

    const uint8_t recorded_keycode = sharkoon_recorded_key[row][col];
    if (recorded_keycode != 0) {
        sharkoon_append_event(SS_UP_CODE, recorded_keycode);
        sharkoon_recorded_key[row][col] = 0;

        --sharkoon_pressed_count;

        if (!sharkoon_accept_new_presses && sharkoon_pressed_count == 0) {
            sharkoon_finish_recording(false);
        }
    }

    return true;
}
