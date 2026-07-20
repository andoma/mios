BOARDNAME := nrf54l15-dk

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

ENABLE_LITTLEFS := yes
ENABLE_NET_BLE := yes
ENABLE_NET_DSIG := yes
ENABLE_NRF_SDC := yes

FLASH_METHOD := jlink

include ${SRC}/platform/nrf54l/nrf54l.mk

SRCS += ${B}/${BOARDNAME}.c
