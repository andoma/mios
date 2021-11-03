BOARDNAME := stm32wb55-nucleo64

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

include ${SRC}/platform/stm32wb/stm32wb.mk

SRCS += ${B}/${BOARDNAME}.c
