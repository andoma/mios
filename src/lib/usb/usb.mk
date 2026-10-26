GLOBALDEPS += ${SRC}/lib/usb/usb.mk

SRCS +=	${SRC}/lib/usb/usb_common.c \
	${SRC}/lib/usb/usb_cdc.c \
	${SRC}/lib/usb/usb_dfu_runtime.c \
	${SRC}/lib/usb/usb_mcp.c \
	${SRC}/lib/usb/usb_host.c \
	${SRC}/lib/usb/usb_host_serial.c \
	${SRC}/lib/usb/usb_typec.c \
	${SRC}/lib/usb/usb_pd.c \

SRCS-${ENABLE_NET_ETHER} += \
	${SRC}/lib/usb/usb_host_ecm.c \

SRCS-${ENABLE_NET_MBUS} += \
	${SRC}/lib/usb/usb_mbus.c \

SRCS-${ENABLE_NET_DSIG} += \
	${SRC}/lib/usb/usb_dsig.c \

${MOS}/lib/usb/%.o : CFLAGS += ${NOFPU}
