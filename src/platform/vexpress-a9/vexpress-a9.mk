
ENABLE_TASK_DEBUG := yes

PLATFORM := vexpress-a9

P := ${SRC}/platform/${PLATFORM}

GLOBALDEPS += ${P}/${PLATFORM}.mk

CPPFLAGS += -I${P} -include ${PLATFORM}.h

LDSCRIPT = ${P}/vexpress-a9.ld

include ${SRC}/cpu/aarch32/aarch32.mk

PE = ${C}/peripherial/gicv1

CPPFLAGS += -I${PE}

SRCS += ${P}/vexpress-a9.c \
	${P}/pl011.c \

SRCS += ${PE}/entry.s \
	${PE}/irq.c \
	${PE}/systick.c \

${P}/%.o : CFLAGS += ${NOFPU}
${PE}/%.o : CFLAGS += ${NOFPU}


run: ${O}/build.elf
	qemu-system-arm -M vexpress-a9 -m 32M -nographic -kernel $<

qemu:
	qemu-system-arm -S -s -M vexpress-a9 -m 32M -nographic

gdb: ${O}/build.elf
	${GDB} -ex "target extended-remote localhost:1234" ${O}/build.elf
