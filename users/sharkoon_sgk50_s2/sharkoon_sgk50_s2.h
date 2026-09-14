#pragma once

#include "quantum.h"

#ifdef RGB_MATRIX_ENABLE
void sharkoon_apply_disabled_led_flags(void);
void sharkoon_clear_disabled_leds(void);
void sharkoon_solid_reactive_process_key_event(keyrecord_t *record);
#endif