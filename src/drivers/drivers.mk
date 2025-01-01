
GLOBALDEPS += ${SRC}/drivers/drivers.mk

SRCS-${ENABLE_MS5611} += \
	${SRC}/drivers/ms5611.c

SRCS-${ENABLE_MPU9250} += \
	${SRC}/drivers/mpu9250.c

SRCS-${ENABLE_BMI120} += \
	${SRC}/drivers/bmi120.c

SRCS-${ENABLE_HDC1080} += \
	${SRC}/drivers/hdc1080.c

SRCS-${ENABLE_BT81X} += \
	${SRC}/drivers/gpu/bt81x/bt81x.c

SRCS-${ENABLE_MCP23008} += \
	${SRC}/drivers/mcp23008.c

SRCS-${ENABLE_SSD1306} += \
	${SRC}/drivers/ssd1306.c

SRCS-${ENABLE_PL011} += \
	${SRC}/drivers/pl011.c
${MOS}/drivers/pl011.o : CFLAGS += ${NOFPU}

SRCS += ${SRC}/drivers/spiflash.c
${MOS}/drivers/spiflash.o : CFLAGS += ${NOFPU}
