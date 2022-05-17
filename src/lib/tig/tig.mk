GLOBALDEPS += ${SRC}/lib/tig/tig.mk

SRCS-${ENABLE_TIG} += \
	${SRC}/lib/tig/tig.c \

${MO}/src/lib/tig/%.o : CFLAGS += ${NOFPU}
