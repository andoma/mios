GLOBALDEPS += ${SRC}/gui/gui.mk

SRCS-${ENABLE_GUI} += \
	${SRC}/gui/gui.c \

${MO}/src/gui/%.o : CFLAGS += ${NOFPU}
