P := ${SRC}/platform/tegra234-ccplex

GLOBALDEPS += ${P}/tegra234-ccplex.mk

CFLAGS += -mcpu=cortex-a78ae

CPPFLAGS += -iquote${P} -I${SRC}/platform/tegra234 -include ${P}/tegra234-ccplex.h

LDSCRIPT = ${P}/tegra234-ccplex.ld

include ${SRC}/cpu/aarch64/aarch64.mk

SRCS += ${P}/tegra234-ccplex.c \
	${P}/tegra234-ccplex_hsp.c \

ENABLE_TASK_ACCOUNTING := no
