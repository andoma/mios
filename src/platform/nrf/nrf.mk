# Shared SoftDevice Controller glue for the Nordic SoCs. The SoC .mk sets
# NRFXLIB, the device -D define, and the LDFLAGS for its prebuilt SDC/MPSL/FEM
# libraries; this fragment adds the SoC-independent source and include paths.

NP := ${SRC}/platform/nrf

NRFXLIB ?= ${T}vendor/nrf/sdk-nrfxlib

# multirole carries every SDC feature (central, coded PHY, extended adv,
# channel sounding, ISO); the linker strips what the build does not enable.
SDC_VARIANT ?= multirole

CPPFLAGS += \
	-iquote${NP} \
	-I${NP}/sdc_shim \
	-I${NRFXLIB}/softdevice_controller/include \
	-I${NRFXLIB}/mpsl/include

SRCS-${ENABLE_NET_BLE} += \
	${NP}/nrf_sdc.c \
