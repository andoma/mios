BOARDNAME := nrf52840-dongle

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

# Console and firmware update are over USB (the dongle exposes no debug UART).
ENABLE_NET_BLE := yes
NRF_DEVICE := NRF52840_XXAA

# App runs above the factory Nordic MBR + USB DFU bootloader (see the .ld).
LDSCRIPT := ${B}/${BOARDNAME}.ld

# `make flash` speaks the nRF DFU serial protocol to the factory bootloader.
FLASH_METHOD := nrfdfu

include ${SRC}/platform/nrf52/nrf52.mk

SRCS += ${B}/${BOARDNAME}.c \
	${SRC}/platform/nrf52/nrf52840_usb.c \
