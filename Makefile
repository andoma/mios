PLATFORM ?= lm3s811evb


O ?= build.${PLATFORM}

OPTLEVEL ?= 2

T := $(shell realpath --relative-to ${CURDIR} $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

GLOBALDEPS += ${T}/Makefile

SRC := ${T}/src

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
SRCS += ${SRC}/mios.c \
	${SRC}/io.c \
	${SRC}/task.c \
	${SRC}/libc.c \
	${SRC}/stdio.c \
	${SRC}/cli.c \
	${SRC}/monitor.c \

SRCS += ${SRC}/lib/math/trig.c \
	${SRC}/lib/math/powf.c \
	${SRC}/lib/math/sqrtf.c \
	${SRC}/lib/math/math_diag.c \


SRCS += ${SRC}/heap_simple.c

SRCS += ${SRC}/drivers/ms5611.c
SRCS += ${SRC}/drivers/mpu9250.c

SRCS += ${SRC}/drivers/sx1280/sx1280.c \
	${SRC}/drivers/sx1280/sx1280_tdma.c \
	${SRC}/drivers/sx1280_mios.c

OBJS +=  ${SRCS:%.c=${O}/%.o}
OBJS :=  ${OBJS:%.s=${O}/%.o}
DEPS +=  ${OBJS:%.o=%.d}



${O}/build.elf: ${OBJS} ${GLOBALDEPS} ${LDSCRIPT}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc -Wl,-T${LDSCRIPT} ${OBJS} -o $@ ${LDFLAGS}

${O}/%.o: %.c ${GLOBALDEPS}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc -MD -MP ${CPPFLAGS} ${CFLAGS} -c $< -o $@

${O}/%.o: %.s ${GLOBALDEPS}
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
