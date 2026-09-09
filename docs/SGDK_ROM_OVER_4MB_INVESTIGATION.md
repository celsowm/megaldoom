# SGDK ROM Sizes Beyond 4 MB: Technical Investigation and MegaLDOOM Recommendations

**Investigation date:** 2026-09-09  
**Target project:** MegaLDOOM  
**Upstream investigated:** [Stephane-D/SGDK](https://github.com/Stephane-D/SGDK), current `master` branch at the time of the investigation

## Executive summary

The common statement that a Sega Mega Drive / Genesis game built with SGDK is limited to a 4 MB ROM is incorrect.

The 4 MB number is the size of the normal cartridge ROM address window visible to the Motorola 68000 at one time. SGDK itself already contains explicit support for cartridges larger than 4 MB by using the official Sega mapper convention associated with Super Street Fighter II (SSF2-style banking).

The most important conclusions are:

1. **4 MB is the normal flat, directly visible ROM window.**
2. **The SGDK linker does not impose a 4 MB output-size limit.** Its ROM memory region is intentionally much larger.
3. **SGDK implements the Sega bank-switching mapper directly** in `mapper.c` / `mapper.h`.
4. The mapper works with **512 KB pages** and **64 possible page indices**.
5. Therefore, the SGDK mapper implementation has a real addressable ceiling of **32 MB** (`64 * 512 KB`).
6. SGDK's resource compiler deliberately separates normal and far binary data, placing far payloads into `.rodata_binf` near the end of the ROM image.
7. Many SGDK subsystems already use `FAR()` / `FAR_SAFE()` internally, including tiles, maps, bitmaps, sprites and XGM/XGM2 music paths.
8. SGDK's automatic mechanism is principally a **far-data banking system**, not an automatic banked-code system. Executable code should remain resident in the directly addressable area.
9. `SYS_getFarData()` uses the two top 512 KB windows (`0x300000-0x37FFFF` and `0x380000-0x3FFFFF`) for far-data access. For a robust large-ROM design, MegaLDOOM should keep resident code and critical near data below `0x300000`.
10. `FAR_SAFE()` can make an object crossing one 512 KB boundary contiguous by mapping two adjacent pages, but it is **not a general-purpose multi-megabyte streaming mapper**. Very large payloads must be chunked or streamed.
11. SGDK's current mapper masks the bank number with `0x3F`. A ROM file can theoretically be linked larger than 32 MB, but data above 32 MB will alias back into the first 32 MB under the current mapper logic.
12. The most practical target for MegaLDOOM is **16 MB or 32 MB with the SSF mapper**, using resident code plus banked WAD-derived assets.

The rest of this document explains these conclusions from the SGDK source code and translates them into an architecture suitable for MegaLDOOM.

---

## 1. The source of the 4 MB limit

The Motorola 68000 in the Mega Drive normally sees cartridge ROM in the range:

```text
0x000000  +----------------------------+
          |                            |
          |    cartridge ROM window    |
          |                            |
0x3FFFFF  +----------------------------+
```

That is a 4 MB CPU address window.

This does **not** imply that the physical cartridge must contain only 4 MB of ROM. A cartridge can contain additional address-decoding logic or a mapper that changes which physical ROM region appears inside the CPU-visible window.

SGDK already knows about exactly such a mapper.

The relevant upstream files are:

- [`inc/mapper.h`](https://github.com/Stephane-D/SGDK/blob/master/inc/mapper.h)
- [`src/mapper.c`](https://github.com/Stephane-D/SGDK/blob/master/src/mapper.c)
- [`inc/config.h`](https://github.com/Stephane-D/SGDK/blob/master/inc/config.h)

The header comment in `mapper.h` is explicit:

> This unit provides tools to deal with ROM larger than 4MB.

That statement is important: large-ROM support is not an accidental side effect of the linker. It is an intentional SGDK feature.

---

## 2. The linker does not impose a 4 MB ROM limit

SGDK's linker script is [`md.ld`](https://github.com/Stephane-D/SGDK/blob/master/md.ld).

Its ROM memory region is defined as:

```ld
MEMORY
{
    rom (rx) : ORIGIN = 0x00000000, LENGTH = 0xE0000000
    ram      : ORIGIN = 0xE0FF0000, LENGTH = 0x00010000
}
```

The ROM region is therefore **not** defined as 4 MB.

`0xE0000000` is approximately 3.5 GiB (about 3.76 GB decimal). This is a linker-address-space allowance, not a claim that the Mega Drive can physically map a 3.8 GB cartridge.

The distinction is:

```text
linker output capacity != hardware-visible cartridge address space
```

The linker permits a very large output layout, while the runtime mapper determines what the console can actually reach.

This explains why SGDK history has referred to very large *theoretical* ROM sizes: the linker is deliberately permissive. The effective runtime limit must be derived from the mapper implementation, not from `md.ld` alone.

### `.text`, near binary data and far binary data

The important section ordering in `md.ld` includes:

```ld
*(.rodata .rodata.*)
...
*(.rodata_bin)
*(.rodata_binf)
```

This ordering matters because the resource compiler deliberately places far binary payloads in `.rodata_binf`, allowing code and metadata to remain before large asset payloads in the final ROM image.

---

## 3. The build system also does not enforce 4 MB

The standard SGDK build path in [`makefile.gen`](https://github.com/Stephane-D/SGDK/blob/master/makefile.gen) performs the following general sequence:

```text
compile objects
      |
      v
link with md.ld
      |
      v
rom.out
      |
      v
objcopy -O binary
      |
      v
rom.bin
      |
      v
sizebnd padding + checksum
```

The build rule uses `objcopy` to generate a raw binary and then calls SGDK's `sizebnd` utility.

The implementation of `sizebnd` is available in:

- [`tools/sizebnd/src/sgdk/sizebnd/Launcher.java`](https://github.com/Stephane-D/SGDK/blob/master/tools/sizebnd/src/sgdk/sizebnd/Launcher.java)

It reads the entire file into a Java byte array, pads it to the requested alignment and calculates the SGDK checksum. There is no normal 4 MB rejection test in this tool.

Therefore, generating a `rom.bin` larger than 4 MB is not inherently a problem for SGDK's output pipeline.

The important limitation is whether the resulting addresses can be mapped correctly at runtime.

---

## 4. SGDK implements the official Sega / SSF2 mapper

`mapper.h` documents the banking design directly.

The 4 MB cartridge window is divided into eight regions of 512 KB each:

```text
CPU-visible ROM window

region 0  0x000000 - 0x07FFFF   fixed
region 1  0x080000 - 0x0FFFFF   switchable
region 2  0x100000 - 0x17FFFF   switchable
region 3  0x180000 - 0x1FFFFF   switchable
region 4  0x200000 - 0x27FFFF   switchable
region 5  0x280000 - 0x2FFFFF   switchable
region 6  0x300000 - 0x37FFFF   switchable
region 7  0x380000 - 0x3FFFFF   switchable
```

The first 512 KB region is fixed because it contains the vector table and boot-critical contents.

The mapper registers documented by SGDK are:

```text
0xA130F3 -> CPU window 0x080000 - 0x0FFFFF
0xA130F5 -> CPU window 0x100000 - 0x17FFFF
0xA130F7 -> CPU window 0x180000 - 0x1FFFFF
0xA130F9 -> CPU window 0x200000 - 0x27FFFF
0xA130FB -> CPU window 0x280000 - 0x2FFFFF
0xA130FD -> CPU window 0x300000 - 0x37FFFF
0xA130FF -> CPU window 0x380000 - 0x3FFFFF
```

The implementation defines:

```c
#define MAPPER_BASE     0xA130F1
#define BANK_SIZE       0x80000
#define BANK_IN_MASK    (BANK_SIZE - 1)
```

`0x80000` is exactly 512 KB.

The low-level bank setter is effectively:

```c
void SYS_setBank(u16 regionIndex, u16 bankIndex)
{
    if ((regionIndex > 0) && (regionIndex < 8))
    {
        *(vu8*)(MAPPER_BASE + (regionIndex * 2)) = bankIndex;
        banks[regionIndex] = bankIndex;
    }
}
```

This is actual runtime mapper control, not just metadata in the ROM header.

---

## 5. Why the SGDK mapper ceiling is 32 MB

This is the most important hard limit found in the current implementation.

`mapper.h` explains that a page is selected using six bits, producing 64 possible physical pages.

Each page is 512 KB:

```text
64 pages * 512 KB = 32 MB
```

The implementation in `mapper.c` confirms this with:

```c
const u16 bankIndex = (addr >> 19) & 0x3F;
```

Why `>> 19`?

```text
2^19 bytes = 524,288 bytes = 512 KB
```

Why `& 0x3F`?

```text
0x3F = 63
valid selected values = 0..63
```

Therefore the logical far-address mapping is:

```text
bank  0 -> 0x0000000 - 0x007FFFF
bank  1 -> 0x0080000 - 0x00FFFFF
...
bank 63 -> 0x1F80000 - 0x1FFFFFF
```

`0x02000000` is exactly 32 MB.

At that point:

```c
(addr >> 19) & 0x3F
```

wraps to bank zero again.

### Consequence

A ROM can theoretically be linked or emitted larger than 32 MB, but the current SGDK mapper logic cannot uniquely address data above the first 32 MB.

For example, conceptually:

```text
logical address 0x0000000 -> bank 0
logical address 0x2000000 -> bank 0 again after & 0x3F
logical address 0x4000000 -> bank 0 again
```

This makes **32 MB the meaningful maximum for the stock SGDK SSF mapper implementation**.

Going beyond 32 MB would require a different mapper protocol and corresponding changes to the runtime logic. Merely changing the linker limit would not solve it.

---

## 6. Enabling bank switching in SGDK

The feature is controlled in [`inc/config.h`](https://github.com/Stephane-D/SGDK/blob/master/inc/config.h):

```c
#define ENABLE_BANK_SWITCH 0
```

The upstream comment describes it as automatic bank switching using the official Sega mapper for ROMs larger than 4 MB.

For a banked build it must become:

```c
#define ENABLE_BANK_SWITCH 1
```

This is a library compile-time setting. The SGDK library must be rebuilt after changing it.

This requirement is also confirmed by SGDK maintainer Stephane Dallongeville in [SGDK issue #305](https://github.com/Stephane-D/SGDK/issues/305), where he states that SGDK includes an automatic bank-switch mechanism but that it must be enabled through `inc/config.h` and the library rebuilt.

During system initialization, SGDK calls `SYS_resetBanks()` when bank switching is enabled, restoring the expected initial mapping.

The initial mapper state tracked by `mapper.c` is:

```c
static u16 banks[8] = {0, 1, 2, 3, 4, 5, 6, 7};
```

That means the normal first 4 MB initially appears as a direct 1:1 mapping before dynamic bank changes occur.

---

## 7. ROM header behavior changes when banking is enabled

SGDK's boot ROM header implementation is:

- [`src/boot/rom_header.c`](https://github.com/Stephane-D/SGDK/blob/master/src/boot/rom_header.c)

When bank switching is enabled, SGDK changes the console identifier from the normal string to the SSF identifier:

```c
#if (ENABLE_BANK_SWITCH != 0)
    "SEGA SSF        ",
#else
    "SEGA MEGA DRIVE ",
#endif
```

The declared visible ROM end also becomes:

```c
0x003FFFFF
```

under bank-switch mode.

This is consistent with the mapper architecture: the CPU-visible cartridge aperture is still 4 MB even if the physical image contains much more ROM.

Conceptually:

```text
physical ROM: 4 MB ... 32 MB
        |
        | bank selection
        v
CPU-visible aperture: 0x000000 ... 0x3FFFFF
```

The header does not need to expose a linear 32 MB CPU range because no such linear range exists on the normal 68000 cartridge bus.

---

## 8. SGDK's ResComp is designed for far data

SGDK's resource compiler deliberately distinguishes near and far binary data.

The relevant code is in:

- [`tools/rescomp/src/sgdk/rescomp/Compiler.java`](https://github.com/Stephane-D/SGDK/blob/master/tools/rescomp/src/sgdk/rescomp/Compiler.java)
- [`tools/rescomp/src/sgdk/rescomp/processor/BinProcessor.java`](https://github.com/Stephane-D/SGDK/blob/master/tools/rescomp/src/sgdk/rescomp/processor/BinProcessor.java)

The `BIN` resource syntax is:

```text
BIN name file [align [size_align [fill [compression [far]]]]]
```

The implementation initializes the flag as:

```java
boolean far = true;
```

So raw `BIN` resources are far by default unless explicitly overridden.

The compiler's own help text describes the flag as placing binary data at the end of the ROM, useful for bank switching.

### Section placement

ResComp exports normal binary resources into:

```asm
.section .rodata_bin
```

and, unless near mode has been forced, emits far binary data into:

```asm
.section .rodata_binf
```

Because `md.ld` places `.rodata_binf` after the normal code/rodata/binary sections, far payloads naturally accumulate near the end of the ROM.

The intended broad layout is therefore:

```text
ROM start
   |
   +-- boot / vectors
   +-- executable code
   +-- normal rodata
   +-- metadata
   +-- near binary data (.rodata_bin)
   +-- far binary payloads (.rodata_binf)
   |
ROM end
```

This is exactly the kind of organization MegaLDOOM needs: the engine and runtime stay resident while WAD-derived graphics, maps, music and other large payloads occupy banked physical ROM.

### Documentation naming inconsistency

The current `config.h` comment refers to a `.far_rodata` section, but the current ResComp/linker path uses `.rodata_binf`.

For implementation work, the actual current compiler and linker section names should be treated as authoritative.

---

## 9. `FAR()` and `FAR_SAFE()` are the core accessors

`mapper.h` defines:

```c
#if (ENABLE_BANK_SWITCH != 0)
    #define FAR(data) SYS_getFarData((void*) (data))
    #define FAR_SAFE(data, size) SYS_getFarDataSafe((void*) (data), size)
#else
    #define FAR(data) data
    #define FAR_SAFE(data, size) data
#endif
```

This design is useful because code can be written in a way that remains valid for both small flat-ROM builds and large banked builds.

When banking is disabled, the macros collapse to direct pointers.

When banking is enabled, they perform the runtime mapping needed to make the requested data visible inside a currently mapped 512 KB region.

---

## 10. Which CPU windows `SYS_getFarData()` actually uses

Although the mapper hardware exposes seven switchable regions, SGDK's automatic far-data helper deliberately uses the final two regions as scratch mapping windows:

```text
region 6: 0x300000 - 0x37FFFF
region 7: 0x380000 - 0x3FFFFF
```

`mapper.h` explicitly warns that `SYS_getFarData()` uses the `0x00300000-0x003FFFFF` range.

The implementation alternates between these two regions and tries to reuse an already mapped bank when possible.

This has a major architectural consequence.

### Recommended resident range for MegaLDOOM

The following is a **design recommendation derived from SGDK's mapper behavior**, not a linker-enforced rule:

```text
0x000000  +----------------------------------+
          | vectors / boot                   |
          | MegaLDOOM executable code        |
          | engine tables                    |
          | critical metadata                |
          | permanent near data              |
0x2FFFFF  +----------------------------------+
0x300000  | FAR mapping window A, 512 KB     |
0x37FFFF  +----------------------------------+
0x380000  | FAR mapping window B, 512 KB     |
0x3FFFFF  +----------------------------------+
```

In other words, for a robust automatic-bank-switch design, MegaLDOOM should aim to keep **all permanent resident executable code and data below `0x300000`**.

That leaves roughly 3 MB of the normal 4 MB aperture for stable resident contents and reserves the upper 1 MB for transient far-data mappings.

This 3 MB figure is **not** a hard SGDK compiler limit. The linker can place data above it. The risk is semantic: anything resident in those upper windows can disappear when `SYS_getFarData()` remaps them.

A build can therefore succeed while still containing a dangerous runtime layout.

MegaLDOOM should add an explicit link-map or symbol-range check rather than relying on the linker to catch this automatically.

---

## 11. Why executable code should remain near

No evidence was found in the current SGDK codebase of an automatic system that assigns C functions to banks and inserts call trampolines around them.

The stock automatic mechanism operates on data access through `FAR()` / `FAR_SAFE()`.

There is no general transformation resembling:

```text
normal C call
    |
    v
select function bank
    |
    v
jump through mapped window
    |
    v
return
    |
    v
restore previous bank
```

Therefore MegaLDOOM should not assume that simply allowing `.text` to grow across a large physical ROM will produce correctly executable banked code.

The safe architecture is:

```text
resident engine code
resident gameplay code
resident interrupt code
resident metadata needed for dispatch
          |
          +---- accesses ----> banked assets
```

instead of:

```text
banked engine code + banked game code + banked assets
```

A custom code-overlay system is possible in principle, but it would be a separate engineering project with significant complexity around calls, callbacks, interrupt safety, function pointers and mapper state.

For MegaLDOOM, far assets provide the large majority of the useful ROM-size win without that complexity.

---

## 12. `FAR_SAFE()` handles one bank crossing, not arbitrary-size streaming

A 512 KB page boundary can bisect a resource.

`SYS_getFarDataSafe()` solves the common case by detecting whether the requested data range crosses a page boundary.

Internally, SGDK can then map two consecutive pages into the two far windows:

```c
SYS_setBank(6, bankIndex + 0);
SYS_setBank(7, bankIndex + 1);
```

This gives the CPU a contiguous temporary view spanning up to two adjacent 512 KB pages:

```text
0x300000 ... 0x37FFFF -> physical page N
0x380000 ... 0x3FFFFF -> physical page N+1
```

That is excellent for normal graphics, map blocks, compressed resources, songs and similar assets.

However, it does **not** turn an arbitrarily large object into one huge contiguous pointer.

A multi-megabyte payload cannot safely be treated as:

```c
const void* p = FAR_SAFE(huge_4mb_blob, 4 * 1024 * 1024);
consume_all_4mb(p);
```

because only two mapping windows exist in this automatic path.

### MegaLDOOM implication

Large WAD-derived structures should be subdivided into independent records or streamed in chunks.

Good examples:

```text
texture page
sprite frame group
flat group
map lump
sound lump
music track
compressed block
sector/geometry block
```

Bad example:

```text
one monolithic multi-megabyte binary that must remain continuously addressable
```

A good resource packer should deliberately control chunk sizes and alignment.

---

## 13. FAR pointers have transient lifetime semantics

A pointer returned by `SYS_getFarData()` points into one of SGDK's temporary mapping windows.

For example:

```c
const u8* a = FAR(assetA);
const u8* b = FAR(assetB);
```

The second call may remap the same CPU window that previously exposed `assetA`.

Therefore `a` is not necessarily a permanently valid pointer after subsequent far accesses.

The correct conceptual model is:

```text
far address = stable logical identifier/address
mapped pointer = temporary view
```

rather than:

```text
far address = permanently dereferenceable CPU pointer
```

### Safe usage pattern

Prefer:

```c
const u8* p = FAR(asset);
consume_immediately(p);
```

or copy/decompress/upload the resource while the mapping is known to be valid.

Avoid storing mapped pointers for arbitrary later use unless mapper ownership and lifetime are tightly controlled.

For MegaLDOOM this argues strongly for a resource API such as:

```text
logical resource ID
      |
      v
resource directory entry
      |
      v
map required bank(s)
      |
      v
consume / copy / DMA / decompress
      |
      v
mapping may be reused
```

instead of exposing arbitrary far pointers throughout gameplay code.

---

## 14. Existing SGDK subsystems already understand far assets

A major positive result of the investigation is that far support is not limited to raw user code.

The SGDK codebase contains many internal `FAR_SAFE()` uses.

Examples include:

### Tile loading

[`src/vdp_tile.c`](https://github.com/Stephane-D/SGDK/blob/master/src/vdp_tile.c) uses far-safe tile pointers before VDP upload.

Conceptually:

```c
VDP_loadTileData(
    FAR_SAFE(tileset->tiles + offset, size),
    ...
);
```

### Maps

[`src/map.c`](https://github.com/Stephane-D/SGDK/blob/master/src/map.c) accesses map data and metatile payloads through far-safe mapping where appropriate.

### Bitmap graphics

[`src/bmp.c`](https://github.com/Stephane-D/SGDK/blob/master/src/bmp.c) uses `FAR_SAFE()` for bitmap image data.

### Background / VDP image paths

[`src/vdp_bg.c`](https://github.com/Stephane-D/SGDK/blob/master/src/vdp_bg.c) uses far-safe access when loading bitmap tile data.

### Sprite engine

[`src/sprite_eng.c`](https://github.com/Stephane-D/SGDK/blob/master/src/sprite_eng.c) accesses tileset payloads with `FAR_SAFE()` during upload/decompression paths.

### Generic tooling / image data

[`src/tools.c`](https://github.com/Stephane-D/SGDK/blob/master/src/tools.c) contains far-safe accesses for image data and decompression paths.

This means a banked MegaLDOOM does not need to replace SGDK's entire graphics pipeline. The framework already contains support in many of the places where large assets are consumed.

That said, project-specific direct pointer access still needs to be audited.

---

## 15. XGM and XGM2 have explicit FAR paths

Sound data can consume a significant fraction of a cartridge, so this is particularly relevant.

SGDK includes APIs such as:

```c
XGM_startPlay_FAR(...)
XGM_setPCM_FAR(...)
```

and for XGM2:

```c
XGM2_load_FAR(...)
XGM2_play_FAR(...)
```

Relevant upstream files include:

- [`inc/snd/xgm.h`](https://github.com/Stephane-D/SGDK/blob/master/inc/snd/xgm.h)
- [`src/snd/xgm.c`](https://github.com/Stephane-D/SGDK/blob/master/src/snd/xgm.c)
- [`inc/snd/xgm2.h`](https://github.com/Stephane-D/SGDK/blob/master/inc/snd/xgm2.h)
- [`src/snd/xgm2.c`](https://github.com/Stephane-D/SGDK/blob/master/src/snd/xgm2.c)

For example, the FAR song wrapper resolves the banked pointer with `FAR_SAFE()` before passing it to the normal playback path.

This makes it practical to place music tracks and PCM-heavy data in the far portion of a 16 MB or 32 MB MegaLDOOM ROM.

---

## 16. Historical SGDK large-ROM evidence

SGDK's [`changelog.txt`](https://github.com/Stephane-D/SGDK/blob/master/changelog.txt) contains several large-ROM / mapper-related changes over time.

Notable entries include references to:

- increasing a maximum ROM size to 12 MB in an older stage of development;
- adding `SYS_getFarDataEx()` and `SYS_getFarDataSafeEx()`;
- adding `SYS_isCrossingBank()`;
- fixing RAM mapping for bank switching with ROMs larger than 12 MB;
- adding FAR XGM PCM support;
- changing SGDK methods to use `FAR_SAFE()` for bank-crossing resources.

These historical entries reinforce that ROM sizes above 4 MB have been an actively supported scenario rather than an unsupported accident.

Older limits mentioned in the changelog should not be confused with the current mapper's 32 MB page-index capacity.

---

## 17. An 8 MB alternative without the SSF mapper

In [SGDK issue #305](https://github.com/Stephane-D/SGDK/issues/305), Stephane Dallongeville also notes that an 8 MB configuration is possible without ordinary bank switching and with very little additional hardware logic.

He also notes a trade-off: that configuration prevents use of the Mega-CD in that arrangement.

This is a different hardware design from the normal SSF mapper approach.

Conceptually, one can think of the distinction as:

### SSF-style banking

```text
large physical ROM
       |
       v
bank mapper
       |
       v
4 MB CPU-visible window
```

### Alternate 8 MB arrangement

```text
hardware exposes a larger/simple arrangement using otherwise conflicting address space
```

For MegaLDOOM, the SSF mapper is the stronger default because:

- SGDK already implements it;
- it scales to 32 MB;
- the far-data APIs already integrate with it;
- project code can remain mostly mapper-agnostic;
- it avoids creating a project-specific 8 MB memory model solely to gain an extra 4 MB.

---

## 18. Practical ROM-size tiers

The investigation produces the following useful engineering matrix.

| ROM design | Practical capacity | SGDK support | Notes |
|---|---:|---|---|
| Flat cartridge | 4 MB | Native/default | Entire visible ROM is directly addressable |
| Special simple 8 MB hardware layout | 8 MB | Possible, not the normal automatic FAR design | Maintainer notes Mega-CD trade-off |
| SSF mapper | Up to 32 MB | Native SGDK mapper support | Recommended for MegaLDOOM |
| Linker-only oversized image | Much larger theoretically | Linker can emit it | Not uniquely addressable by current mapper above 32 MB |
| Custom mapper beyond stock SGDK | Potentially >32 MB | Requires new runtime + hardware/emulator support | Separate engineering project |

The key distinction is that **file size** and **runtime addressability** are separate concerns.

---

## 19. Proposed MegaLDOOM memory architecture

The recommended high-level cartridge design is:

```text
PHYSICAL ROM IMAGE, target 16 MB or 32 MB

0x000000  +---------------------------------------------+
          | vectors / SGDK boot                         |
          | MegaLDOOM engine code                       |
          | renderer code                               |
          | gameplay code                               |
          | audio control code                          |
          | permanent tables                            |
          | resource directory / lump metadata          |
          | small near assets                           |
          | critical strings / state metadata           |
~0x2FFFFF +---------------------------------------------+
          |                                             |
          | FAR payload area begins conceptually here   |
          |                                             |
          | WAD-derived textures                        |
          | wall patches                                |
          | flats                                       |
          | sprites                                     |
          | enemy frames                                |
          | weapon frames                               |
          | map geometry/lumps                          |
          | music                                       |
          | PCM/SFX payloads                            |
          | cutscene/title assets                       |
          | compressed resource packs                   |
          |                                             |
0x1FFFFFF +---------------------------------------------+
          32 MB maximum for stock SGDK mapper
```

The exact physical ordering can be optimized by the resource packer. The important invariant is that resident runtime code must not depend on staying mapped in the SGDK FAR windows.

### Runtime aperture

At any instant the CPU sees:

```text
0x000000  +----------------------------------+
          | stable resident region            |
          |                                  |
0x2FFFFF  +----------------------------------+
0x300000  | FAR window A -> selected 512 KB  |
0x37FFFF  +----------------------------------+
0x380000  | FAR window B -> selected 512 KB  |
0x3FFFFF  +----------------------------------+
```

The physical pages selected into A and B can come from anywhere in the first 32 MB of ROM.

---

## 20. Recommended MegaLDOOM resource model

MegaLDOOM should avoid allowing gameplay systems to depend directly on persistent far pointers.

A WAD-inspired resource directory is a natural abstraction.

For example:

```c
typedef struct
{
    u32 romOffset;
    u32 packedSize;
    u32 unpackedSize;
    u16 type;
    u16 flags;
} MegaLump;
```

A logical resource lookup could produce a descriptor rather than a raw permanently mapped pointer:

```text
resource name / ID
      |
      v
MegaLump
      |
      +-- physical ROM offset
      +-- packed length
      +-- unpacked length
      +-- compression
      +-- resource type
      |
      v
bank-aware loader
```

The loader can then choose the correct strategy:

```text
small resource
    -> FAR/FAR_SAFE
    -> consume immediately

compressed graphics
    -> FAR_SAFE
    -> decompress to RAM/temp DMA buffer
    -> upload to VRAM

large sequential payload
    -> iterate bank-sized chunks
    -> stream/copy/decompress incrementally

music
    -> XGM/XGM2 FAR API
```

This architecture isolates mapper behavior from the rest of the game.

---

## 21. Resource chunking strategy for MegaLDOOM

The 512 KB bank size should influence the asset packer.

Recommended rules:

1. Prefer resources much smaller than 512 KB.
2. If a resource can cross a bank boundary, use a path that knows its exact size and therefore can use `FAR_SAFE()`.
3. Do not intentionally create single resources larger than the two-window contiguous mapping capacity unless the loader explicitly streams them.
4. Consider aligning large frequently accessed resource groups to bank boundaries.
5. Group resources that are usually consumed together into the same 512 KB page where possible.
6. Avoid unnecessarily remapping banks multiple times during one render/update operation.
7. Keep metadata needed to locate far payloads resident and small.
8. Prefer compressed payloads where decompression cost is acceptable, because compression reduces both physical ROM consumption and bank-switch frequency.

### Possible physical packing heuristic

A future packer could use groups such as:

```text
bank group: E1M1 map + E1M1-only textures
bank group: common enemies
bank group: weapon graphics
bank group: common walls/flats
bank group: boss assets
bank group: music tracks
bank group: SFX/PCM
```

The optimal grouping depends on MegaLDOOM's actual runtime access patterns and should eventually be profile-guided.

---

## 22. Build-time checks MegaLDOOM should add

Because SGDK's linker is permissive, MegaLDOOM should enforce its own cartridge-layout invariants.

### 22.1 Resident-code boundary check

After linking, inspect the ELF symbols or map file and fail the build if executable code or critical resident objects extend into the automatic FAR window.

Recommended invariant:

```text
resident end <= 0x300000
```

Ideally use a smaller warning threshold to preserve growth headroom.

For example:

```text
warning threshold: 0x280000
hard threshold:    0x300000
```

The exact warning threshold can be adjusted after measuring current MegaLDOOM usage.

### 22.2 Physical ROM-size check

Fail if the final banked image exceeds:

```text
0x02000000 bytes = 32 MB
```

unless/until MegaLDOOM deliberately implements another mapper.

### 22.3 Far-resource size check

Warn or fail when a resource exceeds the loader's supported contiguous access size.

For an ordinary `FAR_SAFE()`-based path, resources approaching 1 MB need careful inspection.

### 22.4 Bank-boundary report

Generate a report showing:

```text
resource
physical offset
physical bank number
size
crosses bank? yes/no
compression
runtime loader
```

This would make mapper problems visible during development rather than as emulator/hardware crashes.

### 22.5 Near/far classification report

Every generated resource should be classified explicitly:

```text
NEAR
FAR
STREAMED FAR
```

rather than relying on accidental section placement.

---

## 23. Runtime rules MegaLDOOM should follow

A practical set of coding rules would be:

### Rule 1: never assume a far pointer is permanent

Treat mapped FAR pointers as short-lived views.

### Rule 2: do not call arbitrary mapper-changing code while using a far pointer

A helper function may itself trigger another FAR access and invalidate the previous mapping.

### Rule 3: copy, decompress or upload while the mapping is controlled

Typical safe flow:

```text
resolve resource
map bank
copy/decompress/DMA
release conceptual ownership
```

### Rule 4: keep interrupt-critical data near

Interrupt handlers should not depend on transient mapper state unless explicitly designed for it.

### Rule 5: centralize custom far loading

Do not spread manual `SYS_setBank()` calls across game code.

Use one MegaLDOOM resource/mapping layer.

### Rule 6: avoid mapper changes in inner rendering loops when possible

Bank switching itself is simple, but excessive switching plus pointer/lifetime complexity can become expensive and error-prone.

Prefetch or group resources by bank where practical.

---

## 24. Suggested target size: 16 MB first, architecture compatible with 32 MB

MegaLDOOM does not need to jump immediately to a fully populated 32 MB image.

A strong staged strategy is:

### Stage A: 4 MB build remains working

Keep a small-ROM configuration for regression testing.

This validates that normal `FAR()` macros still collapse safely to direct pointers when banking is disabled.

### Stage B: enable SSF mapper and build an 8 MB test ROM

Use deliberately placed test assets above 4 MB.

Validate:

- emulator boot;
- far tile loading;
- far sprite loading;
- far map data;
- far music;
- compressed resources crossing a bank boundary;
- repeated bank changes;
- reset behavior.

### Stage C: target 16 MB for normal development

16 MB provides substantial asset headroom while using only half of the mapper's theoretical page range.

### Stage D: allow growth toward 32 MB

The data format and resource directory should use addresses/types wide enough for the full 32 MB design from the beginning.

This avoids another format transition later.

---

## 25. Why 16-32 MB is particularly useful for MegaLDOOM

MegaLDOOM is an unusually good candidate for ROM banking because much of its potentially large content is read-mostly asset data:

- wall textures;
- patches;
- sprite rotations and animation frames;
- weapon frames;
- flats;
- map data;
- music;
- PCM/SFX;
- UI graphics;
- title/intermission art;
- optional content variants.

These resources do not need to be permanently executable or permanently mapped.

A DOOM-like engine naturally performs explicit resource lookup and cache/load operations, which maps well onto a banked-ROM abstraction.

The large-ROM architecture therefore has a favorable division:

```text
68000 resident region
    = engine + active state + resource index

banked physical ROM
    = mostly immutable content

RAM / VRAM
    = currently active working set
```

This is much simpler than trying to bank arbitrary executable code.

---

## 26. Potential MegaLDOOM WAD strategy

A physical PC DOOM WAD should not necessarily be embedded unchanged and accessed as one monolithic file.

For Mega Drive use, a build-time conversion step can preserve WAD semantics while optimizing physical ROM layout.

Recommended concept:

```text
input WAD
   |
   v
MegaLDOOM asset compiler
   |
   +-- parse directory
   +-- convert graphics
   +-- convert palettes
   +-- convert sounds/music
   +-- preprocess maps
   +-- compress selected lumps
   +-- classify near/far/streamed
   +-- assign 512 KB physical banks
   +-- emit resident directory
   +-- emit far payloads
   |
   v
SGDK resources / generated assembly
```

At runtime, MegaLDOOM can still expose a familiar lump-style API even though the cartridge representation has been optimized for banking.

This approach also allows build-time deduplication, alignment and resource locality optimization.

---

## 27. Hardware and emulator compatibility must be tested explicitly

A ROM larger than 4 MB is no longer just a plain linear cartridge image. Correct behavior depends on support for the expected mapper.

Therefore MegaLDOOM should maintain a compatibility test matrix.

At minimum:

```text
emulators
- BlastEm
- Genesis Plus GX
- other project-supported accurate emulators

flash cartridges / hardware
- devices intended to support SSF2-style mapper ROMs
- real Mega Drive / Genesis revisions when available
```

The investigation found historical SGDK reports involving EverDrive behavior, including fixes related to bank-reset behavior. This reinforces the need for real-device testing rather than assuming all emulators and flash carts handle every mapper path identically.

The mapper must be considered part of the cartridge hardware contract.

---

## 28. A note on SRAM and cartridge hardware combinations

Large-ROM mapping, SRAM and flash-cartridge behavior can interact through cartridge address decoding and implementation details.

MegaLDOOM should therefore avoid assuming that any arbitrary large-ROM PCB/flash-cart arrangement will automatically provide the exact same behavior as a normal small ROM with SRAM.

Before finalizing a physical cartridge configuration, verify:

- SSF mapper register support;
- physical ROM size supported by the board;
- SRAM address decoding;
- save-memory coexistence with mapper logic;
- reset behavior;
- compatibility with intended flash carts.

This is especially important if MegaLDOOM eventually targets a reproducible physical cartridge PCB rather than emulator-only distribution.

---

## 29. What would be required to go beyond 32 MB

The current stock mapper implementation cannot uniquely select pages above bank 63.

The limiting code is conceptually:

```c
bankIndex = (addr >> 19) & 0x3F;
```

To exceed 32 MB, MegaLDOOM would need a different mapping scheme.

That would require at least:

1. a cartridge mapper capable of selecting more physical address bits;
2. a defined register protocol;
3. changes to SGDK/MegaLDOOM mapping code;
4. changes to logical far-address handling if needed;
5. build-time support for the larger image;
6. emulator support for the custom mapper;
7. flash-cartridge or physical-PCB support;
8. a new compatibility test matrix.

A hypothetical 64 MB mapper using 512 KB pages would need at least seven page-selection bits instead of six:

```text
128 pages * 512 KB = 64 MB
```

But there is no benefit in pursuing that complexity before MegaLDOOM demonstrates a real need for more than 32 MB.

The stock SGDK design already provides an eightfold increase over a normal 4 MB cartridge.

---

## 30. Recommended implementation plan for MegaLDOOM

### Phase 1 - establish measurements

- Record current final ROM size.
- Record `.text`, `.rodata`, `.rodata_bin`, `.rodata_binf` sizes.
- Record the highest resident code/data address.
- Produce a top-N list of largest assets.

### Phase 2 - enable and validate SGDK mapper support

- Enable `ENABLE_BANK_SWITCH` in the SGDK configuration used by the project.
- Rebuild SGDK.
- Confirm the generated ROM header identifies the SSF configuration.
- Create a controlled asset above the 4 MB physical boundary.
- Verify correct access in an accurate emulator.

### Phase 3 - add build guards

- Add a 32 MB final-image hard limit.
- Add a `0x300000` resident-range hard check.
- Add bank-crossing diagnostics.
- Add per-resource size diagnostics.

### Phase 4 - centralize MegaLDOOM asset access

- Introduce a bank-aware resource directory.
- Replace project-specific raw long-lived ROM pointers with resource handles/descriptors where necessary.
- Keep mapped pointers local to loading functions.

### Phase 5 - migrate the largest assets first

Suggested order:

1. wall textures;
2. sprite graphics;
3. weapon graphics;
4. music;
5. PCM/SFX;
6. map data;
7. optional/title/intermission content.

### Phase 6 - optimize bank locality

- Measure mapper changes per frame / per level load.
- Group frequently co-accessed resources.
- Align selected data groups to 512 KB boundaries where beneficial.
- Reduce unnecessary cross-bank resources.

### Phase 7 - hardware compatibility

- Test on supported flash cartridges.
- Test on real hardware.
- Document known-good devices and emulator versions/configurations.

---

## 31. Suggested invariants for the project

MegaLDOOM should eventually document and enforce rules similar to these:

```text
MEGALDOOM ROM INVARIANTS

1. Stock banked build size <= 0x02000000 bytes.
2. Resident executable code must not occupy 0x300000-0x3FFFFF.
3. Interrupt-critical data must remain resident.
4. FAR pointers are transient unless a subsystem explicitly owns mapper state.
5. Resources larger than the supported contiguous mapper window must use streaming/chunking.
6. Direct manual mapper register writes are restricted to the central mapper/resource module.
7. Build tooling reports every FAR resource's physical bank(s).
8. Both banked and non-banked configurations should remain testable where practical.
```

These rules would make the mapper a controlled architectural feature rather than a collection of ad hoc pointer fixes.

---

## 32. Important distinction: facts vs recommendations

### Directly established from current SGDK source

- SGDK contains code specifically for ROMs larger than 4 MB.
- The mapper uses 512 KB banks.
- The first 512 KB CPU region is fixed.
- Seven other regions are mapper-controlled.
- SGDK exposes `SYS_setBank()`, `SYS_getFarData()`, `SYS_getFarDataSafe()` and related APIs.
- `ENABLE_BANK_SWITCH` controls automatic support.
- The ROM header changes to the SSF identifier when enabled.
- The current bank selector masks to six bits (`0x3F`).
- Six bits * 512 KB yields a 32 MB physical address range.
- ResComp separates far binary data into `.rodata_binf`.
- `BIN` data defaults to far in the current processor implementation.
- Many graphics/map/sound paths use FAR-aware accessors.
- XGM/XGM2 expose FAR-specific functions.
- The linker allows an address space vastly larger than 4 MB.

### Architectural recommendations made specifically for MegaLDOOM

- Reserve `0x300000-0x3FFFFF` exclusively for automatic FAR mapping.
- Keep resident code/data below `0x300000`.
- Target 16 MB initially while keeping formats compatible with 32 MB.
- Build a centralized WAD/lump-to-bank resource layer.
- Add link-time/build-time resident boundary checks.
- Add resource packing and bank locality optimization.
- Avoid banked executable code unless future measurements prove it necessary.

Keeping these categories separate is important. The 32 MB mapper bound is a code-level fact; the 3 MB resident-layout rule is a conservative architecture choice based on how SGDK's automatic FAR helper uses the top 1 MB of the visible ROM aperture.

---

## 33. Final conclusion

MegaLDOOM does **not** need to remain constrained to a 4 MB cartridge.

The SGDK codebase already contains a coherent large-ROM solution built around the Sega SSF mapper.

The useful practical model is:

```text
4 MB
    normal flat ROM

16 MB
    excellent practical MegaLDOOM target using stock SGDK bank switching

32 MB
    maximum uniquely addressable capacity of the current SGDK 6-bit / 512 KB mapper model

>32 MB
    not solved by increasing linker size; requires a different mapper design
```

The cleanest MegaLDOOM architecture is therefore:

```text
resident 68000 engine + critical metadata
                  |
                  v
central bank-aware resource manager
                  |
                  v
up to ~29 MB of additional banked asset space
```

with SGDK's two upper 512 KB windows used as transient views into the physical ROM.

This is a much better fit for MegaLDOOM than attempting to compress the entire game into 4 MB or designing a custom mapper prematurely.

The immediate next engineering step should be to enable the stock SGDK bank-switch configuration in a controlled branch/build, add resident-range and ROM-size assertions, and migrate one representative large asset class to FAR storage as an end-to-end validation.

---

## 34. Upstream source index

Primary SGDK sources inspected during this investigation:

- [`inc/mapper.h`](https://github.com/Stephane-D/SGDK/blob/master/inc/mapper.h) - mapper design, bank size, mapper registers, FAR/FAR_SAFE API documentation
- [`src/mapper.c`](https://github.com/Stephane-D/SGDK/blob/master/src/mapper.c) - runtime bank selection, 6-bit bank mask, automatic FAR windows
- [`inc/config.h`](https://github.com/Stephane-D/SGDK/blob/master/inc/config.h) - `ENABLE_BANK_SWITCH`
- [`md.ld`](https://github.com/Stephane-D/SGDK/blob/master/md.ld) - linker memory region and section ordering
- [`src/boot/rom_header.c`](https://github.com/Stephane-D/SGDK/blob/master/src/boot/rom_header.c) - SSF header identifier and visible ROM end
- [`inc/sys.h`](https://github.com/Stephane-D/SGDK/blob/master/inc/sys.h) - ROM size definitions and system declarations
- [`src/sys.c`](https://github.com/Stephane-D/SGDK/blob/master/src/sys.c) - mapper reset/init and FAR-aware copying paths
- [`makefile.gen`](https://github.com/Stephane-D/SGDK/blob/master/makefile.gen) - output/link/padding pipeline
- [`tools/sizebnd/src/sgdk/sizebnd/Launcher.java`](https://github.com/Stephane-D/SGDK/blob/master/tools/sizebnd/src/sgdk/sizebnd/Launcher.java) - ROM padding/checksum implementation
- [`tools/rescomp/src/sgdk/rescomp/Compiler.java`](https://github.com/Stephane-D/SGDK/blob/master/tools/rescomp/src/sgdk/rescomp/Compiler.java) - near/far section generation
- [`tools/rescomp/src/sgdk/rescomp/processor/BinProcessor.java`](https://github.com/Stephane-D/SGDK/blob/master/tools/rescomp/src/sgdk/rescomp/processor/BinProcessor.java) - `BIN` far flag behavior
- [`src/vdp_tile.c`](https://github.com/Stephane-D/SGDK/blob/master/src/vdp_tile.c) - FAR-safe tileset access
- [`src/vdp_bg.c`](https://github.com/Stephane-D/SGDK/blob/master/src/vdp_bg.c) - FAR-safe background/bitmap access
- [`src/map.c`](https://github.com/Stephane-D/SGDK/blob/master/src/map.c) - FAR-aware map data
- [`src/bmp.c`](https://github.com/Stephane-D/SGDK/blob/master/src/bmp.c) - FAR-aware bitmap data
- [`src/sprite_eng.c`](https://github.com/Stephane-D/SGDK/blob/master/src/sprite_eng.c) - FAR-aware sprite tile data
- [`src/tools.c`](https://github.com/Stephane-D/SGDK/blob/master/src/tools.c) - FAR-safe image/decompression paths
- [`inc/snd/xgm.h`](https://github.com/Stephane-D/SGDK/blob/master/inc/snd/xgm.h) and [`src/snd/xgm.c`](https://github.com/Stephane-D/SGDK/blob/master/src/snd/xgm.c) - XGM FAR API
- [`inc/snd/xgm2.h`](https://github.com/Stephane-D/SGDK/blob/master/inc/snd/xgm2.h) and [`src/snd/xgm2.c`](https://github.com/Stephane-D/SGDK/blob/master/src/snd/xgm2.c) - XGM2 FAR API
- [`changelog.txt`](https://github.com/Stephane-D/SGDK/blob/master/changelog.txt) - historical large-ROM and FAR-support changes
- [SGDK issue #305 - Question about bank switching](https://github.com/Stephane-D/SGDK/issues/305) - maintainer confirmation of automatic banking and the separate 8 MB hardware option

### Core equations

```text
BANK_SIZE = 0x80000 = 512 KB

bankIndex = (address >> 19) & 0x3F

number of unique banks = 0x40 = 64

64 * 512 KB = 32 MB
```

That is the central technical reason the stock SGDK mapper can go far beyond 4 MB while still having a concrete 32 MB upper bound.
