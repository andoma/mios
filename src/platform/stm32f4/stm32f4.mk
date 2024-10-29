P := ${SRC}/platform/stm32f4

GLOBALDEPS += ${P}/stm32f4.mk

CPPFLAGS += -iquote${P} -include ${P}/stm32f4.h

LDSCRIPT ?= ${P}/stm32f4$(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),_bootloader,).ld
ENTRYPOINT ?= $(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),bl_start,start)


include ${SRC}/cpu/cortexm/cortexm4f.mk

SRCS += ${C}/systick.c \
	${P}/stm32f4.c \
	${P}/stm32f4_info.c \
	${P}/stm32f4_gpio.c \
	${P}/stm32f4_i2c.c \
	${P}/stm32f4_spi.c \
	${P}/stm32f4_dma.c \
	${P}/stm32f4_clk.c \
	${P}/stm32f4_uart_stream.c \
	${P}/stm32f4_adc.c \
	${P}/stm32f4_can.c \
	${P}/stm32f4_otgfs.c \
	${P}/stm32f4_systim.c \

SRCS-${ENABLE_NET_IPV4} += \
	${P}/stm32f4_eth.c \

SRCS-${ENABLE_NET_MBUS} += \
	${P}/stm32f4_uart_mbus.c

${MOS}/platform/stm32f4/%.o : CFLAGS += ${NOFPU}

SRCS-${ENABLE_BUILTIN_BOOTLOADER} += \
	${P}/boot/stm32f4_bootloader.c \
	${P}/boot/isr.s \

${MOS}/platform/stm32f4/boot/stm32f4_bootloader.o : CFLAGS = -Os -ffreestanding -Wall -Werror -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mgeneral-regs-only -include ${BOOTLOADER_DEFS}
