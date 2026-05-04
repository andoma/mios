P := ${SRC}/platform/stm32n6

GLOBALDEPS += ${P}/stm32n6.mk

CPPFLAGS += -iquote${P} -include ${P}/stm32n6.h

LDSCRIPT ?= ${P}/stm32n6.ld

# ENTRYPOINT = bl_start

include ${SRC}/cpu/cortexm/cortexm55.mk

SRCS += ${C}/systick.c \
	${P}/stm32n6_systim.c \
	${P}/stm32n6_entry.s \
	${P}/stm32n6.c \
	${P}/stm32n6_clk.c \
	${P}/stm32n6_gpio.c \
	${P}/stm32n6_uart.c \
	${P}/stm32n6_xspi.c \
	${P}/stm32n6_flash.c \
	${P}/stm32n6_usb.c \
	${P}/stm32n6_spi.c \
	${P}/stm32n6_i2c.c \
	${P}/stm32n6_dma.c \
	${P}/stm32n6_rif.c \
	${P}/stm32n6_idle.c \
	${P}/stm32n6_iwdg.c \
	${P}/stm32n6_info.c \

SRCS-${ENABLE_NET_IPV4} += ${P}/stm32n6_eth.c

SRCS += ${P}/boot/isr.s \
	${P}/boot/stm32n6_bootloader.c

${MOS}/platform/stm32n6/boot/stm32n6_bootloader.o : CFLAGS = -Os -ffreestanding -Wall -Werror -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -mgeneral-regs-only -iquote${P}

${MOS}/platform/stm32n6/%.o : CFLAGS += ${NOFPU}

