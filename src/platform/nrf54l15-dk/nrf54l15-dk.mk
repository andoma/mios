BOARDNAME := nrf54l15-dk

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

include ${SRC}/platform/nrf54l/nrf54l.mk

SRCS += ${B}/${BOARDNAME}.c
