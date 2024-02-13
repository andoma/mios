P := ${SRC}/platform/stm32f4

GLOBALDEPS += ${P}/stm32f4.mk

CPPFLAGS += -iquote${P} -include ${P}/stm32f4.h

LDSCRIPT = ${P}/stm32f4$(if $(subst no,,${ENABLE_OTA}),_ota,).ld
ENTRYPOINT = $(if $(subst no,,${ENABLE_OTA}),bl_,)start

include ${SRC}/cpu/cortexm/cortexm4f.mk

SRCS += ${C}/systick.c \
	${P}/stm32f4.c \
	${P}/stm32f4_info.c \
	${P}/stm32f4_gpio.c \
	${P}/stm32f4_iwdog.c \
	${P}/stm32f4_i2c.c \
	${P}/stm32f4_spi.c \
	${P}/stm32f4_dma.c \
	${P}/stm32f4_clk.c \
	${P}/stm32f4_uart_stream.c \
	${P}/stm32f4_flash.c \
	${P}/stm32f4_adc.c \
	${P}/stm32f4_otgfs.c \
	${P}/stm32f4_systim.c \

SRCS-${ENABLE_NET_IPV4} += \
	${P}/stm32f4_eth.c \

SRCS-${ENABLE_NET_MBUS} += \
	${P}/stm32f4_uart_mbus.c

SRCS-${ENABLE_OTA} += \
	${P}/boot/isr.s \
	${P}/boot/bootloader.c \

${MOS}/platform/stm32f4/%.o : CFLAGS += ${NOFPU}

${MOS}/platform/stm32f4/boot/bootloader.o : CFLAGS = -Os -ffreestanding -Wall -Werror -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -include ${BOOTLOADER_DEFS}

${P}/boot/bootloader.c : ${O}/version_git.h
