ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

PREFIX  := $(DEVKITPPC)/bin/powerpc-eabi-
CC      := $(PREFIX)gcc
AR      := $(PREFIX)ar
LIBOGC  ?= $(DEVKITPRO)/libogc

CFLAGS  := -O2 -Wall -Wextra -DGEKKO -DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float \
           -Iinclude -I$(LIBOGC)/include
WITH_WIIDRC    ?= 0
WITH_WUPC      ?= 0
WITH_SICKSAXIS ?= 0
WITH_TABLET    ?= 0
WITH_MIC       ?= 0
WITH_GCADAPTER ?= 0
WITH_WPADQUEUE ?= 0
PORTLIBS       ?= $(CURDIR)/portlibs/ppc
CFLAGS += -I$(PORTLIBS)/include
ifeq ($(WITH_WIIDRC),1)
 CFLAGS += -DHAVE_WIIDRC
endif
ifeq ($(WITH_WUPC),1)
 CFLAGS += -DHAVE_WUPC
endif
ifeq ($(WITH_SICKSAXIS),1)
 CFLAGS += -DHAVE_SICKSAXIS
endif
# USB pen tablets and the uDraw GameTablet (input_tablet.h)
ifeq ($(WITH_TABLET),1)
 CFLAGS += -DHAVE_TABLET
endif
# Wii Speak and USB microphones (input_wiispeak.h, input_usbmic.h)
ifeq ($(WITH_MIC),1)
 CFLAGS += -DHAVE_MIC
endif
# The GameCube controller adapter for Wii U
ifeq ($(WITH_GCADAPTER),1)
 CFLAGS += -DHAVE_GCADAPTER
endif
# Room for a fourth Wii Remote and the Balance Board (links with --wrap=wiiuse_init_cmd_queue)
ifeq ($(WITH_WPADQUEUE),1)
 CFLAGS += -DHAVE_WPADQUEUE
endif

SRCS    := $(wildcard source/*.c)
# one folder of objects for each set of options, so switching them rebuilds
BUILD   := build/$(WITH_WIIDRC)$(WITH_WUPC)$(WITH_SICKSAXIS)$(WITH_TABLET)$(WITH_MIC)$(WITH_GCADAPTER)$(WITH_WPADQUEUE)
OBJS    := $(patsubst source/%.c,$(BUILD)/%.o,$(SRCS))
LIB     := lib/libinput-wii.a

all: $(LIB)

# made again every time: it may hold another set of options' objects
$(LIB): $(OBJS) FORCE
	@mkdir -p lib
	@rm -f $@
	$(AR) rcs $@ $(OBJS)

$(BUILD)/%.o: source/%.c source/libinput_internal.h source/libinput_hidparse.h source/libinput_gcad.h include/input_wii.h include/input_wiispeak.h include/input_usbmic.h include/input_tablet.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

install: $(LIB)
	@mkdir -p $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/lib $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/include
	cp $(LIB) $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/lib/
	cp include/input_wii.h include/input_wiispeak.h include/input_usbmic.h include/input_tablet.h $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/include/

clean:
	rm -rf build lib

controllers:
	./scripts/build_controller_portlibs.sh

.PHONY: all install clean controllers FORCE
