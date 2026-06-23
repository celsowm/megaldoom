#include "fixed_math.h"

static s16 g_sin_table[ANGLE_STEPS];

static s16 sin_quarter_q8(u16 angle) {
    const s32 a = (s32)(angle & (ANGLE_90 - 1));
    const s32 x = (a * FX_ONE) / ANGLE_90;
    const s32 x2 = (x * x) >> FX_SHIFT;
    const s32 x3 = (x2 * x) >> FX_SHIFT;
    const s32 x5 = (x3 * x2) >> FX_SHIFT;
    const s32 term1 = 402 * x;
    const s32 term2 = 41 * x3;
    const s32 term3 = 5 * x5;

    return (s16)((term1 - term2 + term3) >> FX_SHIFT);
}

void fx_init_tables(void) {
    for (u16 i = 0; i < ANGLE_STEPS; i++) {
        const u16 quadrant = (u16)(i / ANGLE_90);
        const u16 local = (u16)(i & (ANGLE_90 - 1));
        s16 value;

        if (quadrant == 0) {
            value = sin_quarter_q8(local);
        } else if (quadrant == 1) {
            value = sin_quarter_q8((u16)(ANGLE_90 - 1 - local));
        } else if (quadrant == 2) {
            value = (s16)-sin_quarter_q8(local);
        } else {
            value = (s16)-sin_quarter_q8((u16)(ANGLE_90 - 1 - local));
        }

        g_sin_table[i] = value;
    }
}

s16 fx_sin(u16 angle) {
    return g_sin_table[angle & ANGLE_MASK];
}

s16 fx_cos(u16 angle) {
    return g_sin_table[(angle + ANGLE_90) & ANGLE_MASK];
}
