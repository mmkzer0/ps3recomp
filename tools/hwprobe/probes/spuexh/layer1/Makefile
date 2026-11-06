#---------------------------------------------------------------------------------
# spuexh Layer 1: the exhaustive SPU instruction sweep on a console. Built by
#   tools/hwprobe/build.sh spuexh layer1
# (which first runs probes/spuexh/prebuild.sh on the host). The SPU program in ../spu is built first and embedded
# in the SELF; ../common is shared with the host dry run (host/).
#---------------------------------------------------------------------------------
TARGET		:=	spuexh_l1
SOURCES		:=	source ../common
INCLUDES	:=	../common ../generated
LIBS		:=
SPU_DIR		:=	../spu
SPU_NAME	:=	spuexh

include $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../mk/ppu_probe.mk)
