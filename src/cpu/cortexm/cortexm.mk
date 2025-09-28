C := ${SRC}/cpu/cortexm

GLOBALDEPS += ${C}/cortexm.mk ${C}/cortexm.ld

CPPFLAGS += -iquote${C}

CPPFLAGS += -include ${SRC}/cpu/cortexm/cortexm.h

TOOLCHAIN := arm-none-eabi-

GDB ?= ${TOOLCHAIN}gdb

ENTRYPOINT ?= start
LDFLAGS += -e ${ENTRYPOINT}

NOFPU := -mgeneral-regs-only

SRCS += ${C}/isr.s \
	${C}/irq.c \
	${C}/exc.c \
	${C}/cpu.c \
	${C}/rnd.c \

${MOS}/cpu/cortexm/%.o : CFLAGS += ${NOFPU}

GDB_PORT ?= 3333
GDB_HOST ?= 127.0.0.1

stlink: ${O}/build.elf
	${GDB} -ex "target extended-remote ${GDB_HOST}:${GDB_PORT}" -x ${T}/gdb/macros $<

DFU_SRC = \
	${T}/host/mios_image.c \
	${T}/support/dfu/dfu.c

${O}/dfu: ${DFU_SRC} ${ALLDEPS}
	@mkdir -p $(dir $@)
	$(CC) -I${T}/host -Og -ggdb -Wall -Werror -o $@ ${DFU_SRC} $(shell pkg-config --libs --cflags libusb-1.0)

dfu: ${O}/build.elf ${O}/dfu
	${O}/dfu ${O}/build.elf

sysdfu: ${O}/build.bin
	dfu-util -a 0 -D $< -s 0x08000000:leave
