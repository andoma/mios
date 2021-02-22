GLOBALDEPS += ${SRC}/cpu/cortexm/cortexm7.mk

CFLAGS += -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16

CPPFLAGS += -include ${SRC}/cpu/cortexm/cortexm7.h

SRCS += ${C}/cortexm7.c \

include ${SRC}/cpu/cortexm/cortexm.mk

