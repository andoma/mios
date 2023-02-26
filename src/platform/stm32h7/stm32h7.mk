P := ${SRC}/platform/stm32h7

GLOBALDEPS += ${P}/stm32h7.mk

CPPFLAGS += -iquote${P} -include ${P}/stm32h7.h

LDSCRIPT = ${P}/stm32h7.ld

include ${SRC}/cpu/cortexm/cortexm7.mk

SRCS += ${C}/systick.c \
	${P}/stm32h7.c \
	${P}/stm32h7_clk.c \
	${P}/stm32h7_crc.c \
	${P}/stm32h7_gpio.c \
	${P}/stm32h7_uart.c \
	${P}/stm32h7_i2c.c \
	${P}/stm32h7_dma.c \

SRCS-${ENABLE_NET_IPV4} += \
	${P}/stm32h7_eth.c \


${MOS}/platform/stm32h7/%.o : CFLAGS += ${NOFPU}

