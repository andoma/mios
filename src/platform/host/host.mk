#
# Mios as a native Linux process (x86-64 or aarch64). Console on
# stdin/stdout, Ethernet via passt (unprivileged user-mode networking,
# spawned automatically when found in PATH).
#
#   make PLATFORM=host run
#   build.host/mios.elf --no-net
#   build.host/mios.elf -- -t 2323:23      (passt args: forward telnet)
#

ENABLE_TASK_DEBUG := yes
ENABLE_PERFTEST := yes
ENABLE_NET_IPV4 := yes
ENABLE_NET_DSIG_UDP := yes
ENABLE_NET_CAN := yes

# Virtual consoles. No in-tree target enabled these before, so the host
# suites are their only coverage.
ENABLE_VCON := yes

# The VLLP client role, exercised by suite_vllp_client.c and
# suite_vcon_vllp.c.
ENABLE_VLLP_CLIENT := yes

P := ${SRC}/platform/host

GLOBALDEPS += ${P}/host.mk

# Buffer size picked at startup instead of compiled in, so one binary can
# run each test suite at the pool size that suite cares about. Host only;
# see the rationale in src/net/pbuf.h.
ENABLE_PBUF_DYNAMIC_SIZE := yes

# Let a test force buffer-allocation failures; see src/net/pbuf.h.
ENABLE_PBUF_FAULT_INJECT := yes

CPPFLAGS += -iquote${P} -include ${P}/host.h

LDSCRIPT = ${SRC}/cpu/host/host.ld

include ${SRC}/cpu/host/host.mk

SRCS += ${P}/host.c \
	${P}/console.c \
	${P}/selftest.c \
	${P}/passt.c \
	${P}/hostnet.c \
	${P}/hosttest.c \
	${P}/suite_snprintf.c \
	${P}/vnet.c \
	${P}/vcan.c \
	${P}/vcan_loop.c \
	${P}/testterm.c \
	${P}/sim_dhcpd.c \
	${P}/suite_dhcp.c \
	${P}/suite_vcon.c \
	${P}/suite_vcon_vllp.c \
	${P}/suite_vllp.c \
	${P}/suite_vllp_frames.c \
	${P}/suite_vllp_client.c \
	${P}/suite_vllp_xcheck.c \
	${P}/suite_ota.c \
	${P}/vspiflash.c \
	${P}/host_ota.c \
	${T}host/dsig/vllp.c \


# The real host VLLP client, compiled into the test binary in single-
# threaded virtual-time mode (VLLP_SIM). Lets suites exercise the actual
# production host stack against the guest server.
${O}/${T}host/dsig/vllp.o : CFLAGS += -DVLLP_SIM -iquote${T}host/dsig
