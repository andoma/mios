P := ${SRC}/platform/stm32n6

GLOBALDEPS += ${P}/stm32n6.mk

CPPFLAGS += -iquote${P} -include ${P}/stm32n6.h

LDSCRIPT ?= ${P}/stm32n6.ld

include ${SRC}/cpu/cortexm/cortexm55.mk

SRCS += ${C}/systick.c \
	${P}/stm32n6_entry.s \
	${P}/stm32n6.c \
	${P}/stm32n6_clk.c \
	${P}/stm32n6_gpio.c \
	${P}/stm32n6_uart.c \
	${P}/stm32n6_xspi.c \
	${P}/stm32n6_usb.c \
	${P}/stm32n6_spi.c \
	${P}/stm32n6_i2c.c \

SRCS-${ENABLE_NET_IPV4} += ${P}/stm32n6_eth.c

${MOS}/platform/stm32n6/%.o : CFLAGS += ${NOFPU}

