CPU := riscv64

LDSCRIPT := ${P}/linker.ld

LDFLAGS += -Wl,--build-id=none

C := src/cpu/${CPU}
include ${C}/cpu.mk

SRCS += ${P}/platform.c \
	${P}/console.c \

