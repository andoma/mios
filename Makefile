T := $(dir $(lastword $(MAKEFILE_LIST)))

UNAME_S := $(shell uname -s)

-include local.mk

PLATFORM ?= lm3s811evb

O ?= build.${PLATFORM}

.DEFAULT_GOAL := ${O}/build.elf

include $(dir $(abspath $(lastword $(MAKEFILE_LIST))))/mk/$(shell uname).mk

GLOBALDEPS += ${T}Makefile

SRC := ${T}src

MOS := ${O}/${T}src

#
# Include platform- (which in turn include CPU-) specific things
#
include ${SRC}/platform/${PLATFORM}/${PLATFORM}.mk

OPTLEVEL ?= 2

CFLAGS += -Wframe-larger-than=192

CFLAGS += ${CFLAGS-yes}

CFLAGS += -g3 -O${OPTLEVEL} -nostdinc -Wall -Werror -D__mios__

CPPFLAGS += -I${T}include -I${SRC} -I${O} -I${O}/include -include ${O}/include/config.h

LDFLAGS += -nostartfiles -nodefaultlibs ${CFLAGS} -lgcc
CFLAGS += -ffunction-sections -fdata-sections -Wno-attributes
LDFLAGS += -Wl,--gc-sections -Wl,--build-id=sha1

# Needed for linker script includes
LDFLAGS += -L${SRC}


#
# Set defaults for all variables
#

ENABLE_SYSTIM ?= no
ENABLE_MATH ?= yes
ENABLE_TASK_WCHAN ?= yes
ENABLE_TASK_DEBUG ?= no
ENABLE_TASK_ACCOUNTING ?= yes
ENABLE_NET_IPV4 ?= no
ENABLE_NET_MBUS ?= no
ENABLE_NET_MBUS_GW ?= no
ENABLE_NET_BLE ?= no
ENABLE_NET_CAN ?= no
ENABLE_NET_FPU_USAGE ?= no
ENABLE_METRIC ?= no
ENABLE_BUILTIN_BOOTLOADER ?= no
ENABLE_NET_TIMESTAMPING ?= no

CONFIG_H := ${O}/include/config.h

#
# Core
#
include ${SRC}/kernel/kernel.mk
include ${SRC}/shell/shell.mk
include ${SRC}/lib/crypto/crypto.mk
include ${SRC}/lib/libc/libc.mk
include ${SRC}/lib/math/math.mk
include ${SRC}/lib/fixmath.mk
include ${SRC}/lib/littlefs.mk
include ${SRC}/lib/usb/usb.mk
include ${SRC}/lib/gui/gui.mk
include ${SRC}/lib/tig/tig.mk
include ${SRC}/lib/metric/metric.mk
include ${SRC}/lib/diag/diag.mk
include ${SRC}/lib/rpc/rpc.mk
include ${SRC}/drivers/drivers.mk
include ${SRC}/net/net.mk
include ${SRC}/util/util.mk

include ${T}/support/version/gitver.mk

ALL_ENABLE_VARS := $(filter ENABLE_%, $(.VARIABLES))

SRCS += ${SRC}/version.c

SRCS +=  ${SRCS-yes}
SRCS +=  ${SRCS-yes-yes}
SRCS :=  $(sort $(SRCS))
OBJS +=  ${SRCS:%.c=${O}/%.o}
OBJS :=  ${OBJS:%.s=${O}/%.o}
DEPS +=  ${OBJS:%.o=%.d}


${O}/build.elf: ${OBJS} ${GLOBALDEPS} ${LDSCRIPT} ${MIOSVER} ${APPVER}
	@mkdir -p $(dir $@)
	@echo "\tLINK\t$@"
	${TOOLCHAIN}gcc -Wl,-T${LDSCRIPT} ${OBJS} -o $@ ${LDFLAGS}
	${TOOLCHAIN}objcopy --update-section .miosversion=${MIOSVER} $@
ifdef APPNAME
	${TOOLCHAIN}objcopy --update-section .appversion=${APPVER} $@
endif

${O}/build.bin: ${O}/build.elf
	@echo "\tBIN\t$@"
	${TOOLCHAIN}objcopy -O binary $< $@

${O}/%.o: %.c ${GLOBALDEPS} ${CONFIG_H} ${CDEPS} | toolchain
	@mkdir -p $(dir $@)
	@echo "\tCC\t$<"
	${TOOLCHAIN}gcc -MD -MP ${CPPFLAGS} ${CFLAGS} -c $< -o $@

${O}/%.o: %.S ${GLOBALDEPS} ${CONFIG_H} | toolchain
	@mkdir -p $(dir $@)
	@echo "\tASM\t$<"
	${TOOLCHAIN}gcc -MD -MP -DASM ${CPPFLAGS} ${CFLAGS} -c $< -o $@

HASH := \#

CONFIG_H_CONTENTS := $(foreach K,$(ALL_ENABLE_VARS), \
	$(if $(subst no,,${${K}}),"${HASH}define ${K}\n",""))

${CONFIG_H}: ${GLOBALDEPS}
	@mkdir -p $(dir $@)
	@echo "\tGEN\t$@"
	@echo >$@ ${CONFIG_H_CONTENTS}
	@echo >>$@ "#define APPNAME \"${APPNAME}\""

clean::
	rm -rf "${O}"

bin: ${O}/build.bin

toolchain:
	@which >/dev/null ${TOOLCHAIN}gcc || \
	(echo "\nERROR: Toolchain misconfigured, \
	${TOOLCHAIN}gcc is not in path\n"  && exit 1)

builtindefs:
	${TOOLCHAIN}gcc  ${CFLAGS} -dM -E - < /dev/null

include ${SRC}/platform/platforms.mk

-include ${DEPS}

$(V).SILENT:
