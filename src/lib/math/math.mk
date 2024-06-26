ENABLE_MATH ?= yes

GLOBALDEPS += ${SRC}/lib/math/math.mk

SRCS-${ENABLE_MATH} += \
	${SRC}/lib/math/trig.c \
	${SRC}/lib/math/powf.c \
	${SRC}/lib/math/sqrtf.c \
	${SRC}/lib/math/asinf.c \
	${SRC}/lib/math/atanf.c \
	${SRC}/lib/math/atan2f.c \
	${SRC}/lib/math/fmodf.c \
	${SRC}/lib/math/math_diag.c \
