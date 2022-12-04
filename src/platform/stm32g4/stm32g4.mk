P := ${SRC}/platform/stm32g4

GLOBALDEPS += ${P}/stm32g4.mk

CPPFLAGS += -iquote${P}

CPPFLAGS += -include stm32g4.h

#CPPFLAGS += -DHAVE_HRTIMER

LDSCRIPT = ${P}/stm32g4.ld

include ${SRC}/cpu/cortexm/cortexm4f.mk

SRCS += ${P}/stm32g4.c \
	${P}/stm32g4_idle.c \
	${P}/stm32g4_clk.c \
	${P}/stm32g4_gpio.c \
	${P}/stm32g4_uart.c \
	${P}/stm32g4_spi.c \
	${P}/stm32g4_dma.c \
	${P}/stm32g4_i2c.c \
	${P}/stm32g4_usb.c \

${MO}/src/platform/stm32g4/%.o : CFLAGS += ${NOFPU}
