#
# Mios as a Linux x86-64 process. Console on stdin/stdout, Ethernet via
# passt (unprivileged user-mode networking, spawned automatically when
# found in PATH).
#
#   make PLATFORM=host run
#   build.host/mios.elf --no-net
#   build.host/mios.elf -- -t 2323:23      (passt args: forward telnet)
#

ENABLE_TASK_DEBUG := yes
ENABLE_PERFTEST := yes
ENABLE_NET_IPV4 := yes
ENABLE_NET_DSIG_UDP := yes

P := ${SRC}/platform/host

GLOBALDEPS += ${P}/host.mk

CPPFLAGS += -iquote${P} -include ${P}/host.h

LDSCRIPT = ${SRC}/cpu/host/host.ld

include ${SRC}/cpu/host/host.mk

SRCS += ${P}/host.c \
	${P}/console.c \
	${P}/selftest.c \
	${P}/passt.c \

