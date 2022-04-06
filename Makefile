T := $(dir $(lastword $(MAKEFILE_LIST)))

PLATFORM ?= lm3s811evb

O ?= build.${PLATFORM}

.DEFAULT_GOAL := ${O}/build.elf

-include local.mk

include $(dir $(abspath $(lastword $(MAKEFILE_LIST))))/mk/$(shell uname).mk

GLOBALDEPS += ${T}Makefile

SRC := ${T}src

MOS := ${O}/${T}src

#
# Include platform- (which in turn include CPU-) specific things
#
include ${SRC}/platform/${PLATFORM}/${PLATFORM}.mk

OPTLEVEL ?= 2

CFLAGS += -Wframe-larger-than=128

CFLAGS += ${CFLAGS-yes}

CFLAGS += -g3 -O${OPTLEVEL} -nostdinc -Wall -Werror -D__mios__

CPPFLAGS += -I${T}include -I${SRC} -I${O} -include ${O}/include/config.h

LDFLAGS += -nostartfiles -nodefaultlibs ${CFLAGS} -lgcc
CFLAGS += -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections -Wl,--build-id=sha1

# Needed for linker script includes
LDFLAGS += -L${SRC}


#
# Set defaults for all variables
#

ENABLE_MATH ?= yes
ENABLE_TASK_WCHAN ?= yes
ENABLE_TASK_DEBUG ?= no
ENABLE_TASK_ACCOUNTING ?= yes
ENABLE_NET_IPV4 ?= no
ENABLE_NET_MBUS ?= no
ENABLE_NET_PCS ?= no
ENABLE_OTA ?= no

ALL_ENABLE_VARS := $(filter ENABLE_%, $(.VARIABLES))

CONFIG_H := ${O}/include/config.h

#
# Core
#
include ${SRC}/kernel/kernel.mk
include ${SRC}/shell/shell.mk
include ${SRC}/lib/libc/libc.mk
include ${SRC}/lib/math/math.mk
include ${SRC}/lib/fixmath.mk
include ${SRC}/lib/usb/usb.mk
include ${SRC}/drivers/drivers.mk
include ${SRC}/net/net.mk
include ${SRC}/util/util.mk
include ${SRC}/gui/gui.mk

SRCS += ${SRC}/version.c

SRCS +=  ${SRCS-yes}
SRCS :=  $(sort $(SRCS))
OBJS +=  ${SRCS:%.c=${O}/%.o}
OBJS :=  ${OBJS:%.s=${O}/%.o}
DEPS +=  ${OBJS:%.o=%.d}

${O}/build.elf: ${OBJS} ${GLOBALDEPS} ${LDSCRIPT}
	@mkdir -p $(dir $@)
	@echo "\tLINK\t$@"
	${TOOLCHAIN}gcc -Wl,-T${LDSCRIPT} ${OBJS} -o $@ ${LDFLAGS}

${O}/build.bin: ${O}/build.elf
	@echo "\tBIN\t$@"
	${TOOLCHAIN}objcopy -O binary $< $@

${O}/%.o: %.c ${GLOBALDEPS} ${CONFIG_H}
	@mkdir -p $(dir $@)
	@echo "\tCC\t$<"
	${TOOLCHAIN}gcc -MD -MP ${CPPFLAGS} ${CFLAGS} -c $< -o $@

${O}/%.o: %.S ${GLOBALDEPS} ${CONFIG_H}
	@mkdir -p $(dir $@)
	@echo "\tASM\t$<"
	${TOOLCHAIN}gcc -MD -MP -DASM ${CPPFLAGS} ${CFLAGS} -c $< -o $@

CONFIG_H_CONTENTS := $(foreach K,$(ALL_ENABLE_VARS), \
	$(if $(subst no,,${${K}}),"\#define ${K}\n",""))

${CONFIG_H}: ${GLOBALDEPS}
	@mkdir -p $(dir $@)
	@echo "\tGEN\t$@"
	@echo >$@ ${CONFIG_H_CONTENTS}

clean:
	rm -rf "${O}"


GITVER_VARGUARD = $(1)_GUARD_$(shell echo $($(1)) $($(2)) $($(3)) | ${MD5SUM} | cut -d ' ' -f 1)

GIT_DESC_MIOS_OUTPUT ?= $(shell cd "$(T)" && git describe --always --dirty 2>/dev/null)
GIT_DESC_APP_OUTPUT  ?= $(shell git describe --always --dirty 2>/dev/null)

VERSION_DIGEST := $(call GITVER_VARGUARD,GIT_DESC_MIOS_OUTPUT,GIT_DESC_APP_OUTPUT,APPNAME)

${O}/version_git.h: ${O}/.version_git/${VERSION_DIGEST}
	echo >$@ "#define VERSION_MIOS_GIT \"${GIT_DESC_MIOS_OUTPUT}\""
	echo >>$@ "#define VERSION_APP_GIT \"${GIT_DESC_APP_OUTPUT}\""
	echo >>$@ "#define APPNAME \"${APPNAME}\""

${O}/.version_git/${VERSION_DIGEST}:
	rm -rf "${O}/.version_git"
	mkdir -p "${O}/.version_git"
	touch $@

${SRC}/version.c : ${O}/version_git.h

bin: ${O}/build.bin

run: build.lm3s811evb/build.elf
	qemu-system-arm -nographic -serial mon:stdio -machine lm3s811evb -cpu cortex-m4 -kernel $<

builtindefs:
	${TOOLCHAIN}gcc  ${CFLAGS} -dM -E - < /dev/null

include ${SRC}/platform/platforms.mk

-include ${DEPS}

$(V).SILENT:
