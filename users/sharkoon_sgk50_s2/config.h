#pragma once

/*
 * Keep NKRO enabled by default
 */
#define NKRO_DEFAULT_ON true

/*
 * Replace QMK's stock Solid Reactive renderer while preserving the official
 * RGB_MATRIX_SOLID_REACTIVE mode ID.
 */
#define SOLID_REACTIVE SHARKOON_ORIGINAL_SOLID_REACTIVE