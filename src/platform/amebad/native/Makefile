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

all: CLEAN_OBJS CORE_TARGETS COPY_RAM_OBJS

CORE_TARGETS: $(OBJS)

clean: CLEAN_OBJS
	$(REMOVE) *.o
	$(REMOVE) *.i
	$(REMOVE) *.s
	$(REMOVE) *.d
	$(REMOVE) *.su

-include $(DEPS)
