C := ${SRC}/cpu/cortexm

GLOBALDEPS += ${C}/cortexm.mk ${C}/cortexm.ld

CPPFLAGS += -iquote${C}

CPPFLAGS += -include ${SRC}/cpu/cortexm/cortexm.h

TOOLCHAIN := arm-none-eabi-

GDB ?= ${TOOLCHAIN}gdb

LDFLAGS += -e start

NOFPU := -mgeneral-regs-only

SRCS += ${C}/isr.s \
	${C}/irq.c \
	${C}/exc.c \
	${C}/cpu.c \
	${C}/systick.c \
	${C}/rnd.c \

${MOS}/cpu/cortexm/%.o : CFLAGS += ${NOFPU}

stlink: ${O}/build.elf
	${GDB} -ex "target extended-remote localhost:3333" $<
