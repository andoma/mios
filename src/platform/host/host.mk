#
# Mios as a Linux x86-64 process. Console on stdin/stdout.
#
#   make PLATFORM=host run
#

ENABLE_TASK_DEBUG := yes
ENABLE_PERFTEST := yes

P := ${SRC}/platform/host

GLOBALDEPS += ${P}/host.mk

CPPFLAGS += -iquote${P} -include ${P}/host.h

LDSCRIPT = ${SRC}/cpu/host/host.ld

include ${SRC}/cpu/host/host.mk

SRCS += ${P}/host.c \
	${P}/console.c \
	${P}/selftest.c \

