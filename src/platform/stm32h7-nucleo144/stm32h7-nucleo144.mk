BOARDNAME := stm32h7-nucleo144

ENABLE_NET_IPV4 := yes

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

include ${SRC}/platform/stm32h7/stm32h7.mk

SRCS += ${B}/${BOARDNAME}.c

