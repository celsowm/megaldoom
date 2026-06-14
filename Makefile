ifndef GDK
$(error GDK is not set. Point it to your SGDK folder, for example: set GDK=C:\\sgdk)
endif

include $(GDK)/makefile.gen
