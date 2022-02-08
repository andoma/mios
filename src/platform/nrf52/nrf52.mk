P := ${SRC}/platform/nrf52

GLOBALDEPS += ${P}/nrf52.mk

CPPFLAGS += -iquote${P} -include ${P}/nrf52.h

LDSCRIPT = ${P}/nrf52.ld

include ${SRC}/cpu/cortexm/cortexm4f.mk

SRCS += ${P}/nrf52.c \
	${P}/nrf52_uart.c \
	${P}/nrf52_gpio.c \

${MOS}/platform/nrf52/%.o : CFLAGS += ${NOFPU}
