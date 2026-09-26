// Copyright 2024 JoyLee (@itarze)
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H

#include "action_layer.h"
#include "color.h"
#include "dynamic_keymap.h"
#include "keycode.h"
#include "send_string_keycodes.h"
#include "timer.h"

#ifdef SHARKOON_PERF_BENCHMARK
#    ifndef CONSOLE_ENABLE
#        error "SHARKOON_PERF_BENCHMARK requires CONSOLE_ENABLE = yes"
#    endif
#    include "print.h"
#endif

#include <stdint.h>
#include <string.h>

#include "sharkoon_sgk50_s2.h"
#ifdef VIA_ENABLE
#    include "via.h"
#endif
#ifdef VIALRGB_ENABLE
#    include "vialrgb.h"
#endif

#include "socd_cleaner.h"
#include "unicode_typing.h"
socd_cleaner_t socd_opposing_pairs[] = {
    {{KC_A, KC_D}, SOCD_CLEANER_LAST},
    {{MS_LEFT,   MS_RGHT,}, SOCD_CLEANER_LAST},
};
#define SHARKOON_BITSET_WORD_BITS       32u

enum custom_keycodes {
    REC_M0 = QK_USER_0,
    REC_M1 = QK_USER_1,
    REC_STOP = QK_USER_2,
    MACRO_SELECT = QK_USER_3,
    UNICODE_TYPING_CYCLE = QK_USER_4,
    RGB_DARK = QK_USER_5,
};

#include "sharkoon_key_event_guard.inc"
#include "sharkoon_macro_recorder.inc"

#ifdef RGB_MATRIX_ENABLE
#    include "sharkoon_rgb_indicators.inc"
#endif

#include "sharkoon_via_rgb.inc"

#ifdef SHARKOON_PERF_BENCHMARK
#    include "sharkoon_perf_benchmark.inc"
#endif

#ifdef RGB_MATRIX_ENABLE
void keyboard_post_init_user(void) {
#ifdef SHARKOON_PERF_BENCHMARK
    sharkoon_perf_enable_dwt();
    sharkoon_perf_last_key_event_ms = timer_read32();
#endif
    sharkoon_rgb_indicators_init();
}

bool led_update_user(led_t led_state) {
    sharkoon_rgb_caps_lock_changed(led_state.caps_lock);
    return true;
}

layer_state_t layer_state_set_user(layer_state_t state) {
    sharkoon_rgb_layer_state_changed(state);
    return state;
}
#endif

#ifdef RGB_MATRIX_ENABLE
bool pre_process_record_user(uint16_t keycode, keyrecord_t *record) {
    (void)keycode;
#ifdef SHARKOON_PERF_BENCHMARK
    sharkoon_perf_note_key_event(record);
#endif

    /*
     * Maintain only the event-side state required by the currently selected
     * Sharkoon effect. Solid Reactive reconstructs held keys once on effect
     * entry, so other RGB modes no longer pay for its held-bitset updates.
     */
    if (!rgb_matrix_config.enable) {
        return true;
    }

    switch (rgb_matrix_config.mode) {
        case RGB_MATRIX_SOLID_REACTIVE:
            sharkoon_solid_reactive_process_key_event(record);
            break;

#    ifdef VIALRGB_DIRECT_ENABLE
        case RGB_MATRIX_CUSTOM_VIALRGB_DIRECT:
            sharkoon_hybrid_reactive_process_key_event(record);
            break;
#    endif

        default:
            break;
    }

    return true;
}
#endif

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    const uint8_t row = record->event.key.row;
    const uint8_t col = record->event.key.col;

    // A custom control key can resolve to a different keycode on release after
    // changing the layer state. Suppress that physical release by matrix
    // position so no release from the newly exposed key can escape to the host.
    if (!record->event.pressed && sharkoon_take_suppress_release(row, col)) {
        return false;
    }

    switch (keycode) {
#ifdef RGB_MATRIX_ENABLE
        case RGB_DARK:
            sharkoon_set_suppress_release(row, col);

            if (record->event.pressed) {
                sharkoon_toggle_rgb_darkmode();
            }

            return false;

        case RM_TOGG:
            if (record->event.pressed && sharkoon_darkmode_handle_rm_togg()) {
                return false;
            }
            return true;
#endif

        case UNICODE_TYPING_CYCLE:
            sharkoon_set_suppress_release(row, col);

            if (record->event.pressed) {
                static const uint8_t modes[] = {
                    UCTM_NO_MODE,
                    UCTM_AUSSIE,
                    UCTM_ZALGO,
                    UCTM_SUPER,
                };

                uint8_t current = get_unicode_typing_mode();
                uint8_t next_index = 0;

                for (uint8_t i = 0; i < ARRAY_SIZE(modes); i++) {
                    if (modes[i] == current) {
                        next_index = (i + 1) % ARRAY_SIZE(modes);
                        break;
                    }
                }

                const uint8_t next_mode = modes[next_index];
                set_unicode_typing_mode(next_mode);

#ifdef RGB_MATRIX_ENABLE
                sharkoon_rgb_unicode_mode_changed(next_mode);
#endif
            }

            return false;

        case MACRO_SELECT:
            sharkoon_macro_select_process(record, row, col);
            return false;

        case REC_M0:
        case REC_M1:
            sharkoon_macro_record_start_process(keycode, record, row, col);
            return false;

        case REC_STOP:
            sharkoon_macro_record_stop_process(record, row, col);
            return false;

        default:
            break;
    }

    if (!sharkoon_macro_recording_active()) {
        return true;
    }

    return sharkoon_macro_recorder_capture(keycode, record, row, col);
}



const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

    // Layer 0: Basisbelegung
    [0] = LAYOUT_96_iso(
        KC_ESC,    KC_F1,     KC_F2,     KC_F3,     KC_F4,     KC_F5,     KC_F6,     KC_F7,     KC_F8,     KC_F9,     KC_F10,    KC_F11,    KC_F12, QK_MACRO_0, QK_MACRO_1,    KC_DEL,    KC_HOME, KC_PGUP,   RM_TOGG,
        KC_GRV,    KC_1,      KC_2,      KC_3,      KC_4,      KC_5,      KC_6,      KC_7,      KC_8,      KC_9,      KC_0,      KC_MINS,   KC_EQL,                KC_BSPC,    KC_INS,    KC_END,  KC_PGDN,     TG(2),
        KC_TAB,      KC_Q,      KC_W,      KC_E,      KC_R,      KC_T,      KC_Y,      KC_U,      KC_I,      KC_O,      KC_P,      KC_LBRC,   KC_RBRC,                         KC_WSCH,   MS_UP,   KC_PAUS,   KC_MPLY,
        KC_CAPS,      KC_A,      KC_S,      KC_D,      KC_F,      KC_G,      KC_H,      KC_J,      KC_K,      KC_L,      KC_SCLN,   KC_QUOT,   KC_BSLS,            KC_ENT,     MS_LEFT,   MS_DOWN, MS_RGHT,
        SC_LSPO,    KC_NUBS,   KC_Z,      KC_X,      KC_C,      KC_V,      KC_B,      KC_N,      KC_M,      KC_COMM,   KC_DOT,    KC_SLSH,       SC_RSPC,          KC_RSFT,    KC_UP,     MS_BTN4, MS_BTN5,   MS_BTN2,
        SC_LCPO,    KC_LGUI,    SC_LAPO,                                   KC_SPC,                                    SC_RAPC,LT(1, KC_INT3), SC_RCPC,   MS_BTN1,   KC_LEFT,   KC_DOWN,   KC_RGHT, MS_BTN3
    ),

    // Layer 1: Funktionen
    [1] = LAYOUT_96_iso(
        EE_CLR,    KC_MPLY,   KC_MPRV,   KC_MNXT,   KC_MUTE,   KC_VOLD,   KC_VOLU,   RM_SPDD,   RM_SPDU,   QK_MACRO_0,QK_MACRO_1,KC_SCRL,   KC_PSCR,   REC_STOP, REC_STOP,    SOCDTOG,   RM_SATU,   RM_SPDU, RGB_DARK,
        _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,              _______,    _______,   RM_SATD,   RM_SPDD, _______,
        MACRO_SELECT,_______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,                        _______,   MS_WHLU,   _______, UNICODE_TYPING_CYCLE,
        REC_STOP,     _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,           _______,    MS_WHLL,   MS_WHLD,   MS_WHLR,
        _______,    _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,   _______,       _______,         _______,    RM_VALU,   _______,   _______, _______,
        _______,    GU_TOGG,    _______,                                   _______,                                    _______,   _______,   _______,   _______,   RM_HUEU,   RM_VALD,   RM_NEXT,   _______
    ),

    // Deutsch-auf-US Raw-Kompatibilitaet.
    // Layer 2 = ungeshiftete deutsche Zeichenpositionen als US-HID-Keycodes.
    // Die beiden physischen Shift-Tasten senden hier kein Shift, sondern halten Layer 3.
    [2] = LAYOUT_96_iso(
        KC_ESC,    KC_F1,     KC_F2,     KC_F3,     KC_F4,     KC_F5,     KC_F6,     KC_F7,     KC_F8,     KC_F9,     KC_F10,    KC_F11,    KC_F12, QK_MACRO_0,QK_MACRO_1,     KC_DEL,   _______,   _______, _______,
        KC_CIRC,   KC_1,      KC_2,      KC_3,      KC_4,      KC_5,      KC_6,      KC_7,      KC_8,      KC_9,      KC_0,      KC_MINS,   KC_EQL,               _______,    _______,   _______,   _______, _______,
        KC_TAB,      KC_Q,      KC_W,      KC_E,      KC_R,      KC_T,      KC_Z,      KC_U,      KC_I,      KC_O,      KC_P,      KC_U,      KC_PLUS,                        _______,   _______,   _______, KC_MPLY,
        TG(3),        KC_A,      KC_S,      KC_D,      KC_F,      KC_G,      KC_H,      KC_J,      KC_K,      KC_L,      KC_O,      KC_A,      KC_HASH,           _______,    _______,   _______,   _______,
        MO(3),      KC_LABK,   KC_Y,      KC_X,      KC_C,      KC_V,      KC_B,      KC_N,      KC_M,      KC_COMM,   KC_DOT,    KC_MINS,          MO(3),        _______,    _______,   _______,   _______, _______,
        _______,    _______,    _______,                                   _______,                                    _______,   _______,   _______,   _______,  _______,    _______,   _______,   _______
    ),

    // Layer 3:
    // 1) Shift-Ebene des Raw-Kompatibilitaetslayouts.
    // 2) Makro-Auswahl-Layer ueber MACRO_SELECT.
    [3] = LAYOUT_96_iso(
        S(KC_ESC), S(KC_F1),  S(KC_F2),  S(KC_F3),  S(KC_F4),  S(KC_F5),  S(KC_F6),  S(KC_F7),  S(KC_F8),   REC_M0,    REC_M1,   KC_SCRL,   KC_PSCR,   REC_STOP,  REC_STOP,   SOCDTOG,S(KC_HOME),S(KC_PGUP), RM_TOGG,
        KC_CIRC,   KC_EXLM,   KC_DQUO,   KC_HASH,   KC_DLR,    KC_PERC,   KC_AMPR,   KC_SLSH,   KC_LPRN,   KC_RPRN,   KC_EQL,    KC_QUES,    KC_GRV,            S(KC_BSPC), S(KC_INS), S(KC_END),S(KC_PGDN),   KC_NO,
        MACRO_SELECT,S(KC_Q),   S(KC_W),   S(KC_E),   S(KC_R),   S(KC_T),   S(KC_Z),   S(KC_U),   S(KC_I),   S(KC_O),   S(KC_P),   S(KC_U),   KC_ASTR,                        _______,   _______,   _______, UNICODE_TYPING_CYCLE,
        REC_STOP,     S(KC_A),   S(KC_S),   S(KC_D),   S(KC_F),   S(KC_G),   S(KC_H),   S(KC_J),   S(KC_K),   S(KC_L),   S(KC_O),   S(KC_A),   KC_QUOT,          S(KC_ENT),   _______,   _______,   _______,
        TG(3),      KC_RABK,   S(KC_Y),   S(KC_X),   S(KC_C),   S(KC_V),   S(KC_B),   S(KC_N),   S(KC_M),   KC_SCLN,   KC_COLN,   KC_UNDS,       _______,          _______,  S(KC_UP),   _______,   _______, _______,
        _______,    _______,    _______,                                   S(KC_SPC),                                  _______,   _______,   _______,   _______,S(KC_LEFT),S(KC_DOWN),S(KC_RGHT),   _______
    )
};
