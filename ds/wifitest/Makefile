# DSChat Wi-Fi test ROM (Phase 1). Builds with the BlocksDS default ARM9 makefile.
# Usually invoked through ../Makefile (Docker); works natively if BLOCKSDS is set.
#
# Build-time config (written to a generated header, changes trigger a rebuild):
#   SSID  open AP to join             (default DSCHAT; melonDS: melonAP)
#   PORT  relay TCP port              (default 4242)
#   HOST  relay IPv4 address          (default empty = the DHCP gateway)
#   MSG   hardcoded chat message
#   STATIC_IP    fixed IPv4 for the DS (default empty = DHCP), e.g. 192.168.77.60;
#   STATIC_GW    its gateway (default 192.168.77.1, also used as DNS server)
#   STATIC_MASK  its netmask (default 255.255.255.0)

BLOCKSDS	?= /opt/blocksds/core

SSID		?= DSCHAT
PORT		?= 4242
HOST		?=
MSG		?= Hello from a Nintendo DS Lite! In two short sentences, what are you?
STATIC_IP	?=
STATIC_GW	?= 192.168.77.1
STATIC_MASK	?= 255.255.255.0

NAME		:= dschat_wifitest
ifneq ($(SSID),DSCHAT)
NAME		:= $(NAME)_$(SSID)
endif
ifneq ($(STATIC_IP),)
NAME		:= $(NAME)_static
endif
ifeq ($(DEBUG),1)
NAME		:= $(NAME)_debug
endif

GAME_TITLE	:= DSChat Wi-Fi test
GAME_SUBTITLE	:= Diagnostic ROM (Phase 1)
GAME_AUTHOR	:= DSChat 1.1.0
GAME_ICON	:= ../dschat/icon.png

SOURCEDIRS	:= source
CFGDIR		:= build/cfg_$(NAME)
INCLUDEDIRS	:= $(CFGDIR)

ifeq ($(DEBUG),1)
    DEFINES	:= -DDSWIFI_LOGS
    ARM7ELF	:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi_debug.elf
    LIBS	:= -ldswifi9d -lnds9d
else
    ARM7ELF	:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf
    LIBS	:= -ldswifi9 -lnds9
endif

LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
		   $(BLOCKSDS)/libs/libnds

# Generated config header, rewritten only when the values change.
CFG_CONTENT := \#pragma once\n\#define CFG_SSID "$(SSID)"\n\#define CFG_PORT $(PORT)\n\#define CFG_HOST "$(HOST)"\n\#define CFG_MSG "$(MSG)"\n\#define CFG_STATIC_IP "$(STATIC_IP)"\n\#define CFG_STATIC_GW "$(STATIC_GW)"\n\#define CFG_STATIC_MASK "$(STATIC_MASK)"\n
$(shell mkdir -p $(CFGDIR) && printf '$(CFG_CONTENT)' > $(CFGDIR)/cfg.tmp && \
	(cmp -s $(CFGDIR)/cfg.tmp $(CFGDIR)/dschat_cfg.h || cp $(CFGDIR)/cfg.tmp $(CFGDIR)/dschat_cfg.h); \
	rm -f $(CFGDIR)/cfg.tmp)

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
