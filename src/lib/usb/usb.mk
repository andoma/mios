GLOBALDEPS += ${SRC}/lib/usb/usb.mk

SRCS +=	${SRC}/lib/usb/usb_common.c \
	${SRC}/lib/usb/usb_cdc.c \

SRCS-${ENABLE_NET_MBUS} += \
	${SRC}/lib/usb/usb_mbus.c \

${MOS}/lib/usb/%.o : CFLAGS += ${NOFPU}
