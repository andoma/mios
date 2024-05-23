P := ${SRC}/platform/nrf52

GLOBALDEPS += ${P}/nrf52.mk

CPPFLAGS += -iquote${P} -include ${P}/nrf52.h

LDSCRIPT ?= ${P}/nrf52$(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),_bootloader,).ld

ENTRYPOINT ?= $(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),bl_start,start)

include ${SRC}/cpu/cortexm/cortexm4f.mk

SRCS += ${P}/nrf52.c \
	${P}/nrf52_clk.c \
	${P}/nrf52_uart.c \
	${P}/nrf52_gpio.c \
	${P}/nrf52_rng.c \
	${P}/nrf52_spi.c \
	${P}/nrf52_systim.c \
	${P}/nrf52_rtc.c \
	${P}/nrf52_mbus_uart.c \
	${P}/nrf52_flash.c \
	${P}/nrf52_wdt.c \
	${P}/nrf52_adc.c \

SRCS-${ENABLE_NET_BLE} += \
	${P}/nrf52_radio.c \

SRCS-${ENABLE_BUILTIN_BOOTLOADER} += \
	${P}/boot/nrf52_bootloader.c \
	${P}/boot/isr.s \

${MOS}/platform/nrf52/boot/nrf52_bootloader.o : CFLAGS = -Os -ffreestanding -Wall -Werror -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mgeneral-regs-only -include ${BOOTLOADER_DEFS}

${MOS}/platform/nrf52/%.o : CFLAGS += ${NOFPU}
