BOARDNAME := stm32n6-dk

ENABLE_NET_IPV4 := yes
ENABLE_NET_HTTP := yes
ENABLE_RTL8211F := yes
ENABLE_LITTLEFS := yes

B := ${SRC}/platform/${BOARDNAME}

GLOBALDEPS += ${B}/${BOARDNAME}.mk

CPPFLAGS += -I${B} -include ${BOARDNAME}.h

include ${SRC}/platform/stm32n6/stm32n6.mk

SRCS += ${B}/${BOARDNAME}.c

