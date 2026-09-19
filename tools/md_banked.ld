OUTPUT_ARCH(m68k)
SEARCH_DIR(.)
__DYNAMIC  =  0;

/*
 * SGDK's md.ld plus MegalDoom's banked level window (Sega SSF mapper).
 *
 * CPU view while a level is loaded:
 *
 *   0x000000 +--------------------------+
 *            | resident: vectors, code, |  never remapped
 *            | .rodata, resources,      |
 *            | .data load image         |
 *   0x280000 +--------------------------+
 *            | level window, 1.5 MB     |  mapper regions 5-7, switched by
 *            | = that level's wall pack |  src/bsp/level_bank.c at level load
 *   0x3FFFFF +--------------------------+
 *
 * Physical image: pack 0 loads at 0x280000 (banks 5-7, the reset identity
 * mapping), pack N at 0x400000 + (N - 1) * 0x180000. Each pack is padded to
 * the full window, so LEVEL_WINDOW_FIRST_BANK in level_bank.c is 5 + 3 * N.
 * Every pack links at the same VMA, so pointers into it are plain absolute
 * addresses valid whenever its banks are mapped -- no FAR() on the hot path.
 *
 * Nothing but the .wallpackN sections may occupy 0x280000-0x3FFFFF. The
 * overlay's load address collides with .text/.data if the resident image
 * grows past 0x280000, and ld then refuses to link.
 */
MEMORY
{
	rom (rx) : ORIGIN = 0x00000000, LENGTH = 0x00280000
	ram : ORIGIN = 0xE0FF0000, LENGTH = 0x00010000
}

PROVIDE (__stack = 0xE1000000);

SECTIONS
{
  .text 0x00000000:
  {
    KEEP(*(.text.keepboot)) *(.text.*) *(.text)
    . = ALIGN(0x4);
     __CTOR_LIST__ = .;
    LONG((__CTOR_END__ - __CTOR_LIST__) / 4 - 2)
    *(.ctors)
    LONG(0)
    __CTOR_END__ = .;
    __DTOR_LIST__ = .;
    LONG((__DTOR_END__ - __DTOR_LIST__) / 4 - 2)
    *(.dtors)
     LONG(0)
    __DTOR_END__ = .;

    *(.rodata .rodata.*)
    *(.gcc_except_table .gcc_except_table.*)

    . = ALIGN(0x4);
    __INIT_SECTION__ = . ;
    *(.init)
    SHORT (0x4E75)	/* rts */

    __FINI_SECTION__ = . ;
    *(.fini)
    SHORT (0x4E75)	/* rts */

    _etext = .;
    *(.lit)

    *(.rodata_bin)
    *(.rodata_binf)
  } > rom
  _stext = SIZEOF (.text);

  .data 0xE0FF0000 :
  AT ( ADDR (.text) + SIZEOF (.text) )
  {
    *(.got.plt) *(.got)
    *(.shdata)
    *(.data .data.*)
    *(.ramprog .ramprog.*)
    _edata = .;
  } > ram
  _sdata = SIZEOF (.data);

  .bss 0xE0FF0000 + SIZEOF (.data) :
  {
    _start = . ;
    *(.shbss)
    *(.bss .bss.*)
    *(COMMON)
    _bend = . ;
  } > ram

  megaldoom_resident_end = ADDR (.text) + SIZEOF (.text) + SIZEOF (.data);

  OVERLAY 0x00280000 : NOCROSSREFS AT (0x00280000)
  {
    .wallpack0 { KEEP(*(.wallpack0)) . = 0x180000; }
    .wallpack1 { KEEP(*(.wallpack1)) . = 0x180000; }
    .wallpack2 { KEEP(*(.wallpack2)) . = 0x180000; }
    .wallpack3 { KEEP(*(.wallpack3)) . = 0x180000; }
    .wallpack4 { KEEP(*(.wallpack4)) . = 0x180000; }
    .wallpack5 { KEEP(*(.wallpack5)) . = 0x180000; }
  }

  .stab 0 (NOLOAD) :
  {
    *(.stab)
  }

  .stabstr 0 (NOLOAD) :
  {
    *(.stabstr)
  }

  .eh_frame 0 (NOLOAD) :
  {
    *(.eh_frame)
  }
}

ASSERT (megaldoom_resident_end <= 0x00280000,
        "resident image overflows into the banked level window at 0x280000")
