ALLPLATFORMS := \
	lm3s811evb \
	stm32f405-feather \
	stm32g0-nucleo64 \
	stm32f407g-disc1 \
	bluefruit-nrf52 \
	stm32f439-nucleo144 \
	vexpress-a9 \

${ALLPLATFORMS}:
	$(MAKE) PLATFORM=$@

allplatforms: ${ALLPLATFORMS}
