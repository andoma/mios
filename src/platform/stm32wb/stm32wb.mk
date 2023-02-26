P := ${SRC}/platform/stm32wb

GLOBALDEPS += ${P}/stm32wb.mk

CPPFLAGS += -iquote${P} -include ${P}/stm32wb.h

LDSCRIPT = ${P}/stm32wb.ld

include ${SRC}/cpu/cortexm/cortexm4f.mk

SRCS += ${C}/systick.c \
	${P}/stm32wb.c \
	${P}/stm32wb_idle.c \
	${P}/stm32wb_gpio.c \
	${P}/stm32wb_clk.c \
	${P}/stm32wb_uart.c \
	${P}/stm32wb_dma.c \
	${P}/stm32wb_spi.c \
	${P}/stm32wb_adc.c \
	${P}/stm32wb_i2c.c \
	${P}/stm32wb_info.c \
	${P}/stm32wb_flash.c \

${MOS}/platform/stm32wb/%.o : CFLAGS += ${NOFPU}
