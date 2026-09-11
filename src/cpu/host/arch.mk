#
# Machine-specific bits of the host CPU layer, shared by the two
# packagings (host.mk: static executable, hostlib.mk: shared object).
#
# Only the thin layer that talks to the hardware and the kernel ABI is
# per-machine: context switch and process entry (entry_${arch}.S),
# switch frame construction and register dumps (arch_${arch}.c), and
# the syscall numbers / signal frame layout (linux_${arch}.h).
#

HOSTARCH := $(shell uname -m)

ifeq (${HOSTARCH},x86_64)

# Our context switch is incompatible with shadow stacks
CFLAGS += -fcf-protection=none

# x86-64 GCC bumps alignment of >=16 byte objects to 16 by default, which
# inserts padding between the entries of the linker section arrays
# (clicmd, udpinput, driver, ...) that the kernel walks by sizeof().
# Stick to the psABI's natural alignment like every other target.
CFLAGS += -malign-data=abi

else ifeq (${HOSTARCH},aarch64)

# No pointer authentication or branch target identification: threads are
# entered through frames we build by hand (arch_aarch64.c), which the
# checks would reject. Distro GCCs default this to "standard".
CFLAGS += -mbranch-protection=none

# libgcc's out-of-line atomics probe for LSE through glibc's __getauxval,
# which a -nodefaultlibs link cannot resolve. Inline the atomics instead.
CFLAGS += -mno-outline-atomics

else

$(error host platform: unsupported machine "${HOSTARCH}", need x86_64 or aarch64)

endif

GLOBALDEPS += ${C}/arch.mk ${C}/linux_${HOSTARCH}.h

SRCS += ${C}/entry_${HOSTARCH}.s \
	${C}/arch_${HOSTARCH}.c \

