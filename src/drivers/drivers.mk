
GLOBALDEPS += ${SRC}/drivers/drivers.mk

SRCS += ${SRC}/drivers/ms5611.c

SRCS += ${SRC}/drivers/mpu9250.c

SRCS += ${SRC}/drivers/sx1280/sx1280.c \
	${SRC}/drivers/sx1280/sx1280_tdma.c \
	${SRC}/drivers/sx1280_mios.c

${MO}/src/drivers/sx1280/%.o : CFLAGS += ${NOFPU}
${MO}/src/drivers/sx1280_mios.o : CFLAGS += ${NOFPU}
