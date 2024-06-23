P := ${SRC}/platform/stm32g4

GLOBALDEPS += ${P}/stm32g4.mk

CPPFLAGS += -iquote${P}

CPPFLAGS += -include stm32g4.h

LDSCRIPT = ${P}/stm32g4.ld

include ${SRC}/cpu/cortexm/cortexm4f.mk

SRCS += ${C}/systick.c \
	${P}/stm32g4.c \
	${P}/stm32g4_idle.c \
	${P}/stm32g4_clk.c \
	${P}/stm32g4_gpio.c \
	${P}/stm32g4_uart_stream.c \
	${P}/stm32g4_uart_mbus.c \
	${P}/stm32g4_spi.c \
	${P}/stm32g4_dma.c \
	${P}/stm32g4_i2c.c \
	${P}/stm32g4_usb.c \
	${P}/stm32g4_adc.c \
	${P}/stm32g4_systim.c \
	${P}/stm32g4_crc.c \
	${P}/stm32g4_info.c \

${MO}/src/platform/stm32g4/%.o : CFLAGS += ${NOFPU}
