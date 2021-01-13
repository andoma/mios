
BOARDNAME := stm32f405-feather

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

include ${SRC}/platform/stm32f4/stm32f4.mk

SRCS += ${B}/${BOARDNAME}.c
