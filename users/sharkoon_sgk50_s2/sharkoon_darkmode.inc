/*
 * Sharkoon RGB Darkmode / semi-off control.
 *
 * Idle Darkmode keeps RGB Matrix disabled. It wakes RGB only while macro
 * lighting needs the normal indicator render loop, then returns to semi-off.
 */

static bool sharkoon_rgb_darkmode = false;

static bool sharkoon_darkmode_is_active(void) {
    return sharkoon_rgb_darkmode;
}

static void sharkoon_darkmode_idle(void) {
    rgb_matrix_disable_noeeprom();
    rgb_matrix_task(); // STARTING -> RENDERING
    rgb_matrix_task(); // RENDERING -> FLUSHING, buffer is black
    sharkoon_render_status_leds();
    rgb_matrix_task(); // flush black + status LEDs once
}

static void sharkoon_darkmode_refresh(void) {
    if (!sharkoon_rgb_darkmode || rgb_matrix_is_enabled()) {
        return;
    }

    // RGB Matrix is sleeping; update only the event-driven status LEDs.
    if (sharkoon_caps_lock_led != NO_LED) {
        rgb_matrix_set_color(sharkoon_caps_lock_led, 0, 0, 0);
    }
    if (sharkoon_unicode_typing_led != NO_LED) {
        rgb_matrix_set_color(sharkoon_unicode_typing_led, 0, 0, 0);
    }

    sharkoon_render_status_leds();
    rgb_matrix_update_pwm_buffers();
}

static void sharkoon_update_darkmode_rgb_state(void) {
    if (!sharkoon_rgb_darkmode) {
        return;
    }

    if (sharkoon_macro_lighting_active()) {
        if (!rgb_matrix_is_enabled()) {
            rgb_matrix_enable_noeeprom();
        }
    } else if (rgb_matrix_is_enabled()) {
        sharkoon_darkmode_idle();
    }
}

static void sharkoon_toggle_rgb_darkmode(void) {
    if (sharkoon_rgb_darkmode) {
        sharkoon_rgb_darkmode = false;
        if (!rgb_matrix_is_enabled()) {
            rgb_matrix_enable_noeeprom();
        }
        return;
    }

    if (!rgb_matrix_is_enabled()) {
        rgb_matrix_enable_noeeprom();
        return;
    }

    sharkoon_rgb_darkmode = true;
    if (!sharkoon_macro_lighting_active()) {
        sharkoon_darkmode_idle();
    }
}

/*
 * RM_TOGG while Darkmode is active must always leave RGB enabled, including
 * the temporary macro-blink state where RGB Matrix is already awake.
 * Return true when the key event is fully handled here.
 */
static bool sharkoon_darkmode_handle_rm_togg(void) {
    if (!sharkoon_rgb_darkmode) {
        return false;
    }

    sharkoon_rgb_darkmode = false;
    if (rgb_matrix_is_enabled()) {
        return true;
    }

    return false;
}
