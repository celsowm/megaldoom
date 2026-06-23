#ifndef MEGALDOOM_FIXED_MATH_H
#define MEGALDOOM_FIXED_MATH_H

#include <genesis.h>

#define FX_SHIFT 8
#define FX_ONE (1 << FX_SHIFT)
#define ANGLE_STEPS 256
#define ANGLE_MASK (ANGLE_STEPS - 1)
#define ANGLE_90 (ANGLE_STEPS / 4)

void fx_init_tables(void);
s16 fx_sin(u16 angle);
s16 fx_cos(u16 angle);

#endif
