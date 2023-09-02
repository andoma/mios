GLOBALDEPS += ${SRC}/net/net.mk

SRCS += ${SRC}/net/pbuf.c

SRCS_net += \
	${SRC}/net/service.c \
	${SRC}/net/net_main.c \
	${SRC}/net/net_log.c \
	${SRC}/net/service/svc_echo.c \
	${SRC}/net/service/svc_shell.c \
	${SRC}/net/service/svc_chargen.c \
	${SRC}/net/service/svc_rpc.c \
	${SRC}/net/service/svc_discard.c \

SRCS-${ENABLE_NET_MBUS} += ${SRCS_net} \
	${SRC}/net/mbus/mbus.c \
	${SRC}/net/mbus/mbus_seqpkt.c \
	${SRC}/net/mbus/mbus_dsig.c \
	${SRC}/net/mbus/mbus_rpc.c \

SRCS-${ENABLE_NET_IPV4} += ${SRCS_net} \
	${SRC}/net/ether.c \
	${SRC}/net/ipv4/ipv4.c \
	${SRC}/net/ipv4/udp.c \
	${SRC}/net/ipv4/tcp.c \
	${SRC}/net/ipv4/dhcpv4.c \
	${SRC}/net/ipv4/cmd_ipv4.c \

SRCS-${ENABLE_NET_MBUS_GW} += ${SRCS_net} \
	${SRC}/net/mbus/mbus_gateway.c \

SRCS-${ENABLE_NET_BLE} += ${SRCS_net} \
	${SRC}/net/ble/l2cap.c

${MOS}/net/%.o : CFLAGS += ${NOFPU}
${MOS}/net/mbus/%.o : CFLAGS += ${NOFPU}
${MOS}/net/ipv4/%.o : CFLAGS += ${NOFPU}
${MOS}/net/service/%.o : CFLAGS += ${NOFPU}
