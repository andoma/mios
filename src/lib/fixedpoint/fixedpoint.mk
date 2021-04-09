ENABLE_FIXEDPOINT ?= no

GLOBALDEPS += ${SRC}/lib/fixedpoint/fixedpoint.mk

SRCS-${ENABLE_FIXEDPOINT} += \
	${SRC}/lib/fixedpoint/q16_trig.c \
