TOOLCHAIN := riscv64-linux-gnu-

CFLAGS += -mcmodel=medany -mabi=lp64 -nostdlib

LDFLAGS += -nostartfiles

SRCS += ${C}/init.s \
	${C}/timer.c \
	${C}/trap.c \
	${C}/cpu.c \


