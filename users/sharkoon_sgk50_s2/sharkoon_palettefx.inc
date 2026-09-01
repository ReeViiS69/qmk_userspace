#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS

/*
 * Compile the original PaletteFx renderers here without registering
 * additional RGB Matrix effect IDs.
 */
#define PALETTEFX_GRADIENT_ENABLE
#define PALETTEFX_FLOW_ENABLE
#define PALETTEFX_RIPPLE_ENABLE
#define PALETTEFX_SPARKLE_ENABLE
#define PALETTEFX_VORTEX_ENABLE
#define PALETTEFX_REACTIVE_ENABLE

#include "rgb_matrix_module.inc"

#undef PALETTEFX_GRADIENT_ENABLE
#undef PALETTEFX_FLOW_ENABLE
#undef PALETTEFX_RIPPLE_ENABLE
#undef PALETTEFX_SPARKLE_ENABLE
#undef PALETTEFX_VORTEX_ENABLE
#undef PALETTEFX_REACTIVE_ENABLE


/*
 * Restore the original QMK effect names.
 * Their IDs stay unchanged, only their renderers are replaced.
 */
#undef GRADIENT_UP_DOWN
#undef GRADIENT_LEFT_RIGHT
#undef TYPING_HEATMAP
#undef CYCLE_PINWHEEL
#undef CYCLE_SPIRAL
#undef PIXEL_RAIN

bool GRADIENT_UP_DOWN(effect_params_t *params) {
    return PALETTEFX_GRADIENT(params);
}

bool GRADIENT_LEFT_RIGHT(effect_params_t *params) {
    return PALETTEFX_SPARKLE(params);
}

bool TYPING_HEATMAP(effect_params_t *params) {
    return PALETTEFX_REACTIVE(params);
}

bool CYCLE_PINWHEEL(effect_params_t *params) {
    return PALETTEFX_FLOW(params);
}

bool CYCLE_SPIRAL(effect_params_t *params) {
    return PALETTEFX_VORTEX(params);
}

bool PIXEL_RAIN(effect_params_t *params) {
    /*
     * Keep a reference to QMK's renamed static renderer so GCC does not
     * report it as an unused static function.
     */
    (void)SHARKOON_ORIGINAL_PIXEL_RAIN;

    return PALETTEFX_RIPPLE(params);
}
#endif