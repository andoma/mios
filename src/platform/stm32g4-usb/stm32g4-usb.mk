
BOARDNAME := stm32g4-usb

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

ENABLE_NET_CORE := yes

FLASH_METHOD := dfu

include ${SRC}/platform/stm32g4/stm32g4.mk

SRCS += ${B}/${BOARDNAME}.c
