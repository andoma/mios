GLOBALDEPS += ${SRC}/cpu/cortexm/cortexm33.mk

CFLAGS += -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard

CPPFLAGS += -include ${SRC}/cpu/cortexm/cortexm33.h

SRCS += ${C}/cortexm33.c \
	${C}/mpu_v8.c \

include ${SRC}/cpu/cortexm/cortexm.mk
