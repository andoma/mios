
BOARDNAME := stm32f405-feather

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

include ${SRC}/platform/stm32f405/stm32f405.mk

SRCS += ${B}/${BOARDNAME}.c
