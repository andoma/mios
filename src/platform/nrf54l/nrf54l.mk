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
	${P}/nrf54l_spi.c \
	${P}/nrf54l_systim.c \
	${P}/nrf54l_wdt.c \
	${SRC}/shell/mcp_uart.c \

# BLE is Nordic's SoftDevice Controller + MPSL binary blobs (HCI boundary,
# gives DLE/Coded PHY/central/channel sounding). Our own link layer is gone.
# The SoC-independent HCI/l2cap glue comes from platform/nrf/nrf.mk; here we
# add the nRF54L hardware layer and link its prebuilt libraries.
CPPFLAGS += -DNRF54L15_XXAA

SRCS-${ENABLE_NET_BLE} += \
	${P}/nrf54l_mpsl.c \
	${P}/nrf54l_trng.c \

include ${SRC}/platform/nrf/nrf.mk

# Order matters: SDC needs MPSL and FEM symbols, FEM needs MPSL.
LDFLAGS += \
	${NRFXLIB}/softdevice_controller/lib/nrf54l/hard-float/libsoftdevice_controller_${SDC_VARIANT}.a \
	${NRFXLIB}/mpsl/fem/common/lib/nrf54l/hard-float/libmpsl_fem_common.a \
	${NRFXLIB}/mpsl/lib/nrf54l/hard-float/libmpsl.a \

${MOS}/platform/nrf54l/%.o : CFLAGS += ${NOFPU}
