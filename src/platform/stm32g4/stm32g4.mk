P := ${SRC}/platform/stm32g4

GLOBALDEPS += ${P}/stm32g4.mk

CPPFLAGS += -iquote${P} -include stm32g4.h

LDSCRIPT ?= ${P}/stm32g4$(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),_bootloader,).ld

ENTRYPOINT ?= $(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),bl_start,start)

include ${SRC}/cpu/cortexm/cortexm4f.mk

SRCS += ${C}/systick.c \
	${P}/stm32g4.c \
	${P}/stm32g4_idle.c \
	${P}/stm32g4_clk.c \
	${P}/stm32g4_gpio.c \
	${P}/stm32g4_uart_stream.c \
	${P}/stm32g4_uart_mbus.c \
	${P}/stm32g4_spi.c \
	${P}/stm32g4_dma.c \
	${P}/stm32g4_i2c.c \
	${P}/stm32g4_usb.c \
	${P}/stm32g4_adc.c \
	${P}/stm32g4_systim.c \
	${P}/stm32g4_crc.c \
	${P}/stm32g4_info.c \
	${P}/stm32g4_can.c \
	${P}/stm32g4_rtc.c \
	${P}/stm32g4_opt.c \

${MO}/src/platform/stm32g4/%.o : CFLAGS += ${NOFPU}

SRCS-${ENABLE_BUILTIN_BOOTLOADER} += \
	${P}/boot/stm32g4_bootloader.c \
	${P}/boot/isr.s \

${MOS}/platform/stm32g4/boot/stm32g4_bootloader.o : CFLAGS = -Os -ffreestanding -Wall -Werror -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mgeneral-regs-only -include ${BOOTLOADER_DEFS}
