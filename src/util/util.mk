GLOBALDEPS += ${SRC}/util/util.mk

SRCS += ${SRC}/util/crc32.c \
	${SRC}/util/crc8.c \
	${SRC}/util/hdlc.c \
	${SRC}/util/pkv.c \
	${SRC}/util/dsig.c \
	${SRC}/util/ntcpoly.c \

SRCS-${ENABLE_OTA} += ${SRC}/util/ota.c

${MOS}/util/%.o : CFLAGS += ${NOFPU}
