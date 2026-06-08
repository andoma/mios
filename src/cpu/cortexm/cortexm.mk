C := ${SRC}/cpu/cortexm

GLOBALDEPS += ${C}/cortexm.mk ${C}/cortexm.ld

CPPFLAGS += -iquote${C}

CPPFLAGS += -include ${SRC}/cpu/cortexm/cortexm.h

CFLAGS += -funwind-tables

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
	${C}/unwind.c \

${MOS}/cpu/cortexm/%.o : CFLAGS += ${NOFPU}

GDB_PORT ?= 3333
GDB_HOST ?= 127.0.0.1

stlink: ${O}/build.elf
	${GDB} -ex "target extended-remote ${GDB_HOST}:${GDB_PORT}" -x ${T}/gdb/macros $<

DFU_SRC = \
	${T}/host/mios_image.c \
	${T}/support/dfu/dfu.c

# dfu.c / mios_image.h are #included by support/dfu/dfu.c, so list them as
# prerequisites too (otherwise edits to them don't trigger a rebuild).
DFU_DEPS = ${DFU_SRC} ${T}/host/dfu.c ${T}/host/mios_image.h

${O}/dfu: ${DFU_DEPS} ${ALLDEPS}
	@mkdir -p $(dir $@)
	$(CC) -I${T}/host -Og -ggdb -Wall -Werror -o $@ ${DFU_SRC} $(shell pkg-config --libs --cflags libusb-1.0)

# Feed the tool the stripped ELF (stripped-build.elf is defined in the
# top-level Makefile): the STM32N6 path embeds the whole ELF as the parked
# image, so it must be compact. Loadable segments, the build-id note and
# app/version sections are preserved, so the internal-flash path is fine.
dfu: ${O}/stripped-build.elf ${O}/dfu
	${O}/dfu ${O}/stripped-build.elf

sysdfu: ${O}/build.bin
	dfu-util -a 0 -D $< -s 0x08000000:leave
