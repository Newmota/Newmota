# This file, recorded before any include adds to MAKEFILE_LIST; the sub-makes of all reread it.
NEWMOTA_RAM_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))

include $(MAKE_INCLUDE_GEN)

.PHONY: all clean

DIR = $(SRCDIR)

MODULE_IFLAGS = -I$(DIR) -I$(DIR)/include
GLOBAL_CFLAGS += $(CPPFLAGS_EXTRA)
CPPFLAGS += -std=gnu++17 -fno-exceptions -fno-rtti

# The C rule reads GLOBAL_CFLAGS; filter its C-only -Wstrict-prototypes flag from C++ CPPFLAGS.
CPPFLAGS := $(filter-out -Wstrict-prototypes,$(CPPFLAGS))

ifeq ($(NEWMOTA_USE_BERRY),1)
MODULE_IFLAGS += -I$(DIR)/include/berry/src -I$(DIR)/include/berry/generated
endif

include $(DIR)/newmota_sources.mk

# Objects are flattened into this directory, so let make find each selected source
# in the directory recorded by the generated source list.
vpath %.c $(sort $(dir $(CSRC)))
vpath %.cpp $(sort $(dir $(CPPSRC)))

OBJS = $(notdir $(CSRC:.c=.o)) $(notdir $(CPPSRC:.cpp=.o))

# Every build recompiles these objects (CPPFLAGS_EXTRA carries the build time; ccache keeps it
# cheap). The clean runs in its own make first: as a prerequisite of all, a parallel make
# (JOBS) would judge some objects up to date, delete them, and then fail to copy them.
all:
	+$(MAKE) -f $(NEWMOTA_RAM_MAKEFILE) CLEAN_OBJS
	+$(MAKE) -f $(NEWMOTA_RAM_MAKEFILE) COPY_RAM_OBJS

CORE_TARGETS: $(OBJS)

clean: CLEAN_OBJS
	$(REMOVE) *.o
	$(REMOVE) *.i
	$(REMOVE) *.s
	$(REMOVE) *.d
	$(REMOVE) *.su

-include $(DEPS)
