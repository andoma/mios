
# BLE via Nordic's SoftDevice Controller (nRF52832).
ENABLE_NET_BLE=yes
ENABLE_NET_MBUS=yes
NRF_DEVICE := NRF52832_XXAA

BOARDNAME := bluefruit-nrf52

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

include ${SRC}/platform/nrf52/nrf52.mk

SRCS += ${B}/${BOARDNAME}.c
