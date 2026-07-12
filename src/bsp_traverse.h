#ifndef MEGALDOOM_BSP_TRAVERSE_H
#define MEGALDOOM_BSP_TRAVERSE_H

#include "bsp_map.h"
#include "raycast.h"

typedef void (*BspTraverseLeafFn)(u16 subsector_id, void *context);
typedef bool (*BspTraverseRangeClosedFn)(u16 left_sample, u16 right_sample,
                                         void *context);
typedef bool (*BspTraverseAllClosedFn)(void *context);

// Shared conservative BSP walk.  Children are visited front-to-back, boxes
// outside the FOV are rejected, and renderer-owned clip state can prune ranges.
void bsp_traverse_front_to_back(const PlayerState *player,
                                BspTraverseLeafFn visit_leaf,
                                BspTraverseRangeClosedFn range_closed,
                                BspTraverseAllClosedFn all_closed,
                                void *context);

#endif
