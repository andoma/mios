GLOBALDEPS += ${SRC}/cpu/cortexm/cortexm0plus.mk

ENABLE_MATH ?= no
ENABLE_TASK_ACCOUNTING ?= no

CFLAGS += -mcpu=cortex-m0plus -mthumb

CPPFLAGS += -include ${SRC}/cpu/cortexm/cortexm0plus.h

SRCS += ${C}/cortexm0plus.s \

${MOS}/cpu/cortexm/unwind.o : CFLAGS += -fno-lto

include ${SRC}/cpu/cortexm/cortexm.mk
