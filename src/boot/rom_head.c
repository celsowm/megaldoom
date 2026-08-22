#include "genesis.h"

__attribute__((externally_visible))
const ROMHeader rom_header = {
#if (ENABLE_BANK_SWITCH != 0)
    "SEGA SSF        ",
#elif (MODULE_MEGAWIFI != 0)
    "SEGA MEGAWIFI   ",
#else
    "SEGA MEGA DRIVE ",
#endif
    "(C)SGDK 2024    ",
    "SAMPLE PROGRAM                                  ",
    "SAMPLE PROGRAM                                  ",
    "GM 00000000-00",
    0x000,
    "JD              ",
    0x00000000,
#if (ENABLE_BANK_SWITCH != 0)
    0x003FFFFF,
#else
    /* The cartridge is a normal linear ROM up to the project's 4 MB cap.
     * Keeping SGDK's 1 MB sample value here makes emulators mirror addresses
     * above 0x0FFFFF once the two-map asset set grows past that boundary. */
    0x003FFFFF,
#endif
    0xE0FF0000,
    0xE0FFFFFF,
    "RA",
    0xF820,
    0x00200000,
    0x0020FFFF,
    "            ",
    "DEMONSTRATION PROGRAM                   ",
    "JUE             "
};
