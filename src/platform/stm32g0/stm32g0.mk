P := ${SRC}/platform/stm32g0

GLOBALDEPS += ${P}/stm32g0.mk

CPPFLAGS += -iquote${P} -include ${P}/stm32g0.h

LDSCRIPT ?= ${P}/stm32g0$(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),_bootloader,).ld

ENTRYPOINT ?= $(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),bl_start,start)

include ${SRC}/cpu/cortexm/cortexm0plus.mk

SRCS += ${C}/systick.c \
	${P}/stm32g0.c \
	${P}/stm32g0_clk.c \
	${P}/stm32g0_gpio.c \
	${P}/stm32g0_uart_stream.c \
	${P}/stm32g0_i2c.c \
	${P}/stm32g0_spi.c \
	${P}/stm32g0_crc.c \
	${P}/stm32g0_adc.c \
	${P}/stm32g0_info.c \
	${P}/stm32g0_idle.c \
	${P}/stm32g0_dma.c \
	${P}/stm32g0_flash.c \

SRCS-${ENABLE_NET_MBUS} += \
	${P}/stm32g0_uart_mbus.c

SRCS-${ENABLE_SYSTIM} += ${P}/stm32g0_systim.c

SRCS-${ENABLE_BUILTIN_BOOTLOADER} += \
	${P}/boot/stm32g0_bootloader.c \
	${P}/boot/isr.s \

${MOS}/platform/stm32g0/boot/stm32g0_bootloader.o : CFLAGS = -Os -ffreestanding -Wall -Werror -mcpu=cortex-m0plus -mthumb -include ${BOOTLOADER_DEFS}
