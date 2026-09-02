ALLPLATFORMS := \
	lm3s811evb \
	stm32f405-feather \
	stm32g0-nucleo64 \
	stm32f407g-disc1 \
	bluefruit-nrf52 \
	stm32f439-nucleo144 \
	stm32g4-usb \
	vexpress-a9 \
	stm32h7-nucleo144 \
	nrf54l15-dk \
	nrf52840-dongle \

# The host platform is a native Linux x86-64 process
ifeq ($(shell uname -sm),Linux x86_64)
ALLPLATFORMS += host
endif

${ALLPLATFORMS}:
	$(MAKE) PLATFORM=$@

allplatforms: ${ALLPLATFORMS}

