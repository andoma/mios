GLOBALDEPS += ${SRC}/kernel/kernel.mk

SRCS += ${SRC}/kernel/mios.c \
	${SRC}/kernel/io.c \
	${SRC}/kernel/task.c \
	${SRC}/kernel/device.c \
	${SRC}/kernel/driver.c \
	${SRC}/kernel/timer.c \
	${SRC}/kernel/eventlog.c \
	${SRC}/kernel/panic.c \

${MOS}/kernel/%.o : CFLAGS += ${NOFPU}
