P := ${SRC}/platform/stm32f4

GLOBALDEPS += ${P}/stm32f4.mk
CPPFLAGS += -I${P}

CFLAGS += -mfpu=fpv4-sp-d16 -mfloat-abi=hard

CPPFLAGS += -DHAVE_HRTIMER

LDSCRIPT = ${P}/stm32f4.ld

include ${SRC}/cpu/cortexm/cortexm.mk

SRCS += ${P}/stm32f4.c \
	${P}/stm32f4_gpio.c \
	${P}/stm32f4_i2c.c \
	${P}/stm32f4_spi.c \
	${P}/stm32f4_dma.c \
	${P}/stm32f4_tim.c \
	${P}/stm32f4_rnd.c \
	${P}/stm32f4_clk.c \
	${P}/stm32f4_uart.c \
	${P}/stm32f4_flash.c \
