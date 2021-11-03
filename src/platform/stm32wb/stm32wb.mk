P := ${SRC}/platform/stm32wb

GLOBALDEPS += ${P}/stm32wb.mk

CPPFLAGS += -iquote${P} -include ${P}/stm32wb.h

LDSCRIPT = ${P}/stm32wb.ld

include ${SRC}/cpu/cortexm/cortexm4f.mk

SRCS += ${P}/stm32wb.c \
	${P}/stm32wb_gpio.c \
	${P}/stm32wb_clk.c \
	${P}/stm32wb_uart.c \
	${P}/stm32wb_dma.c \
	${P}/stm32wb_spi.c \

${MO}/src/platform/stm32wb/%.o : CFLAGS += ${NOFPU}
