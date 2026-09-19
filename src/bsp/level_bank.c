#include "level_bank.h"
#include "raycast.h"
#include "generated_assets.h"
#include "generated_map_limits.h"

// Generated with the packs (src/bsp/generated_wall_packs.s): per level, one
// block address per global texture id.
extern const u8 *const megaldoom_level_wall_bases[];
extern const u8 *const megaldoom_level_door_bases[];

// Section .wallpackN is linked at the window and loaded straight after pack
// N - 1, padded to whole 512 KB banks (tools/md_banked.ld). The linker exports
// each pack's load address and padded size as absolute symbols.
#define LEVEL_WINDOW_FIRST_REGION 5
#define LEVEL_WINDOW_REGIONS 3
#define MAPPER_BANK_SHIFT 19 // 512 KB banks
#define PACK_SYMBOLS(n) extern const u8 megaldoom_pack_lma##n[], megaldoom_pack_size##n[]
PACK_SYMBOLS(0); PACK_SYMBOLS(1); PACK_SYMBOLS(2); PACK_SYMBOLS(3);
PACK_SYMBOLS(4); PACK_SYMBOLS(5); PACK_SYMBOLS(6);
static const u32 PACK_LMA[MEGALDOOM_LEVEL_PACK_COUNT] = {
    (u32)megaldoom_pack_lma0, (u32)megaldoom_pack_lma1, (u32)megaldoom_pack_lma2,
    (u32)megaldoom_pack_lma3, (u32)megaldoom_pack_lma4, (u32)megaldoom_pack_lma5,
    (u32)megaldoom_pack_lma6,
};
static const u32 PACK_SIZE[MEGALDOOM_LEVEL_PACK_COUNT] = {
    (u32)megaldoom_pack_size0, (u32)megaldoom_pack_size1, (u32)megaldoom_pack_size2,
    (u32)megaldoom_pack_size3, (u32)megaldoom_pack_size4, (u32)megaldoom_pack_size5,
    (u32)megaldoom_pack_size6,
};
// Region r's bank register (SGDK mapper.h: 0xA130F1 + 2r).
#define MAPPER_BANK_REG(region) ((vu8 *)(0xA130F1UL + 2UL * (region)))

_Static_assert(MEGALDOOM_LEVEL_PACK_COUNT == MEGALDOOM_MAP_COUNT,
               "every campaign map needs exactly one banked wall pack");
_Static_assert(MEGALDOOM_LEVEL_PACK_COUNT == 7,
               "tools/md_banked.ld places exactly seven .wallpackN sections");

// Power-on mapping is the identity, which is level 0's banks.
const u8 *const *g_level_wall_bases = megaldoom_level_wall_bases;
const u8 *const *g_level_door_bases = megaldoom_level_door_bases;

void level_bank_select(u16 level_index) {
    if (level_index >= MEGALDOOM_LEVEL_PACK_COUNT) return;
    const u8 first_bank = (u8)(PACK_LMA[level_index] >> MAPPER_BANK_SHIFT);
    const u16 banks = (u16)(PACK_SIZE[level_index] >> MAPPER_BANK_SHIFT);
    for (u16 i = 0; i < LEVEL_WINDOW_REGIONS; i++) {
        // A region past the end of a short pack is never read. It repeats the
        // pack's first bank rather than showing the next pack or, for the
        // last pack, a bank past the end of the cartridge.
        *MAPPER_BANK_REG(LEVEL_WINDOW_FIRST_REGION + i) =
            (u8)(first_bank + (i < banks ? i : 0));
    }
    const u16 row = (u16)(level_index * FREEDOOM_WALL_TEXTURE_COUNT);
    g_level_wall_bases = megaldoom_level_wall_bases + row;
    g_level_door_bases = megaldoom_level_door_bases + row;
}
