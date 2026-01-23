SUBTARGET ?= pico
ENABLE_PSRAM ?= 1

fuzix.bin::
	+make -C platform/platform-rpipico image
	cp platform/platform-rpipico/build_$(SUBTARGET)_psram$(ENABLE_PSRAM)/fuzix.uf2 $@
