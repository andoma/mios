GLOBALDEPS += ${SRC}/net/net.mk

SRCS-${ENABLE_NET} += \
	${SRC}/net/pbuf.c \
	${SRC}/net/net_main.c \
	${SRC}/net/ether.c \
	${SRC}/net/ipv4.c \
	${SRC}/net/dhcpv4.c \
	${SRC}/net/cmd_net.c \


SRCS-${ENABLE_NET_PCS} += \
	${SRC}/net/pcs/pcs.c \
	${SRC}/net/pcs_shell.c

${MO}/src/pcs/pcs.o : CFLAGS += ${NOFPU}
${MO}/src/net/%.o : CFLAGS += ${NOFPU}
