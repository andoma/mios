CPU := cortexm

CFLAGS += -mfpu=fpv4-sp-d16  -mfloat-abi=hard

LDSCRIPT = ${P}/stm32f4.ld

C := ${SRC}/cpu/${CPU}
include ${C}/${CPU}.mk

SRCS += ${P}/platform.c \
	${P}/uart.c \
	${P}/console.c \
	${P}/gpio.c \


