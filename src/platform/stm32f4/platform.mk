CPU := cortexm

C := src/cpu/${CPU}

LDSCRIPT = ${P}/stm32f4.ld

include ${C}/cpu.mk

SRCS += ${P}/platform.c \
	${P}/gpio.c \


