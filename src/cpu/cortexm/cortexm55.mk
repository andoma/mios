GLOBALDEPS += ${SRC}/cpu/cortexm/cortexm55.mk

CFLAGS += -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -mfp16-format=ieee

CPPFLAGS += -include ${SRC}/cpu/cortexm/cortexm55.h

SRCS += ${C}/cortexm55.c \
	${C}/cache.c \
	${C}/mpu_v8.c \

include ${SRC}/cpu/cortexm/cortexm.mk

