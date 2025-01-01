C := ${SRC}/cpu/aarch64

TOOLCHAIN ?= ${TOOLCHAIN_AARCH64}

ENABLE_MATH ?= no
ENABLE_TASK_ACCOUNTING ?= no

GLOBALDEPS += ${C}/aarch64.mk ${C}/aarch64.ld

CPPFLAGS += -iquote${C}

CPPFLAGS += -include ${C}/aarch64.h

CFLAGS += -mgeneral-regs-only

GDB ?= ${TOOLCHAIN}gdb

SRCS += \
	${C}/entry.s \
	${C}/clock.c \
	${C}/cpu.c \
	${C}/exc.c \
	${C}/aarch64.c \


ENTRYPOINT ?= start
LDFLAGS += -e ${ENTRYPOINT}
