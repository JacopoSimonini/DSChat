# mictest ARM7 core: a trimmed copy of the BlocksDS sdk v1.25.0
# sys/arm7/main_core/Makefile.generic, fixed to the dswifi variant.
# Output: arm7_mictest.elf (used by ../Makefile as ARM7ELF).

BLOCKSDS		?= /opt/blocksds/core
WONDERFUL_TOOLCHAIN	?= /opt/wonderful
PREFIX		:= $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-arm-none-eabi/bin/arm-none-eabi-
CC		:= $(PREFIX)gcc

ELF		:= arm7_mictest.elf
BUILDDIR	:= build
LIBS		:= -lnds7 -ldswifi7 -lc
LIBDIRS		:= $(BLOCKSDS)/libs/libnds $(BLOCKSDS)/libs/dswifi
SPECS		:= $(BLOCKSDS)/sys/crts/ds_arm7.specs
ARCH		:= -mthumb -mcpu=arm7tdmi
DEFINES		:= -DPICOLIBC_LONG_LONG_PRINTF_SCANF -DUSE_DSWIFI=1

INCLUDEFLAGS	:= $(foreach p,$(LIBDIRS),-I$(p)/include)
LIBDIRSFLAGS	:= $(foreach p,$(LIBDIRS),-L$(p)/lib)
CFLAGS		:= -std=gnu23 -Wall -Wextra -Wpedantic $(INCLUDEFLAGS) $(DEFINES) \
		   $(ARCH) -O2 -ffunction-sections -fdata-sections -specs=$(SPECS)
LDFLAGS		:= $(ARCH) $(LIBDIRSFLAGS) -Wl,-Map,$(BUILDDIR)/mictest.map \
		   -Wl,--start-group $(LIBS) -Wl,--end-group -specs=$(SPECS)

SOURCES		:= $(wildcard source/*.c)
OBJS		:= $(patsubst source/%.c,$(BUILDDIR)/%.o,$(SOURCES))

.PHONY: all clean
all: $(ELF)

$(ELF): $(OBJS)
	@echo "  LD7     $@"
	@$(CC) -o $@ $(OBJS) $(LDFLAGS)

$(BUILDDIR)/%.o: source/%.c
	@echo "  CC7     $<"
	@mkdir -p $(BUILDDIR)
	@$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR) $(ELF)
