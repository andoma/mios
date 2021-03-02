
P := ${SRC}/platform/lm3s811evb

GLOBALDEPS += ${P}/lm3s811evb.mk

CPPFLAGS += -I${P} -include lm3s811evb.h

LDSCRIPT = ${P}/lm3s811evb.ld

# Technically this "Stellaris" board is an cortex-m3 but qemu supports FPU

CFLAGS += -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

include ${SRC}/cpu/cortexm/cortexm4f.mk

SRCS += ${P}/lm3s811evb.c \
	${P}/console.c \

