GLOBALDEPS += ${SRC}/cpu/cortexm/cortexm7.mk

CFLAGS += -mcpu=cortex-m7 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

CPPFLAGS += -include ${SRC}/cpu/cortexm/cortexm7.h

SRCS += ${C}/cortexm7.c \

include ${SRC}/cpu/cortexm/cortexm.mk

