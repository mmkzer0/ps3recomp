#---------------------------------------------------------------------------------
# lv2conf Layer 1 probe: every case of ../cases on a console. Built by
#   tools/hwprobe/build.sh lv2conf layer1
# The cases are the shared sources of the host backend; conf.h (one directory up) is their only
# header, and source/conf_ppu.c is the PPU backend behind it.
#---------------------------------------------------------------------------------
TARGET		:=	lv2conf_l1
SOURCES		:=	source ../cases
INCLUDES	:=	.. ../cases
LIBS		:=

include $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../mk/ppu_probe.mk)
