GLOBALDEPS += ${SRC}/lib/gui/gui.mk

SRCS-${ENABLE_GUI} += \
	${SRC}/lib/gui/gui.c \

${MO}/src/lib/gui/%.o : CFLAGS += ${NOFPU}
