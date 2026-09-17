ifndef GDK
$(error GDK is not set. Point it to your SGDK folder, for example: set GDK=C:\\sgdk)
endif

# makefile.gen's INCS is -I$(SRC) only (not recursive), so headers that moved
# into src/<group>/ need their own -I to stay reachable by bare #include from
# other groups.
EXTRA_FLAGS := -Isrc/billboard -Isrc/renderer -Isrc/bsp

include $(GDK)/makefile.gen

# Banked cartridge (Sega SSF mapper): link with tools/md_banked.ld instead of
# SGDK's flat md.ld so each level's wall pack lands in its own physical banks
# behind the shared 0x280000 level window. Same command as makefile.gen's
# rule otherwise; make uses this later recipe.
$(OUT)/rom.out: $(OUT)/sega.o $(OUT)/cmd_ $(LIBMD) tools/md_banked.ld
	$(MKDIR) -p $(dir $@)
	$(CC) -m68000 -B$(BIN) -n -T tools/md_banked.ld -nostdlib $(OUT)/sega.o @$(OUT)/cmd_ $(LIBMD) $(LIBGCC) -o $(OUT)/rom.out -Wl,--gc-sections -flto -flto=auto -ffat-lto-objects
	$(RM) $(OUT)/cmd_

# .incbin payloads are invisible to -MMD.
$(OUT)/src/bsp/generated_wall_packs.o: $(wildcard src/bsp/generated_wallpack_*.dat)
$(OUT)/src/bsp/generated_bsp_vis_programs.o: $(wildcard src/bsp/generated_bsp_vis_*.dat)
