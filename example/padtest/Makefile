# Controller test for libwiiinput.  `make` builds boot.dol; `make run` sends
# it to a Wii running the Homebrew Channel (WIILOAD=tcp:<wii-ip>).
#
# The GameCube adapter for Wii U, USB game controllers and a fourth Wii
# Remote are built in (no extra library). `make CONTROLLERS=1` adds the
# Wii U GamePad (vWii), Wii U Pro Controller and DualShock 3, after
# `make controllers` in the library's folder has built their libraries.

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

PREFIX  := $(DEVKITPPC)/bin/powerpc-eabi-
CC      := $(PREFIX)gcc
LIBOGC  ?= $(DEVKITPRO)/libogc
LIB     := ../..
CONTROLLERS ?= 0

CFLAGS  := -O2 -Wall -Wextra -DGEKKO -DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float \
           -DHAVE_GCADAPTER -DHAVE_HIDPADS -DHAVE_WPADQUEUE \
           -I$(LIB)/include -I$(LIB)/source -I$(LIBOGC)/include
LDFLAGS := -mrvl -mcpu=750 -meabi -mhard-float -L$(LIBOGC)/lib/wii \
           -Wl,--wrap=wiiuse_init_cmd_queue
LIBS    := -lfat -lwiikeyboard -lwiiuse -lbte -logc -lm

ifeq ($(CONTROLLERS),1)
 CFLAGS  += -DHAVE_WIIDRC -DHAVE_WUPC -DHAVE_SICKSAXIS -I$(LIB)/portlibs/ppc/include
 LDFLAGS += -L$(LIB)/portlibs/ppc/lib -Wl,-wrap,wiiuse_accept
 LIBS    := -lfat -lwiikeyboard -lwiidrc -lwupc -lsicksaxis -lwiiuse -lbte -logc -lm
endif

SRCS    := main.c $(wildcard $(LIB)/source/*.c)
BUILD   := build/$(CONTROLLERS)
OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRCS)))

vpath %.c . $(LIB)/source

all: boot.dol

boot.dol: boot.elf
	$(DEVKITPRO)/tools/bin/elf2dol $< $@

boot.elf: $(OBJS) FORCE
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(BUILD)/%.o: %.c $(wildcard $(LIB)/include/*.h $(LIB)/source/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

run: boot.dol
	$(DEVKITPRO)/tools/bin/wiiload $<

clean:
	rm -rf build boot.elf boot.dol

.PHONY: all run clean FORCE
