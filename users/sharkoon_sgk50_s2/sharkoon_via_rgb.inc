#ifdef VIALRGB_DIRECT_ENABLE
void sharkoon_direct_mark_dirty_packet(const uint8_t *data, uint8_t length);

static uint32_t sharkoon_direct_last_packet = 0;
static bool     sharkoon_direct_seen        = false;

bool sharkoon_direct_stream_active(void) {
    if (!sharkoon_direct_seen) {
        return false;
    }

    if (timer_elapsed32(sharkoon_direct_last_packet) >= 250) {
        sharkoon_direct_seen = false;
        return false;
    }

    return true;
}
#endif

#ifdef VIA_ENABLE
bool via_command_kb(uint8_t *data, uint8_t length) {
#ifdef RGB_MATRIX_ENABLE
    sharkoon_rgb_observe_via_command(data, length);
#endif

#ifdef VIALRGB_DIRECT_ENABLE
    if (length >= 2 && data[0] == id_custom_set_value && data[1] == vialrgb_direct_fastset) {
        sharkoon_direct_mark_dirty_packet(data, length);
        sharkoon_direct_last_packet = timer_read32();
        sharkoon_direct_seen        = true;
    }
#endif

    // Never consume VIA/Vial traffic here; the core must process it normally.
    return false;
}
#endif
