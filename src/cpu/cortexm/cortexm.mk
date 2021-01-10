C := ${SRC}/cpu/cortexm

GLOBALDEPS += ${C}/cortexm.mk
CPPFLAGS += -iquote${C}

TOOLCHAIN := arm-none-eabi-

LDSCRIPT ?= ${C}/linker.ld

NOFPU := -mgeneral-regs-only

SRCS += ${C}/isr.s \
	${C}/irq.c \
	${C}/exc.c \
	${C}/cpu.c \
	${C}/systick.c \
	${C}/rnd.c \

${MO}/src/cpu/cortexm/%.o : CFLAGS += ${NOFPU}
