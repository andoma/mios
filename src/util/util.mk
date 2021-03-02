GLOBALDEPS += ${SRC}/util/util.mk

SRCS += ${SRC}/util/crc32.c \
	${SRC}/util/hdlc.c \

${MO}/src/util/%.o : CFLAGS += ${NOFPU}
