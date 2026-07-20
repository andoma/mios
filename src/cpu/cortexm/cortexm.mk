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

stlink: ${O}/${ARTIFACT}.elf
	${GDB} -ex "target extended-remote ${GDB_HOST}:${GDB_PORT}" -x ${T}/gdb/macros $<

# Deprecated alias for 'make flash' with the DFU backend. Matches the old
# tool's behavior: always flash (-f), optional boot cmdline in RAM via
# CMDLINE='usb.vid=0x1234 ...'
dfu: FLASH_METHOD := dfu
dfu: FLASH_OPTS += -f $(if $(CMDLINE),-c "$(CMDLINE)")
dfu: flash

sysdfu: ${O}/${ARTIFACT}.bin
	dfu-util -a 0 -D $< -s 0x08000000:leave
