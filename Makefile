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

CPPFLAGS += -I${T}/include -I${SRC}

LDFLAGS += -nostartfiles -nodefaultlibs ${CFLAGS} -lgcc

#
# Core
#
include ${SRC}/kernel/kernel.mk
include ${SRC}/shell/shell.mk
include ${SRC}/lib/libc/libc.mk
include ${SRC}/lib/math/math.mk
include ${SRC}/drivers/drivers.mk
include ${SRC}/net/net.mk

OBJS +=  ${SRCS:%.c=${O}/%.o}
OBJS :=  ${OBJS:%.s=${O}/%.o}
DEPS +=  ${OBJS:%.o=%.d}



${O}/build.elf: ${OBJS} ${GLOBALDEPS} ${LDSCRIPT}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc -Wl,-T${LDSCRIPT} ${OBJS} -o $@ ${LDFLAGS}

${O}/%.o: %.c ${GLOBALDEPS}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc -MD -MP ${CPPFLAGS} ${CFLAGS} -c $< -o $@

${O}/%.o: %.S ${GLOBALDEPS}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc -MD -MP ${CPPFLAGS} ${CFLAGS} -c $< -o $@

clean:
	rm -rf "${O}"


${O}/build.bin: ${O}/build.elf
	${TOOLCHAIN}objcopy -O binary $< $@

bin: ${O}/build.bin

run: build.lm3s811evb/build.elf
	qemu-system-arm -nographic -serial mon:stdio -machine lm3s811evb -cpu cortex-m4 -kernel $<

-include ${DEPS}
