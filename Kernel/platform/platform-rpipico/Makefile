# Set one of these depending.
# Prefer an already-installed SDK when PICO_SDK_PATH is provided, otherwise fetch.
ifeq ($(origin PICO_SDK_PATH), undefined)
export PICO_SDK_FETCH_FROM_GIT = yes
endif
#export PICO_SDK_PATH = ${HOME}/.pico-sdk/sdk/1.5.1

# Set 'SUBTARGET' to one of pico-sdk compatible boards
# valid values: pico or pico2
ifndef SUBTARGET
SUBTARGET=pico # set default subtarget
endif

# Optional: disable PicoCalc PSRAM support (ramdisk + framebuffer).
# Example: make SUBTARGET=pico2 ENABLE_PSRAM=0 build_pico2_psram0/fuzix.elf
ifndef ENABLE_PSRAM
ENABLE_PSRAM=1
endif

# Set to 1 to build without requiring picotool (disables UF2 output/signing).
ifndef PICO_NO_PICOTOOL
PICO_NO_PICOTOOL=0
endif

# CMake caches the selected Pico platform (rp2040 vs rp2350). Re-using the same
# build directory across different SUBTARGETs causes configuration errors. Use a
# per-config build directory to avoid cache conflicts.
BUILDDIR = build_$(SUBTARGET)_psram$(ENABLE_PSRAM)

ifeq ($(SUBTARGET),pico)
TOTALMEM=140
endif
ifeq ($(SUBTARGET),pico_w)
TOTALMEM=140
endif
ifeq ($(SUBTARGET),pico2)
TOTALMEM=320
endif
ifeq ($(SUBTARGET),pico2_w)
TOTALMEM=320
endif

PICOTOOLFLAGS= --family rp2040
ifeq (${SUBTARGET}, pico2)
PICOTOOLFLAGS= --family data
endif

PICOTOOL=picotool

ifeq (, $(shell which picotool))
PICOTOOL=$(BUILDDIR)/_deps/picotool/picotool
endif

include ../../../version.mk

PLATFORM_DEPS = $(wildcard *.c *.h *.S *.pio *.cmake)

$(BUILDDIR)/fuzix.elf: ../../version.c $(BUILDDIR)/Makefile $(PLATFORM_DEPS)
	$(MAKE) -C $(BUILDDIR)

../../version.c:
	$(MAKE) -C ../.. TARGET=rpipico version.c

$(BUILDDIR)/Makefile: CMakeLists.txt $(wildcard ../../*.[chS]) $(wildcard ../../*/*.[chS])
	mkdir -p $(BUILDDIR)
	(cd $(BUILDDIR) && VERBOSE=1 cmake -DPICO_BOARD=${SUBTARGET} -DTOTALMEM=${TOTALMEM} -DENABLE_PSRAM=${ENABLE_PSRAM} -DPICO_NO_PICOTOOL=${PICO_NO_PICOTOOL} ..)

image:: world
	./update-flash.sh
	../../../Standalone/mkftl -s 1952 -e 0x1000 -g 10 filesystem.img -o filesystem.ftl
	${PICOTOOL} uf2 convert filesystem.ftl -t bin filesystem.uf2 -o 0x10018000 ${PICOTOOLFLAGS}

diskimage:

clean:
	rm -rf $(BUILDDIR)
	rm -f uf2conv
	$(MAKE) -C ../../../Library/libs -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/util -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/audio -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/netd -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/V7/cmd -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/V7/cmd/sh -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/games -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/levee -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/cave -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/cursesgames -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/mc -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/vector -f Makefile.armm0 clean
	$(MAKE) -C ../../../Standalone clean
	$(MAKE) -C utils clean

world: $(BUILDDIR)/fuzix.elf
	$(MAKE) -C ../../../Library/libs -f Makefile.armm0
	$(MAKE) -C ../../../Applications/util -f Makefile.armm0
	$(MAKE) -C ../../../Applications/audio -f Makefile.armm0
	$(MAKE) -C ../../../Applications/netd -f Makefile.armm0
	$(MAKE) -C ../../../Applications/V7/cmd -f Makefile.armm0
	$(MAKE) -C ../../../Applications/V7/cmd/sh -f Makefile.armm0
	$(MAKE) -C ../../../Applications/games -f Makefile.armm0
	$(MAKE) -C ../../../Applications/levee -f Makefile.armm0
	$(MAKE) -C ../../../Applications/cave -f Makefile.armm0
	$(MAKE) -C ../../../Applications/cursesgames -f Makefile.armm0
	$(MAKE) -C ../../../Applications/mc -f Makefile.armm0
	$(MAKE) -C ../../../Applications/vector -f Makefile.armm0
	$(MAKE) -C ../../../Standalone
	$(MAKE) -C utils
