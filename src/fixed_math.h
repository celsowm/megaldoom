#ifndef MEGALDOOM_FIXED_MATH_H
#define MEGALDOOM_FIXED_MATH_H

#include <genesis.h>

#define FX_SHIFT 8
#define FX_ONE (1 << FX_SHIFT)
#define ANGLE_STEPS 256
#define ANGLE_MASK (ANGLE_STEPS - 1)
#define ANGLE_90 (ANGLE_STEPS / 4)

// fx_sin/fx_cos are Q8.8 but deliberately NOT unit-amplitude: they return
// 1.1839 * sin(angle), so |(fx_cos(a), fx_sin(a))| is ~303 rather than FX_ONE --
// uniformly across all 256 headings (1.1740..1.1966 * FX_ONE, ratio 1.0192).
// The gain is baked into sin_quarter_q8's coefficients; there is no runtime
// constant for it, because no caller ever needs to apply or remove it.
//
// Why the gain exists: the table's shape was wrong for a long time and its
// magnitude averaged 1.1904*FX_ONE while swinging 1.0645..1.4102 with heading.
// Correcting the shape to unit amplitude would have been geometrically right but
// shrinks view-space depth ~17%, which renders the whole world ~20% larger --
// measured at 2.7x the sprite rasterization cost and 11.79 -> 15.50
// vblanks/frame on tools/routes/checkpoints.txt. Baking the old *mean*
// magnitude into the coefficients keeps today's rendered sizes and movement
// speed while removing the heading-dependent breathing.
//
// Consequences for callers: the gain cancels in any ratio of two basis
// projections (screen-x is PROJ*lateral/depth, so projection is exact), but it
// does NOT cancel where a basis projection is used as an absolute length --
// wall/sprite height, view-space depth, fog banding and movement thrust all
// carry it. Anything needing a true unit vector must divide the gain out.
// tools/test-world-scale.py models thrust as command*THRUST_SCALE, i.e. an
// exactly-unit basis, so real movement runs ~18% above that certified
// walk=283.6u/s figure. That gap predates this gain; it is not new.

void fx_init_tables(void);
s16 fx_sin(u16 angle);
s16 fx_cos(u16 angle);

#endif
