#---------------------------------------------------------------------------------
# spuexh SPU program (spu-gcc). Built by layer1's Makefile (target `bin`); the result, spuexh.elf, is
# embedded into the PPU SELF. Inputs: source/, ../common (shared header), ../generated (prebuild.sh).
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/spu_rules

TARGET		:=	spuexh
BUILD		:=	build
SOURCES		:=	source ../generated
DATA		:=
INCLUDES	:=	../common ../generated

# -O2, no -ffast-math, no vectorizer flags of the PSL1GHT defaults: the op words are executed as raw instructions and
# the generator is plain integer code, so nothing in the flags can change a result
CFLAGS		=	-O2 -Wall -Wno-unused-function -std=gnu11 $(MACHDEP) $(INCLUDE)
CXXFLAGS	=	$(CFLAGS)
ASFLAGS		=	$(MACHDEP) $(INCLUDE)
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS		:=	-lsputhread

LIBDIRS		:=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export BUILDDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))

export LD	:=	$(CC)
export OFILES	:=	$(CFILES:.c=.o) $(SFILES:.S=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile check-layout

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf

else

DEPENDS	:= $(OFILES:.o=.d)

$(OUTPUT).elf: $(OFILES)

# LS layout guard (see common/spuexh.h). The raw-exec arenas sit at fixed LS addresses and are not reserved by the
# linker; the PSL1GHT SPU script (elf32_spu.x) puts the stack at __stack = 0x3FFF0 and it grows down. The arena-B vs
# stack side is a _Static_assert in spuexh.h; the loaded-image side depends on the link result (_end), so it is
# checked here: the build fails loudly when the image grows into arena A (0x1E000) instead of overwriting it at run
# time. If that happens, move the arenas in spuexh.h and keep SPUEXH_ARENA_* in sync here and in spu_main.c.
SPUEXH_ARENA_A_HEX	:=	0001E000
check-layout:
	@end="$$($(PREFIX)nm $(OUTPUT).elf | awk '$$3 == "_end" { print $$1; exit }' | tr a-f A-F)" ; \
	if [ -z "$$end" ] ; then \
		echo "spuexh: LS layout guard: no _end symbol in $(OUTPUT).elf" ; exit 1 ; \
	fi ; \
	if [ "$$end" \> "$(SPUEXH_ARENA_A_HEX)" ] ; then \
		echo "spuexh: LS layout guard: loaded image ends at 0x$$end, reaching arena A (0x1E000); move the arenas (spuexh.h) or shrink the image" ; exit 1 ; \
	fi ; \
	echo "spuexh: LS layout ok: image end 0x$$end < arena A 0x1E000 < arena B (0x3C000..0x3D800) <= stack floor 0x3DFF0 (stack top 0x3FFF0, 8 KiB reserve)"

-include $(DEPENDS)

endif
