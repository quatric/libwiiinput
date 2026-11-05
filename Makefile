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
WITH_HIDPADS   ?= 0
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
# The Wii Speak's boot firmware (Nintendo's, never in the repository), built in
# when given: WS_BuiltinFirmware() then returns it.  PMIC_FIRM=none leaves it out.
 PMIC_FIRM ?= $(wildcard $(CURDIR)/data/pmic_firm.bin)
 ifneq ($(filter-out none,$(strip $(PMIC_FIRM))),)
  ifeq ($(wildcard $(PMIC_FIRM)),)
   $(error PMIC_FIRM=$(PMIC_FIRM) does not exist)
  endif
  FIRM_FILE := $(abspath $(PMIC_FIRM))
  CFLAGS += -DWS_FIRMWARE_FILE='"$(FIRM_FILE)"'
 endif
endif
# The GameCube controller adapter for Wii U
ifeq ($(WITH_GCADAPTER),1)
 CFLAGS += -DHAVE_GCADAPTER
endif
# USB game controllers with Nintendont's controller.ini layouts
ifeq ($(WITH_HIDPADS),1)
 CFLAGS += -DHAVE_HIDPADS
endif
# Room for a fourth Wii Remote and the Balance Board (links with --wrap=wiiuse_init_cmd_queue)
ifeq ($(WITH_WPADQUEUE),1)
 CFLAGS += -DHAVE_WPADQUEUE
endif

SRCS    := $(wildcard source/*.c)
# one folder of objects for each set of options, so switching them rebuilds
BUILD   := build/$(WITH_WIIDRC)$(WITH_WUPC)$(WITH_SICKSAXIS)$(WITH_TABLET)$(WITH_MIC)$(WITH_GCADAPTER)$(WITH_WPADQUEUE)$(WITH_HIDPADS)$(if $(FIRM_FILE),f)
OBJS    := $(patsubst source/%.c,$(BUILD)/%.o,$(SRCS))
LIB     := lib/libinput-wii.a

all: $(LIB)

# made again every time: it may hold another set of options' objects
$(LIB): $(OBJS) FORCE
	@mkdir -p lib
	@rm -f $@
	$(AR) rcs $@ $(OBJS)

$(BUILD)/%.o: source/%.c source/libinput_internal.h source/libinput_hidparse.h source/libinput_gcad.h source/libinput_hid.h include/input_wii.h include/input_wiispeak.h include/input_usbmic.h include/input_tablet.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# rebuilt when the firmware file changes
$(BUILD)/libinput_wiispeak_firm.o: $(FIRM_FILE)

install: $(LIB)
	@mkdir -p $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/lib $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/include
	cp $(LIB) $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/lib/
	cp include/input_wii.h include/input_wiispeak.h include/input_usbmic.h include/input_tablet.h $(DESTDIR)$(DEVKITPRO)/portlibs/ppc/include/

clean:
	rm -rf build lib

# Extracts the Wii Speak firmware from an RVL SDK pmic.a into data/pmic_firm.bin,
# which the library then builds in (WITH_MIC=1).  The file is Nintendo's and
# ignored by git; apps that use this library take it from here.
SDK ?=
firmware:
	@if [ -z "$(SDK)" ]; then echo "make firmware SDK=/path/to/RVL_SDK/RVL/lib/pmic.a"; exit 1; fi
	@mkdir -p data
	python3 tools/extract_pmic_firmware.py "$(SDK)" -o data/pmic_firm.bin

# host test of the controller.ini reader (no devkitPro needed)
test:
	cc -DHAVE_HIDPADS -Wall -Wextra -fsanitize=address,undefined -Isource tests/hid_test.c source/libinput_hid.c source/libinput_hidpads_data.c -o build/hid_test
	build/hid_test

controllers:
	./scripts/build_controller_portlibs.sh

.PHONY: all install clean controllers test firmware FORCE
