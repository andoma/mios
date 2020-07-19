CPU := cortexm

C := src/cpu/${CPU}

CFLAGS += -mfpu=fpv4-sp-d16  -mfloat-abi=hard

LDSCRIPT = ${P}/stm32f4.ld

include ${C}/cpu.mk

SRCS += ${P}/platform.c \
	${P}/console.c \
	${P}/gpio.c \


