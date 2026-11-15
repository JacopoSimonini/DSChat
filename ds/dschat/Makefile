# DSChat app. Builds with the BlocksDS default ARM9 makefile.
# Usually invoked through ../Makefile (Docker); works natively if BLOCKSDS is set.
#
# All network settings are chosen on the DS (settings screen, saved to
# dschat/settings.txt on the SD card). The only build-time value is the
# default SSID used when there is no settings file:
#   SSID   default network (DSCHAT; `make melon` builds the melonDS ROM, melonAP)

BLOCKSDS	?= /opt/blocksds/core

VERSION		:= 1.1.0
SSID		?= DSCHAT

NAME		:= dschat
ifneq ($(SSID),DSCHAT)
NAME		:= $(NAME)_$(SSID)
endif

# ROM banner (flashcard menu): icon + three lines. icon.png is drawn by
# tools/make_icon.py.
GAME_TITLE	:= DSChat
GAME_SUBTITLE	:= Chat with an AI from your DS
GAME_AUTHOR	:= Version $(VERSION)
GAME_ICON	:= icon.png

SOURCEDIRS	:= source
CFGDIR		:= build/cfg_$(NAME)
INCLUDEDIRS	:= $(CFGDIR)

ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf
LIBS		:= -ldswifi9 -lnds9
LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
		   $(BLOCKSDS)/libs/libnds

# Generated header, rewritten only when the values change.
CFG_CONTENT := \#pragma once\n\#define DSCHAT_VERSION "$(VERSION)"\n\#define CFG_SSID "$(SSID)"\n
$(shell mkdir -p $(CFGDIR) && printf '$(CFG_CONTENT)' > $(CFGDIR)/cfg.tmp && \
	(cmp -s $(CFGDIR)/cfg.tmp $(CFGDIR)/dschat_build.h || cp $(CFGDIR)/cfg.tmp $(CFGDIR)/dschat_build.h); \
	rm -f $(CFGDIR)/cfg.tmp)

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
