ALLPLATFORMS := \
	lm3s811evb \
	stm32f405-feather \
	stm32g0-nucleo64 \
	stm32f407g-disc1 \
	stm32h7-nucleo144 \
	stm32wb55-nucleo64 \
	bluefruit-nrf52 \

${ALLPLATFORMS}:
	$(MAKE) PLATFORM=$@

allplatforms: ${ALLPLATFORMS}
