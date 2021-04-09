PLATFORM ?= lm3s811evb

O ?= build.${PLATFORM}

OPTLEVEL ?= 2

T := $(shell realpath --relative-to ${CURDIR} $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

GLOBALDEPS += ${T}/Makefile

SRC := ${T}/src

MO := ${O}/${T}

#
# Include platform- (which in turn include CPU-) specific things
#
include ${SRC}/platform/${PLATFORM}/${PLATFORM}.mk

CFLAGS += -Wframe-larger-than=128

CFLAGS += -g3 -O${OPTLEVEL} -nostdinc -Wall -Werror -D__mios__

CPPFLAGS += -I${T}/include -I${SRC} -include ${O}/include/config.h

LDFLAGS += -nostartfiles -nodefaultlibs ${CFLAGS} -lgcc
CFLAGS += -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections

# Needed for linker script includes
LDFLAGS += -L${SRC}


#
# Set defaults for all variables
#

ENABLE_MATH ?= yes
ENABLE_TASK_WCHAN ?= yes
ENABLE_TASK_DEBUG ?= no
ENABLE_TASK_ACCOUNTING ?= yes
ENABLE_NET ?= no

ALL_ENABLE_VARS := $(filter ENABLE_%, $(.VARIABLES))

CONFIG_H := ${O}/include/config.h

#
# Core
#
include ${SRC}/kernel/kernel.mk
include ${SRC}/shell/shell.mk
include ${SRC}/lib/libc/libc.mk
include ${SRC}/lib/math/math.mk
include ${SRC}/drivers/drivers.mk
include ${SRC}/net/net.mk
include ${SRC}/util/util.mk

SRCS +=  ${SRCS-yes}
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



bin: ${O}/build.bin

run: build.lm3s811evb/build.elf
	qemu-system-arm -nographic -serial mon:stdio -machine lm3s811evb -cpu cortex-m4 -kernel $<

builtindefs:
	${TOOLCHAIN}gcc  ${CFLAGS} -dM -E - < /dev/null

stlink: ${O}/build.elf
	gdb-multiarch -ex "target extended-remote localhost:3333" $<

include ${SRC}/platform/platforms.mk

-include ${DEPS}

$(V).SILENT:
