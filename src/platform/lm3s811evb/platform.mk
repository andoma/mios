CPU := cortexm

C := ${SRC}/cpu/${CPU}
include ${C}/${CPU}.mk

SRCS += ${P}/platform.c \
	${P}/console.c \

