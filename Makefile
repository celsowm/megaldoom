ifndef GDK
$(error GDK is not set. Point it to your SGDK folder, for example: set GDK=C:\\sgdk)
endif

# makefile.gen's INCS is -I$(SRC) only (not recursive), so headers that moved
# into src/<group>/ need their own -I to stay reachable by bare #include from
# other groups.
EXTRA_FLAGS := -Isrc/billboard -Isrc/renderer -Isrc/bsp

include $(GDK)/makefile.gen
