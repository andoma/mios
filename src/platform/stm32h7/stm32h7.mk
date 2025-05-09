P := ${SRC}/platform/stm32h7

GLOBALDEPS += ${P}/stm32h7.mk

CPPFLAGS += -iquote${P} -include ${P}/stm32h7.h

LDSCRIPT ?= ${P}/stm32h7$(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),_bootloader,).ld

ENTRYPOINT ?= $(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),bl_start,start)

include ${SRC}/cpu/cortexm/cortexm7.mk

SRCS += ${C}/systick.c \
	${P}/stm32h7.c \
	${P}/stm32h7_clk.c \
	${P}/stm32h7_crc.c \
	${P}/stm32h7_gpio.c \
	${P}/stm32h7_uart.c \
	${P}/stm32h7_i2c.c \
	${P}/stm32h7_dma.c \
	${P}/stm32h7_usb.c \
	${P}/stm32h7_spi.c \
	${P}/stm32h7_adc.c \
	${P}/stm32h7_can.c \
	${P}/stm32h7_systim.c \
	${P}/stm32h7_idle.c \
	${P}/stm32h7_info.c \
	${P}/stm32h7_flash.c \

SRCS-${ENABLE_NET_IPV4} += \
	${P}/stm32h7_eth.c \


${MOS}/platform/stm32h7/%.o : CFLAGS += ${NOFPU}

SRCS-${ENABLE_BUILTIN_BOOTLOADER} += \
	${P}/boot/stm32h7_bootloader.c \
	${P}/boot/isr.s \

${MOS}/platform/stm32h7/boot/stm32h7_bootloader.o : CFLAGS = -Os -ffreestanding -Wall -Werror -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -mgeneral-regs-only -include ${BOOTLOADER_DEFS}
