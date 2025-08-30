GLOBALDEPS += ${SRC}/util/util.mk

SRCS += ${SRC}/util/alert.c \
	${SRC}/util/bumpalloc.c \
	${SRC}/util/base64.c \
	${SRC}/util/crc32.c \
	${SRC}/util/crc16.c \
	${SRC}/util/crc8.c \
	${SRC}/util/crc4.c \
	${SRC}/util/hdlc.c \
	${SRC}/util/ntcpoly.c \
	${SRC}/util/splice.c \
	${SRC}/util/block.c \
	${SRC}/util/datetime.c \
	${SRC}/util/smux.c \
	${SRC}/util/pipe.c \
	${SRC}/util/pmem.c \
	${SRC}/util/fdt.c \

${MOS}/util/%.o : CFLAGS += ${NOFPU}
