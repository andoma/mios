P := ${SRC}/platform/tegra234-ccplex

GLOBALDEPS += ${P}/tegra234-ccplex.mk

CFLAGS += -mcpu=cortex-a78ae

CPPFLAGS += -iquote${P} -include ${P}/tegra234-ccplex.h

LDSCRIPT = ${P}/tegra234-ccplex.ld

include ${SRC}/cpu/aarch64/aarch64.mk

SRCS += ${P}/tegra234-ccplex.c

ENABLE_TASK_ACCOUNTING := no

GDB_PORT ?= 1234
GDB_HOST ?= 127.0.0.1

qemu: ${O}/build.elf
	qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a57 -nographic -kernel $< -s -S

gdb: ${O}/build.elf
	${GDB} -ex "target extended-remote ${GDB_HOST}:${GDB_PORT}" -x ${T}/gdb/macros $<
