#---------------------------------------------------------------------------------
# Shared build rules of the hwprobe probes (PSL1GHT ppu_rules, as the PSL1GHT samples do).
#
# A layer Makefile sets, then includes this file:
#   TARGET     output name (ring110_l1 -> ring110_l1.elf, .self, .fake.self)
#   SOURCES    source directories relative to the Makefile (the shared library is added here)
#   INCLUDES   extra include directories relative to the Makefile
#   LIBS       extra libraries (the shared library's are added here)
# Optional SPU program (a probe with SPU code): SPU_DIR (relative to the layer Makefile) holds an spu_rules Makefile
# whose output is SPU_NAME.elf; it is built first and embedded in the SELF as SPU_NAME_spu.bin, i.e. the symbols
# SPU_NAME_spu_bin / SPU_NAME_spu_bin_size declared in SPU_NAME_spu_bin.h (PSL1GHT bin2o convention).
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

export OFILES	:=	$(CFILES:.c=.o) $(if $(SPU_NAME),$(SPU_NAME)_spu.bin.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(if $(filter /%,$(dir)),$(dir),$(CURDIR)/$(dir))) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean

$(BUILD): $(if $(SPU_DIR),spu-program)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

ifneq ($(SPU_DIR),)
.PHONY: spu-program
spu-program:
	@$(MAKE) --no-print-directory -C $(SPU_DIR)
	@[ -d $(BUILD) ] || mkdir -p $(BUILD)
	@cp -f $(SPU_DIR)/$(SPU_NAME).elf $(BUILD)/$(SPU_NAME)_spu.bin
	@echo "extern const u8 $(SPU_NAME)_spu_bin_end[];" > $(BUILD)/$(SPU_NAME)_spu_bin.h
	@echo "extern const u8 $(SPU_NAME)_spu_bin[];" >> $(BUILD)/$(SPU_NAME)_spu_bin.h
	@echo "extern const u32 $(SPU_NAME)_spu_bin_size;" >> $(BUILD)/$(SPU_NAME)_spu_bin.h
endif

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).fake.self
	@$(if $(SPU_DIR),$(MAKE) --no-print-directory -C $(SPU_DIR) clean)

#---------------------------------------------------------------------------------
else

DEPENDS	:= $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

# the header with the symbol declarations is written by the spu-program step above (before any C file is compiled)
%.bin.o: %.bin
	@echo $(notdir $<)
	@bin2s -a 64 $< | $(AS) -o $@

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
