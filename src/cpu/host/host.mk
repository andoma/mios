#
# "host" CPU: run Mios as a regular Linux x86-64 process.
#
# The kernel, libc and everything above is compiled unchanged. This layer
# provides what the cortexm/aarch64/riscv64 layers provide on real silicon:
# context switching, an interrupt controller, a clock and a boot path.
#
# Interrupts are POSIX signals delivered to the single CPU thread. IRQ
# priority masking (basepri on Cortex-M) is a software level, so
# irq_forbid()/irq_permit() cost no syscalls. See irq.c for details.
#
# The binary is fully static and freestanding: Mios supplies libc, we
# supply the handful of raw syscalls needed (linux.h). No glibc anywhere.
#

C := ${SRC}/cpu/host

GLOBALDEPS += ${C}/host.mk ${C}/host.ld

CPPFLAGS += -iquote${C}
CPPFLAGS += -include ${SRC}/cpu/host/host.h

# Native toolchain
TOOLCHAIN :=

# Freestanding: no PIE, no stack protector (needs TLS canary we don't have),
# no CET (our context switch is incompatible with shadow stacks).
CFLAGS += -fno-pie -fno-stack-protector -fcf-protection=none -fno-stack-clash-protection

# x86-64 GCC bumps alignment of >=16 byte objects to 16 by default, which
# inserts padding between the entries of the linker section arrays
# (clicmd, udpinput, driver, ...) that the kernel walks by sizeof().
# Stick to the psABI's natural alignment like every other target.
CFLAGS += -malign-data=abi

# Host stack frames are wider than Cortex-M frames (64-bit spills).
# The 192 byte limit is tuned for the MCU targets, allow more here.
FRAME_LIMIT := 1024

LDFLAGS += -static -no-pie -Wl,-z,noexecstack

# Page-aligned segments please. The -n (nmagic) used for MCU targets
# collapses everything into one RWX segment.
LD_NMAGIC :=

ENTRYPOINT ?= _start
LDFLAGS += -e ${ENTRYPOINT}

SRCS += ${C}/entry.s \
	${C}/cpu.c \
	${C}/irq.c \
	${C}/timer.c \
	${C}/rnd.c \
	${C}/sim.c \

run: ${O}/${ARTIFACT}.elf
	${O}/${ARTIFACT}.elf

gdb: ${O}/${ARTIFACT}.full.elf
	gdb ${O}/${ARTIFACT}.full.elf

# Run every virtual-time test suite (see src/platform/host/hosttest.h).
# Realtime suites depend on wall-clock scheduling and are skipped; run
# them by hand with ${O}/${ARTIFACT}.elf <suite>.
test: ${O}/${ARTIFACT}.elf
	@set -e; for s in $$($< --list | grep -vE '^(Platform|pbuf|$$)' | grep -v '(realtime)'); do \
	  echo "==== $$s"; $< $$s; \
	done

.PHONY: run gdb test
