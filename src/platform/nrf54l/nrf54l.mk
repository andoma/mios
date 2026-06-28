P := ${SRC}/platform/nrf54l

GLOBALDEPS += ${P}/nrf54l.mk

CPPFLAGS += -iquote${P} -include ${P}/nrf54l.h

LDSCRIPT ?= ${P}/nrf54l.ld

ENTRYPOINT ?= start

include ${SRC}/cpu/cortexm/cortexm33.mk

SRCS += ${C}/entry-xip.s \
	${C}/systick.c \
	${P}/nrf54l.c \
	${P}/nrf54l_uart.c \
	${P}/nrf54l_gpio.c \

${MOS}/platform/nrf54l/%.o : CFLAGS += ${NOFPU}
