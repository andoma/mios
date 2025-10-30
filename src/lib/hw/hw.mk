GLOBALDEPS += ${SRC}/lib/hw/hw.mk

SRCS-${ENABLE_POWER_RAIL_API} += \
	${SRC}/lib/hw/power_rail.c

SRCS-${ENABLE_CLIMATE_ZONE_API} += \
	${SRC}/lib/hw/climate_zone.c
