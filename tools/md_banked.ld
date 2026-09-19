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
 * Physical image: pack 0 loads at 0x280000 (bank 5 onwards, the reset
 * identity mapping) and each later pack straight after the one before. A pack
 * is padded only to the next 512 KB mapper bank, not to the whole window, so a
 * small level takes two banks instead of three; level_bank.c reads each pack's
 * first bank from the megaldoom_pack_lmaN symbols below, and never maps a bank
 * past the end of the pack.
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
    .wallpack0 { KEEP(*(.wallpack0)) . = ALIGN(0x80000); }
    .wallpack1 { KEEP(*(.wallpack1)) . = ALIGN(0x80000); }
    .wallpack2 { KEEP(*(.wallpack2)) . = ALIGN(0x80000); }
    .wallpack3 { KEEP(*(.wallpack3)) . = ALIGN(0x80000); }
    .wallpack4 { KEEP(*(.wallpack4)) . = ALIGN(0x80000); }
    .wallpack5 { KEEP(*(.wallpack5)) . = ALIGN(0x80000); }
    .wallpack6 { KEEP(*(.wallpack6)) . = ALIGN(0x80000); }
  }

  /* Each pack's load address and padded size, read by src/bsp/level_bank.c. */
  megaldoom_pack_lma0 = LOADADDR (.wallpack0);
  megaldoom_pack_size0 = SIZEOF (.wallpack0);
  megaldoom_pack_lma1 = LOADADDR (.wallpack1);
  megaldoom_pack_size1 = SIZEOF (.wallpack1);
  megaldoom_pack_lma2 = LOADADDR (.wallpack2);
  megaldoom_pack_size2 = SIZEOF (.wallpack2);
  megaldoom_pack_lma3 = LOADADDR (.wallpack3);
  megaldoom_pack_size3 = SIZEOF (.wallpack3);
  megaldoom_pack_lma4 = LOADADDR (.wallpack4);
  megaldoom_pack_size4 = SIZEOF (.wallpack4);
  megaldoom_pack_lma5 = LOADADDR (.wallpack5);
  megaldoom_pack_size5 = SIZEOF (.wallpack5);
  megaldoom_pack_lma6 = LOADADDR (.wallpack6);
  megaldoom_pack_size6 = SIZEOF (.wallpack6);

  /* ALIGN no longer stops a pack at the window, so bound each one here. */
  ASSERT (SIZEOF (.wallpack0) <= 0x180000, "level pack 0 overflows the 1.5 MB level window")
  ASSERT (SIZEOF (.wallpack1) <= 0x180000, "level pack 1 overflows the 1.5 MB level window")
  ASSERT (SIZEOF (.wallpack2) <= 0x180000, "level pack 2 overflows the 1.5 MB level window")
  ASSERT (SIZEOF (.wallpack3) <= 0x180000, "level pack 3 overflows the 1.5 MB level window")
  ASSERT (SIZEOF (.wallpack4) <= 0x180000, "level pack 4 overflows the 1.5 MB level window")
  ASSERT (SIZEOF (.wallpack5) <= 0x180000, "level pack 5 overflows the 1.5 MB level window")
  ASSERT (SIZEOF (.wallpack6) <= 0x180000, "level pack 6 overflows the 1.5 MB level window")

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
