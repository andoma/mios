
BOARDNAME := stm32f439-nucleo144

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

ENABLE_NET_IPV4 := yes

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

include ${SRC}/platform/stm32f4/stm32f4.mk

SRCS += ${B}/${BOARDNAME}.c
