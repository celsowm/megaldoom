#include "fixed_math.h"

static s16 g_sin_table[ANGLE_STEPS];

static s16 sin_quarter_q8(u16 angle) {
    const s32 a = (s32)(angle & (ANGLE_90 - 1));
    const s32 x = (a * FX_ONE) / ANGLE_90;
    const s32 x2 = (x * x) >> FX_SHIFT;
    const s32 x3 = (x2 * x) >> FX_SHIFT;
    const s32 x5 = (x3 * x2) >> FX_SHIFT;
    // Taylor series for sin(x*pi/2), x in [0,1), pre-scaled by the 1.1839 basis
    // gain that fixed_math.h documents. The unit-amplitude Q8.8 coefficients are
    // pi/2 == 402, (pi/2)^3/3! == 165 and (pi/2)^5/5! == 20; each below is that
    // value times the gain.
    //
    // The 3rd/5th terms used to be 41 and 5 -- each exactly a quarter of correct
    // -- which made this a near-linear ramp. Because the view basis is
    // (cos, sin) with right = perp(forward), a magnitude error scales depth and
    // lateral together: screen-x stays correct (the scale cancels in
    // PROJ*lateral/depth) but wall height goes as 1/|basis|. |basis|/256 swung
    // 1.0645..1.4102 with heading, so every wall AND sprite pulsed 32.5% with a
    // 90-degree period as the player turned -- a wall 256 units dead ahead
    // measured 28px facing an axis and 37px facing diagonally. Fixing the shape
    // drops that ratio to 1.0192 and the heading error from -3.03..+4.43 deg to
    // -0.19..+1.60. See AGENTS.md.
    const s32 term1 = 479 * x; // 402 * FX_BASIS_GAIN
    const s32 term2 = 196 * x3; // 165 * FX_BASIS_GAIN
    const s32 term3 = 24 * x5;  //  20 * FX_BASIS_GAIN

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
