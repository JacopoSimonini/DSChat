# mictest: DS Lite microphone and upload diagnostic ROM.
# Built through ../Makefile (Docker): make APPS=mictest
#   SSID  default network if dschat/settings.txt is missing (melonDS: melonAP)
#
# shared/ = symlinks to the dschat modules it reuses unchanged (net, config,
# storage, gfx, proto, textbuf). arm7/ = the default ARM7 core plus a
# microphone-gain FIFO handler (the stock core always records at x160).

BLOCKSDS	?= /opt/blocksds/core

VERSION		:= 0.1.0
SSID		?= DSCHAT

NAME		:= mictest
ifneq ($(SSID),DSCHAT)
NAME		:= $(NAME)_$(SSID)
endif

GAME_TITLE	:= DSChat mictest
GAME_SUBTITLE	:= Microphone experiment
GAME_AUTHOR	:= Version $(VERSION)
GAME_ICON	:= ../dschat/icon.png

SOURCEDIRS	:= source shared
CFGDIR		:= build/cfg_$(NAME)
INCLUDEDIRS	:= $(CFGDIR) shared

ARM7ELF		:= arm7/arm7_mictest.elf
LIBS		:= -ldswifi9 -lnds9
LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
		   $(BLOCKSDS)/libs/libnds

# Same generated header as dschat (config.c needs it).
CFG_CONTENT := \#pragma once\n\#define DSCHAT_VERSION "mictest $(VERSION)"\n\#define CFG_SSID "$(SSID)"\n
$(shell mkdir -p $(CFGDIR) && printf '$(CFG_CONTENT)' > $(CFGDIR)/cfg.tmp && \
	(cmp -s $(CFGDIR)/cfg.tmp $(CFGDIR)/dschat_build.h || cp $(CFGDIR)/cfg.tmp $(CFGDIR)/dschat_build.h); \
	rm -f $(CFGDIR)/cfg.tmp)

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile

# Our ARM7 core is built first.
$(ROM): $(ARM7ELF)
$(ARM7ELF): arm7/source/main.c arm7/Makefile
	$(MAKE) -C arm7 BLOCKSDS=$(BLOCKSDS)

clean: clean_arm7
.PHONY: clean_arm7
clean_arm7:
	$(MAKE) -C arm7 clean
