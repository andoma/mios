GLOBALDEPS += ${SRC}/net/net.mk

SRCS_net += \
	${SRC}/net/pbuf.c \
	${SRC}/net/socket.c \
	${SRC}/net/net_main.c \
	${SRC}/net/ether.c \

SRCS-${ENABLE_NET_MBUS} += ${SRCS_net} \
	${SRC}/net/mbus/mbus.c \
	${SRC}/net/mbus/mbus_rpc.c \
	${SRC}/net/mbus/mbus_dsig.c \

SRCS-${ENABLE_NET_IPV4} += ${SRCS_net} \
	${SRC}/net/ipv4/ipv4.c \
	${SRC}/net/ipv4/udp.c \
	${SRC}/net/ipv4/dhcpv4.c \
	${SRC}/net/ipv4/cmd_ipv4.c \

SRCS-${ENABLE_NET_PCS} += \
	${SRC}/net/pcs/pcs.c \
	${SRC}/net/pcs_shell.c

${MO}/src/net/%.o : CFLAGS += ${NOFPU}
${MO}/src/net/ipv4/%.o : CFLAGS += ${NOFPU}
${MO}/src/net/pcs/%.o : CFLAGS += ${NOFPU}
