
BOARDNAME := stm32f407g-disc1

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

include ${SRC}/platform/stm32f407/stm32f407.mk

SRCS += ${B}/${BOARDNAME}.c
