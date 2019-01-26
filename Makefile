O=build

TOOLCHAIN := arm-none-eabi-

CFLAGS += -mcpu=cortex-m4 -mthumb -g3 -O2  -fno-builtin
LDFLAGS += ${CFLAGS} -nostartfiles -nodefaultlibs -Wl,-Tsrc/linker.ld


SRCS += src/isr.s \
	src/init.c \
	src/main.c \

OBJS +=  ${SRCS:%.c=${O}/%.o}
OBJS :=  ${OBJS:%.s=${O}/%.o}
DEPS +=  ${OBJS:%.o=%.d}



${O}/build.elf: ${OBJS}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc ${LDFLAGS} ${OBJS} -o $@

${O}/%.o: %.c ${GLOBALDEPS}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc ${CFLAGS} -c $< -o $@

${O}/%.o: %.s ${GLOBALDEPS}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc ${CFLAGS} -c $< -o $@

clean:
	rm -rf "${O}"

-include ${DEPS}
