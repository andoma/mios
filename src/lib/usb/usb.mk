GLOBALDEPS += ${SRC}/lib/usb/usb.mk

SRCS +=	${SRC}/lib/usb/usb_common.c \
	${SRC}/lib/usb/usb_cdc.c \
	${SRC}/lib/usb/usb_mbus.c \

${MO}/src/lib/usb/%.o : CFLAGS += ${NOFPU}
