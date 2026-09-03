# SPDX-License-Identifier: CC0-1.0

BLOCKSDS	?= /opt/blocksds/core

# User config

NAME		:= savesync

ifeq ($(DEBUG),1)
NAME	:= $(NAME)_debug
endif

GAME_TITLE	:= DSi Save Sync
GAME_SUBTITLE	:= NiFi save sync tool

# Libraries
# ---------
#
# This app needs DSWiFi (for local multiplayer / NiFi) on top of the normal
# libnds + libfat (SD card access) that every BlocksDS ROM gets by default.

ifeq ($(DEBUG),1)
    DEFINES	:= -DDSWIFI_LOGS
    ARM7ELF	:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi_debug.elf
    LIBS	:= -ldswifi9d_noip -lnds9d
else
    ARM7ELF	:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf
    LIBS	:= -ldswifi9_noip -lnds9
endif

LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
		   $(BLOCKSDS)/libs/libnds

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
