SRCS += ${SRC}/kernel/mios.c \
	${SRC}/kernel/io.c \
	${SRC}/kernel/task.c \

${MO}/src/kernel/%.o : CFLAGS += ${NOFPU}
