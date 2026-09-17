#include "genesis.h"

__attribute__((externally_visible))
const ROMHeader rom_header = {
    /* "SEGA SSF" selects the Sega/SSF2 bank mapper (BlastEm: MAPPER_SEGA_MED_V2).
     * The cartridge is larger than the 4 MB CPU window: each level's wall pack
     * sits in its own physical banks and src/bsp/level_bank.c maps it into
     * 0x280000-0x3FFFFF (see tools/md_banked.ld). SGDK's own ENABLE_BANK_SWITCH
     * stays off -- nothing uses FAR(), and the mapper is driven directly. */
    "SEGA SSF        ",
    "(C)SGDK 2024    ",
    "SAMPLE PROGRAM                                  ",
    "SAMPLE PROGRAM                                  ",
    "GM 00000000-00",
    0x000,
    "JD              ",
    0x00000000,
    /* The CPU-visible ROM window. The physical image is bigger. */
    0x003FFFFF,
    0xE0FF0000,
    0xE0FFFFFF,
    /* No save RAM. A declared SRAM at 0x200000 would sit inside the resident
     * image on mappers that overlay it there. */
    "  ",
    0x2020,
    0x20202020,
    0x20202020,
    "            ",
    "DEMONSTRATION PROGRAM                   ",
    "JUE             "
};
