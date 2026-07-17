P := ${SRC}/platform/nrf54l

GLOBALDEPS += ${P}/nrf54l.mk

CPPFLAGS += -iquote${P} -include ${P}/nrf54l.h

LDSCRIPT ?= ${P}/nrf54l.ld

ENTRYPOINT ?= start

include ${SRC}/cpu/cortexm/cortexm33.mk

# Nordic's SDC/MPSL binary libraries are built with AAPCS "small" enums.
# Build the whole platform the same way so the ABI matches everywhere.
CFLAGS += -fshort-enums

SRCS += ${C}/entry-xip.s \
	${P}/nrf54l.c \
	${P}/nrf54l_uart.c \
	${P}/nrf54l_gpio.c \
	${P}/nrf54l_radio_core.c \
	${P}/nrf54l_spi.c \
	${P}/nrf54l_systim.c \
	${P}/nrf54l_wdt.c \
	${SRC}/shell/mcp_uart.c \

# BLE controller selection: ENABLE_NRF_SDC=yes links Nordic's SoftDevice
# Controller + MPSL binary blobs (HCI boundary, gives DLE/Coded PHY/central/
# channel sounding). Default is our own link layer.
ENABLE_NRF_SDC ?= no

ifeq (${ENABLE_NRF_SDC},yes)

NRFXLIB ?= ${T}vendor/nrf/sdk-nrfxlib
SDC_VARIANT ?= peripheral

CPPFLAGS += -DNRF54L15_XXAA \
	-I${P}/sdc_shim \
	-I${NRFXLIB}/softdevice_controller/include \
	-I${NRFXLIB}/mpsl/include

SRCS-${ENABLE_NET_BLE} += \
	${P}/nrf54l_mpsl.c \
	${P}/nrf54l_trng.c \
	${P}/nrf54l_sdc.c \

# Order matters: SDC needs MPSL and FEM symbols, FEM needs MPSL.
LDFLAGS += \
	${NRFXLIB}/softdevice_controller/lib/nrf54l/hard-float/libsoftdevice_controller_${SDC_VARIANT}.a \
	${NRFXLIB}/mpsl/fem/common/lib/nrf54l/hard-float/libmpsl_fem_common.a \
	${NRFXLIB}/mpsl/lib/nrf54l/hard-float/libmpsl.a \

else

SRCS-${ENABLE_NET_BLE} += \
	${P}/nrf54l_radio.c \

endif
${MOS}/platform/nrf54l/%.o : CFLAGS += ${NOFPU}
