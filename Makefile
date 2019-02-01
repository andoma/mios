O=build

TOOLCHAIN := arm-none-eabi-

GLOBALDEPS := Makefile

CFLAGS += -mcpu=cortex-m4 -mthumb -g3 -Os -fno-builtin -nostdinc -Werror -Wall
CFLAGS += -I. -Iinclude
LDFLAGS += ${CFLAGS} -nostartfiles -nodefaultlibs -Wl,-Tsrc/linker.ld


SRCS += src/isr.s \
	src/init.c \
	src/main.c \
	src/task.c \
	src/timer.c \

SRCS += src/heap_simple.c

SRCS += ext/tinyprintf/tinyprintf.c

OBJS +=  ${SRCS:%.c=${O}/%.o}
OBJS :=  ${OBJS:%.s=${O}/%.o}
DEPS +=  ${OBJS:%.o=%.d}



${O}/build.elf: ${OBJS} ${GLOBALDEPS} src/linker.ld
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc ${LDFLAGS} ${OBJS} -o $@

${O}/%.o: %.c ${GLOBALDEPS}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc -MD -MP ${CFLAGS} -c $< -o $@

${O}/%.o: %.s ${GLOBALDEPS}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc ${CFLAGS} -c $< -o $@

clean:
	rm -rf "${O}"


${O}/build.bin: ${O}/build.elf
	${TOOLCHAIN}objcopy -O binary $< $@

bin: ${O}/build.bin

run: build/build.elf
	qemu-system-arm -nographic -serial mon:stdio -machine lm3s811evb -cpu cortex-m4 -kernel $@

-include ${DEPS}
