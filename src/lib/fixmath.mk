ENABLE_FIXMATH ?= no

GLOBALDEPS += ${SRC}/lib/fixmath.mk

SRCS-${ENABLE_FIXMATH} += \
	${SRC}/lib/libfixmath/libfixmath/fix16.c \
	${SRC}/lib/libfixmath/libfixmath/fix16_exp.c \
	${SRC}/lib/libfixmath/libfixmath/fix16_sqrt.c \
	${SRC}/lib/libfixmath/libfixmath/fix16_str.c \
	${SRC}/lib/libfixmath/libfixmath/fix16_trig.c \

CFLAGS-${ENABLE_FIXMATH} += -I${SRC}/lib/libfixmath/libfixmath

${MOS}/lib/libfixmath/libfixmath/%.o : CFLAGS += -DFIXMATH_NO_CACHE
