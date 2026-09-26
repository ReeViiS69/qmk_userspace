// Physical-key release suppression for control keys that change layer state.

#define SHARKOON_MATRIX_KEY_COUNT       (MATRIX_ROWS * MATRIX_COLS)
#define SHARKOON_MATRIX_BITSET_WORDS    ((SHARKOON_MATRIX_KEY_COUNT + SHARKOON_BITSET_WORD_BITS - 1u) / SHARKOON_BITSET_WORD_BITS)

static uint32_t sharkoon_suppress_release[SHARKOON_MATRIX_BITSET_WORDS];

static uint16_t sharkoon_matrix_bit_index(uint8_t row, uint8_t col) {
    return ((uint16_t)row * MATRIX_COLS) + col;
}

static void sharkoon_set_suppress_release(uint8_t row, uint8_t col) {
    const uint16_t bit = sharkoon_matrix_bit_index(row, col);
    sharkoon_suppress_release[bit / SHARKOON_BITSET_WORD_BITS] |=
        UINT32_C(1) << (bit % SHARKOON_BITSET_WORD_BITS);
}

static bool sharkoon_take_suppress_release(uint8_t row, uint8_t col) {
    const uint16_t bit  = sharkoon_matrix_bit_index(row, col);
    uint32_t      *word = &sharkoon_suppress_release[bit / SHARKOON_BITSET_WORD_BITS];
    const uint32_t mask = UINT32_C(1) << (bit % SHARKOON_BITSET_WORD_BITS);

    if ((*word & mask) == 0) {
        return false;
    }

    *word &= ~mask;
    return true;
}
