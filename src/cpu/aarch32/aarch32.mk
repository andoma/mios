ENABLE_TASK_ACCOUNTING ?= no

C := ${SRC}/cpu/aarch32

GLOBALDEPS += ${C}/aarch32.mk ${C}/aarch32.ld

CPPFLAGS += -iquote${C}

CPPFLAGS += -include ${SRC}/cpu/aarch32/aarch32.h

TOOLCHAIN := arm-none-eabi-

GDB ?= ${TOOLCHAIN}gdb

ENTRYPOINT ?= start
LDFLAGS += -e ${ENTRYPOINT}

#CFLAGS += -mthumb -mcpu=cortex-a9
CFLAGS += -mcpu=cortex-a9

NOFPU := -mgeneral-regs-only

SRCS += ${C}/cpu.c \
	${C}/exc.c \

${MOS}/cpu/aarch32/%.o : CFLAGS += ${NOFPU}
