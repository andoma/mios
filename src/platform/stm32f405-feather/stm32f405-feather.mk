
B := ${SRC}/platform/stm32f405-feather

GLOBALDEPS += ${B}/stm32f405-feather.mk
CPPFLAGS += -I${B}

include ${SRC}/platform/stm32f4/stm32f4.mk

SRCS += ${B}/stm32f405-feather.c
