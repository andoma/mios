GLOBALDEPS += ${SRC}/net/net.mk

SRCS += ${SRC}/net/pbuf.c

ENABLE_NET := $(findstring yes,\
	${ENABLE_NET_MBUS}\
	${ENABLE_NET_CAN}\
	${ENABLE_NET_BLE}\
	${ENABLE_NET_IPV4})

ENABLE_NET_ETHER := $(call OR, ${ENABLE_NET_IPV4})
ENABLE_NET_DSIG := $(call OR, ${ENABLE_NET_MBUS} ${ENABLE_NET_CAN})

SRCS-${ENABLE_NET_DSIG} += \
	${SRC}/net/dsig.c \
	${SRC}/net/vllp.c \
	${SRC}/net/pushpull.c \

SRCS-${ENABLE_NET_MBUS} += \
	${SRC}/net/mbus/mbus.c \
	${SRC}/net/mbus/mbus_seqpkt.c \

SRCS-${ENABLE_NET_CAN} += \
	${SRC}/net/can/can.c \

SRCS-${ENABLE_NET_MBUS}-${ENABLE_RPC} += \
	${SRC}/net/mbus/mbus_rpc.c

SRCS-${ENABLE_NET_IPV4} += \
	${SRC}/net/ipv4/ipv4.c \
	${SRC}/net/ipv4/igmp.c \
	${SRC}/net/ipv4/udp.c \
	${SRC}/net/ipv4/tcp.c \
	${SRC}/net/ipv4/dhcpv4.c \
	${SRC}/net/ipv4/ntp.c \
	${SRC}/net/ipv4/cmd_ipv4.c \
	${SRC}/net/ipv4/mdns.c \

SRCS-${ENABLE_NET_ETHER} += \
	${SRC}/net/ether.c \
	${SRC}/net/lldp.c \
	${SRC}/net/ethphy/ethphy_dp83826.c \

SRCS-${ENABLE_NET_HTTP} += \
       ${SRC}/net/http/http.c \
       ${SRC}/net/http/http_parser.c \
       ${SRC}/net/http/http_stream.c \
       ${SRC}/net/http/http_util.c \

SRCS-${ENABLE_NET_MBUS_GW} += \
	${SRC}/net/mbus/mbus_gateway.c \

SRCS-${ENABLE_NET_BLE} += \
	${SRC}/net/ble/l2cap.c

SRCS-${ENABLE_NET} += \
	${SRC}/net/service.c \
	${SRC}/net/net_main.c \
	${SRC}/net/net_log.c \
	${SRC}/net/service/svc_echo.c \
	${SRC}/net/service/svc_shell.c \
	${SRC}/net/service/svc_chargen.c \
	${SRC}/net/service/svc_discard.c \
	${SRC}/net/service/svc_ota.c \
	${SRC}/net/service/svc_alert.c \

SRCS-${ENABLE_RPC} += \
	${SRC}/net/service/svc_rpc.c \

SRCS-${ENABLE_NET_IPV4} += \
	${SRC}/net/service/svc_telnet.c \


${MOS}/net/%.o : CFLAGS += ${NOFPU}
${MOS}/net/mbus/%.o : CFLAGS += ${NOFPU}
${MOS}/net/ipv4/%.o : CFLAGS += ${NOFPU}
${MOS}/net/service/%.o : CFLAGS += ${NOFPU}
