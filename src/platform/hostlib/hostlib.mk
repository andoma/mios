#
# "hostlib": build mios as a Linux shared object that a host harness loads
# (dlopen/dlmopen) and drives in lockstep virtual time. Same kernel/libc as
# every other target; the CPU layer is src/cpu/host with a cooperative
# library boot (host_lib_boot/step in cpu.c). All symbols are hidden except
# the sim ABI (hostlib.syms), so mios's own libc cannot clash with the
# host's glibc.
#
#   make PLATFORM=hostlib HOSTLIB_APP=/abs/path/to/app.c
#

ENABLE_TASK_DEBUG := yes

P := ${SRC}/platform/hostlib
C := ${SRC}/cpu/host

PLATFORM := hostlib
ARTIFACT := mios

GLOBALDEPS += ${P}/hostlib.mk ${C}/host.mk ${C}/host_shared.ld ${P}/hostlib.syms

CPPFLAGS += -iquote${C} -iquote${P}
CPPFLAGS += -include ${C}/host.h

TOOLCHAIN :=

# Position-independent, hidden by default. Same freestanding choices as the
# host exe (no stack protector / CET / stack-clash; psABI data alignment).
CFLAGS += -fPIC -fvisibility=hidden
CFLAGS += -fno-stack-protector -fcf-protection=none -fno-stack-clash-protection
CFLAGS += -malign-data=abi

FRAME_LIMIT := 1024

LDSCRIPT = ${C}/host_shared.ld
LD_NMAGIC :=
LDFLAGS += -shared -Wl,-z,noexecstack
LDFLAGS += -Wl,--version-script=${P}/hostlib.syms

SRCS += ${C}/entry.s \
	${C}/cpu.c \
	${C}/irq.c \
	${C}/timer.c \
	${C}/rnd.c \
	${C}/sim.c \

SRCS += ${P}/hostlib.c \
	${P}/console_lib.c \
	${P}/vi2c.c \
	${P}/libmios.c \

SRCS += ${HOSTLIB_APP}
