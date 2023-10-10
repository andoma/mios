GLOBALDEPS += ${SRC}/util/util.mk

SRCS += ${SRC}/util/bumpalloc.c \
	${SRC}/util/base64.c \
	${SRC}/util/crc32.c \
	${SRC}/util/crc8.c \
	${SRC}/util/crc4.c \
	${SRC}/util/hdlc.c \
	${SRC}/util/pkv.c \
	${SRC}/util/dsig.c \
	${SRC}/util/ntcpoly.c \

SRCS-${ENABLE_OTA} += ${SRC}/util/ota.c

${MOS}/util/%.o : CFLAGS += ${NOFPU}
