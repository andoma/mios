P := ${SRC}/platform/stm32g0

GLOBALDEPS += ${P}/stm32g0.mk

CPPFLAGS += -iquote${P}

CPPFLAGS += -DHAVE_HRTIMER

LDSCRIPT = ${P}/stm32g0.ld

include ${SRC}/cpu/cortexm/cortexm0plus.mk

SRCS += ${P}/stm32g0.c \
	${P}/stm32g0_clk.c \
	${P}/stm32g0_gpio.c \
	${P}/stm32g0_uart.c \
	${P}/stm32g0_tim.c \
