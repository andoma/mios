PLATFORM ?= lm3s811evb


O ?= build.${PLATFORM}

T := $(shell realpath --relative-to ${CURDIR} $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

GLOBALDEPS += ${T}/Makefile

SRC := ${T}/src

#
# Include platform- (which in turn include CPU-) specific things
#
P := ${T}/src/platform/${PLATFORM}

GLOBALDEPS += ${P}/${PLATFORM}.mk
CPPFLAGS += -I${P}

include ${P}/${PLATFORM}.mk

CFLAGS += -g3 -Os -nostdinc -Wall -fno-builtin -Werror

CPPFLAGS += -I${SRC}/platform -I${SRC}/cpu -I${T}/include -I${SRC}

LDFLAGS += -nostartfiles -nodefaultlibs ${CFLAGS} -lgcc

#
# Core
#
SRCS += ${SRC}/init.c \
	${SRC}/main.c \
	${SRC}/task.c \
	${SRC}/libc.c \
	${SRC}/stdio.c \

SRCS += ${SRC}/heap_simple.c

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
