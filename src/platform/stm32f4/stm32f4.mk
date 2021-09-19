P := ${SRC}/platform/stm32f4

GLOBALDEPS += ${P}/stm32f4.mk

CPPFLAGS += -iquote${P} -include ${P}/stm32f4.h

CPPFLAGS += -DHAVE_HRTIMER

LDSCRIPT = ${P}/stm32f4.ld

include ${SRC}/cpu/cortexm/cortexm4f.mk

SRCS += ${P}/stm32f4.c \
	${P}/stm32f4_info.c \
	${P}/stm32f4_gpio.c \
	${P}/stm32f4_iwdog.c \
	${P}/stm32f4_i2c.c \
	${P}/stm32f4_spi.c \
	${P}/stm32f4_dma.c \
	${P}/stm32f4_tim.c \
	${P}/stm32f4_clk.c \
	${P}/stm32f4_uart.c \
	${P}/stm32f4_flash.c \
	${P}/stm32f4_adc.c \
	${P}/stm32f4_otgfs.c \

${MO}/src/platform/stm32f4/%.o : CFLAGS += ${NOFPU}
