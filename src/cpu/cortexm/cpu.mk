TOOLCHAIN := arm-none-eabi-

CFLAGS += -mcpu=cortex-m4 -mthumb

LDSCRIPT = ${C}/linker.ld

SRCS += ${C}/isr.s \
	${C}/exc.c \
	${C}/systick.c \

