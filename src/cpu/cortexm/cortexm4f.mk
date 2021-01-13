GLOBALDEPS += ${SRC}/cpu/cortexm/cortexm4f.mk

CFLAGS += -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

CPPFLAGS += -include ${SRC}/cpu/cortexm/cortexm4f.h

SRCS += ${C}/cortexm4f.c \

include ${SRC}/cpu/cortexm/cortexm.mk

