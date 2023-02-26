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
	${C}/rnd.c \

${MOS}/cpu/cortexm/%.o : CFLAGS += ${NOFPU}

GDB_PORT ?= 3333

stlink: ${O}/build.elf
	${GDB} -ex "target extended-remote localhost:${GDB_PORT}" $<

dfu: ${O}/build.bin
	dfu-util -a 0 -D $< -s 0x08000000:leave
