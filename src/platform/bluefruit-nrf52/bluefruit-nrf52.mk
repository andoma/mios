
BOARDNAME := bluefruit-nrf52

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

include ${SRC}/platform/nrf52/nrf52.mk

SRCS += ${B}/${BOARDNAME}.c
