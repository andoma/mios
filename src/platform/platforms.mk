
ALLPLATFORMS := \
	lm3s811evb \
	stm32f405-feather \
	stm32g0-nucleo64 \
	stm32f407g-disc1 \
	stm32h7-nucleo144 \

${ALLPLATFORMS}:
	$(MAKE) PLATFORM=$@

allplatforms: ${ALLPLATFORMS}
