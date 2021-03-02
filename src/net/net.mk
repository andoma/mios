GLOBALDEPS += ${SRC}/net/net.mk

SRCS-${ENABLE_NET_PCS} += \
	${SRC}/net/pcs/pcs.c \
	${SRC}/net/pcs_shell.c

SRCS-${ENABLE_HDLC} += \
	${SRC}/net/hdlc.c

SRCS += ${SRC}/net/crc32.c

${MO}/src/pcs/pcs.o : CFLAGS += ${NOFPU}
${MO}/src/net/%.o : CFLAGS += ${NOFPU}
