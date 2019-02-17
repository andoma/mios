TOOLCHAIN := arm-none-eabi-

CFLAGS += -mcpu=cortex-m4 -mthumb

LDSCRIPT = ${C}/linker.ld

SRCS += ${C}/isr.s \
	${C}/irq.c \
	${C}/exc.c \
	${C}/cpu.c \
	${C}/systick.c \

