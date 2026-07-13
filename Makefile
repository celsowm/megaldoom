ifndef GDK
$(error GDK is not set. Point it to your SGDK folder, for example: set GDK=C:\\sgdk)
endif

include $(GDK)/makefile.gen

# Only main.c is redirected. keyed_runtime.c still sees and calls the original
# functions, so coloured/reusable key policy can wrap the existing map and
# billboard implementations without duplicating their mutable state.
$(OUT_DIR)/src/main.o: CFLAGS += \
	-Dbillboard_init=keyed_billboard_init \
	-Dbillboard_collect_near=keyed_billboard_collect_near \
	-Dbsp_use_in_front=keyed_bsp_use_in_front
