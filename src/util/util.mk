GLOBALDEPS += ${SRC}/util/util.mk

SRCS += ${SRC}/util/crc32.c \
	${SRC}/util/crc8.c \
	${SRC}/util/hdlc.c \
	${SRC}/util/pkv.c \
	${SRC}/util/dsig.c \
	${SRC}/util/ntcpoly.c \

${MO}/src/util/%.o : CFLAGS += ${NOFPU}
