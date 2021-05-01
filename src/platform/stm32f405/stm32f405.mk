include ${SRC}/platform/stm32f4/stm32f4.mk

GLOBALDEPS += ${SRC}/platform/stm32f405/stm32f405.mk

CPPFLAGS += -include stm32f4_ccm.h

SRCS += ${P}/stm32f4_ccm.c \
	${P}/stm32f4_rnd.c \

