
B := ${SRC}/platform/stm32f407g-disc1

GLOBALDEPS += ${B}/stm32f407g-disc1.mk
CPPFLAGS += -I${B}

include ${SRC}/platform/stm32f4/stm32f4.mk

SRCS += ${B}/stm32f407g-disc1.c
