SRCS += ${SRC}/kernel/mios.c \
	${SRC}/kernel/io.c \
	${SRC}/kernel/task.c \
	${SRC}/kernel/device.c \
	${SRC}/kernel/timer.c \

${MOS}/kernel/%.o : CFLAGS += ${NOFPU}
