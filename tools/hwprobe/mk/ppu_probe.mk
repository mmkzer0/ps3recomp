#---------------------------------------------------------------------------------
# Shared build rules of the hwprobe probes (PSL1GHT ppu_rules, as the PSL1GHT samples do).
#
# A layer Makefile sets, then includes this file:
#   TARGET     output name (ring110_l1 -> ring110_l1.elf, .self, .fake.self)
#   SOURCES    source directories relative to the Makefile (the shared library is added here)
#   INCLUDES   extra include directories relative to the Makefile
#   LIBS       extra libraries (the shared library's are added here)
# build.sh passes HWP_GIT_DESCRIBE and HWP_SOURCE_SHA256 on the make command line; they end
# up in the result header (common/hwprobe_result.c). Missing = "unknown" there.
#
# Build: PS3DEV=... PSL1GHT=... make        (.elf, .self, .fake.self)
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/ppu_rules

# tools/hwprobe, from the including Makefile (probes/<probe>/<layer>/Makefile). The first
# entry of MAKEFILE_LIST is the layer Makefile, both in the outer run and in the
# recursive run inside build/ (which is started with an absolute -f).
HWP_ROOT	:=	$(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../../..)
HWP_COMMON	:=	$(HWP_ROOT)/common

BUILD		:=	build
SOURCES		:=	$(SOURCES) $(HWP_COMMON)
INCLUDES	:=	$(INCLUDES) $(HWP_COMMON)

HWP_GIT_DESCRIBE	?=	unknown
HWP_SOURCE_SHA256	?=	unknown

CFLAGS		=	-O2 -Wall -std=gnu99 -mcpu=cell $(MACHDEP) $(INCLUDE) \
				-DHWP_GIT_DESCRIBE=\"$(HWP_GIT_DESCRIBE)\" -DHWP_SOURCE_SHA256=\"$(HWP_SOURCE_SHA256)\"
CXXFLAGS	=	$(CFLAGS)

LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

# order matters: rsx needs gcm_sys
LIBS		:=	$(LIBS) -lsysutil -laudio -lrsx -lgcm_sys -lio -lrt -llv2 -lm

LIBDIRS		:=

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)

# SOURCES entries are relative to the Makefile, except the absolute shared library
export VPATH	:=	$(foreach dir,$(SOURCES),$(if $(filter /%,$(dir)),$(dir),$(CURDIR)/$(dir)))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

export BUILDDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(if $(filter /%,$(dir)),$(dir),$(CURDIR)/$(dir))/*.c)))

export LD	:=	$(CC)

export OFILES	:=	$(CFILES:.c=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(if $(filter /%,$(dir)),$(dir),$(CURDIR)/$(dir))) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).fake.self

#---------------------------------------------------------------------------------
else

DEPENDS	:= $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
