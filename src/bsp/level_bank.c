#include "level_bank.h"
#include "raycast.h"
#include "generated_assets.h"
#include "generated_map_limits.h"

// Generated with the packs (src/bsp/generated_wall_packs.s): per level, one
// block address per global texture id.
extern const u8 *const megaldoom_level_wall_bases[];
extern const u8 *const megaldoom_level_door_bases[];

// Section .wallpackN is linked at the window and loaded at physical
// 0x280000 + N * 0x180000, i.e. banks 5 + 3N .. 7 + 3N (tools/md_banked.ld).
#define LEVEL_WINDOW_FIRST_REGION 5
#define LEVEL_WINDOW_REGIONS 3
// Region r's bank register (SGDK mapper.h: 0xA130F1 + 2r).
#define MAPPER_BANK_REG(region) ((vu8 *)(0xA130F1UL + 2UL * (region)))

_Static_assert(MEGALDOOM_LEVEL_PACK_COUNT == MEGALDOOM_MAP_COUNT,
               "every campaign map needs exactly one banked wall pack");
_Static_assert(MEGALDOOM_LEVEL_PACK_COUNT == 5,
               "tools/md_banked.ld places exactly five .wallpackN sections");

// Power-on mapping is the identity, which is level 0's banks.
const u8 *const *g_level_wall_bases = megaldoom_level_wall_bases;
const u8 *const *g_level_door_bases = megaldoom_level_door_bases;

void level_bank_select(u16 level_index) {
    if (level_index >= MEGALDOOM_LEVEL_PACK_COUNT) return;
    const u8 first_bank = (u8)(LEVEL_WINDOW_FIRST_REGION +
                               LEVEL_WINDOW_REGIONS * level_index);
    for (u16 i = 0; i < LEVEL_WINDOW_REGIONS; i++) {
        *MAPPER_BANK_REG(LEVEL_WINDOW_FIRST_REGION + i) = (u8)(first_bank + i);
    }
    const u16 row = (u16)(level_index * FREEDOOM_WALL_TEXTURE_COUNT);
    g_level_wall_bases = megaldoom_level_wall_bases + row;
    g_level_door_bases = megaldoom_level_door_bases + row;
}
