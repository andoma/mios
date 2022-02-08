C := ${SRC}/cpu/cortexm

GLOBALDEPS += ${C}/cortexm.mk

CPPFLAGS += -iquote${C}

CPPFLAGS += -include ${SRC}/cpu/cortexm/cortexm.h

TOOLCHAIN := arm-none-eabi-

LDFLAGS += -e start

NOFPU := -mgeneral-regs-only

SRCS += ${C}/isr.s \
	${C}/irq.c \
	${C}/exc.c \
	${C}/cpu.c \
	${C}/systick.c \
	${C}/rnd.c \

${MOS}/cpu/cortexm/%.o : CFLAGS += ${NOFPU}
