P := ${SRC}/platform/nrf54l

GLOBALDEPS += ${P}/nrf54l.mk

CPPFLAGS += -iquote${P} -include ${P}/nrf54l.h

LDSCRIPT ?= ${P}/nrf54l.ld

ENTRYPOINT ?= start

include ${SRC}/cpu/cortexm/cortexm33.mk

SRCS += ${C}/entry-xip.s \
	${P}/nrf54l.c \
	${P}/nrf54l_uart.c \
	${P}/nrf54l_gpio.c \
	${P}/nrf54l_spi.c \
	${P}/nrf54l_systim.c \
	${P}/nrf54l_wdt.c \
	${SRC}/shell/mcp_uart.c \

SRCS-${ENABLE_NET_BLE} += \
	${P}/nrf54l_radio.c \

${MOS}/platform/nrf54l/%.o : CFLAGS += ${NOFPU}
