P := ${SRC}/platform/nrf52

GLOBALDEPS += ${P}/nrf52.mk

CPPFLAGS += -iquote${P} -include ${P}/nrf52.h

LDSCRIPT ?= ${P}/nrf52$(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),_bootloader,).ld

ENTRYPOINT ?= $(if $(subst no,,${ENABLE_BUILTIN_BOOTLOADER}),bl_start,start)

include ${SRC}/cpu/cortexm/cortexm4f.mk

# Nordic's SDC/MPSL binary libraries are built with AAPCS "small" enums; build
# the whole platform the same way so the ABI matches everywhere it links them.
CFLAGS += -fshort-enums

SRCS += ${C}/entry-xip.s \
	${P}/nrf52.c \
	${P}/nrf52_clk.c \
	${P}/nrf52_uart.c \
	${P}/nrf52_gpio.c \
	${P}/nrf52_rng.c \
	${P}/nrf52_spi.c \
	${P}/nrf52_systim.c \
	${P}/nrf52_rtc.c \
	${P}/nrf52_mbus_uart.c \
	${P}/nrf52_wdt.c \
	${P}/nrf52_adc.c \

# BLE via Nordic's SoftDevice Controller. SoC-independent HCI/l2cap glue comes
# from platform/nrf/nrf.mk; here we add the nRF52 hardware layer and link the
# nrf52 prebuilt libraries. The board picks the device (default nRF52840).
NRF_DEVICE ?= NRF52840_XXAA

ifeq (${ENABLE_NET_BLE},yes)

CPPFLAGS += -D${NRF_DEVICE}

SRCS += \
	${P}/nrf52_mpsl.c \
	${P}/nrf52_trng.c \

include ${SRC}/platform/nrf/nrf.mk

# Order matters: SDC needs MPSL and FEM symbols, FEM needs MPSL.
LDFLAGS += \
	${NRFXLIB}/softdevice_controller/lib/nrf52/hard-float/libsoftdevice_controller_${SDC_VARIANT}.a \
	${NRFXLIB}/mpsl/fem/common/lib/nrf52/hard-float/libmpsl_fem_common.a \
	${NRFXLIB}/mpsl/lib/nrf52/hard-float/libmpsl.a \

endif

SRCS-${ENABLE_BUILTIN_BOOTLOADER} += \
	${P}/boot/nrf52_bootloader.c \
	${P}/boot/isr.s \

${MOS}/platform/nrf52/boot/nrf52_bootloader.o : CFLAGS = -Os -ffreestanding -Wall -Werror -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mgeneral-regs-only -include ${BOOTLOADER_DEFS}

${MOS}/platform/nrf52/%.o : CFLAGS += ${NOFPU}
