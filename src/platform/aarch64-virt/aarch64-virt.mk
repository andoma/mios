ENABLE_PL011 := yes

P := ${SRC}/platform/aarch64-virt

GLOBALDEPS += ${P}/aarch64-virt.mk

CPPFLAGS += -iquote${P} -include ${P}/aarch64-virt.h

LDSCRIPT = ${P}/aarch64-virt.ld

include ${SRC}/cpu/aarch64/aarch64.mk

SRCS += \
	${P}/aarch64-virt.c \

ENABLE_TASK_ACCOUNTING := no

GDB_PORT ?= 1234
GDB_HOST ?= 127.0.0.1

qemu: ${O}/build.elf
	qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a57 -nographic -kernel $< -s -S

run: ${O}/build.elf
	qemu-system-aarch64 -m 8192 -M virt,gic-version=3 -cpu cortex-a57 -nographic -kernel $<

gdb: ${O}/build.elf
	${GDB} -ex "target extended-remote ${GDB_HOST}:${GDB_PORT}" -ex "layout asm" -ex "layout regs" -x ${T}/gdb/macros $<
