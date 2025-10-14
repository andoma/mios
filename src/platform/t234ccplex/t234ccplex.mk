P := ${SRC}/platform/t234ccplex

GLOBALDEPS += ${P}/t234ccplex.mk

CFLAGS += -mcpu=cortex-a78ae

T234 = ${SRC}/platform/t234

CPPFLAGS += -iquote${P} -I${T234} -include ${P}/t234ccplex.h

LDSCRIPT = ${P}/t234ccplex.ld

include ${SRC}/cpu/aarch64/aarch64.mk

SRCS += ${P}/t234ccplex.c \
	${P}/t234ccplex_hsp.c \
	${P}/t234ccplex_ari.c \
	${P}/t234ccplex_bpmp.c \
	${P}/t234ccplex_qspi.c \
	${P}/t234ccplex_pcie.c \
	${P}/t234ccplex_xusb.c \
	${P}/t234ccplex_bootchain.c \
	${P}/t234ccplex_fdt.c \
	${P}/efiruntime.c \
	${P}/efiboot.c \
	${P}/smbios.c \
	${P}/asm.s \

SRCS += ${T234}/t234_bootflash.c \


ENABLE_TASK_ACCOUNTING := no
ENABLE_RTL8168 := yes
ENABLE_NET_IPV4 := yes
ENABLE_NET_HTTP := yes

${MOS}/src/platform/t234ccplex/efiruntime.o : CFLAGS += -fpic
