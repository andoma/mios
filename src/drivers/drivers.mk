
GLOBALDEPS += ${SRC}/drivers/drivers.mk

SRCS-${ENABLE_MS5611} += \
	${SRC}/drivers/ms5611.c

SRCS-${ENABLE_MPU9250} += \
	${SRC}/drivers/mpu9250.c

SRCS-${ENABLE_SX1280} += \
	${SRC}/drivers/sx1280/sx1280.c \
	${SRC}/drivers/sx1280/sx1280_tdma.c \
	${SRC}/drivers/sx1280_mios.c

SRCS-${ENABLE_HDC1080} += \
	${SRC}/drivers/hdc1080.c

${MO}/src/drivers/sx1280/%.o : CFLAGS += ${NOFPU}
${MO}/src/drivers/sx1280_mios.o : CFLAGS += ${NOFPU}
