GLOBALDEPS += ${SRC}/kernel/kernel.mk

SRCS += ${SRC}/kernel/mios.c \
	${SRC}/kernel/io.c \
	${SRC}/kernel/task.c \
	${SRC}/kernel/device.c \
	${SRC}/kernel/driver.c \
	${SRC}/kernel/timer.c \
	${SRC}/kernel/eventlog.c \
	${SRC}/kernel/panic.c \

SRCS-${ENABLE_PROFILE} += ${SRC}/kernel/profile.c

# When profiling, bump function alignment to the bucket size so each
# bucket holds at most one function's prologue — keeps addr2line honest
# at bucket boundaries (no literal-pool aliasing into the next function).
ifeq ($(ENABLE_PROFILE),yes)
CFLAGS += -falign-functions=16
endif

${MOS}/kernel/%.o : CFLAGS += ${NOFPU}
