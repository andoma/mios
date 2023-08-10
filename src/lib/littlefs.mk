ENABLE_LITTLEFS ?= no

GLOBALDEPS += ${SRC}/lib/littlefs.mk

SRCS-${ENABLE_LITTLEFS} += \
	${SRC}/lib/littlefs/lfs.c \
	${SRC}/lib/littlefs/lfs_util.c \
	${SRC}/lib/fs.c \

LITTLEFS_CFLAGS += -include ${SRC}/lib/littlefs_mios.h

${MOS}/lib/littlefs/%.o : CFLAGS += \
	-Wframe-larger-than=192 ${LITTLEFS_CFLAGS} ${NOFPU}

${MOS}/lib/fs.o : CFLAGS += ${LITTLEFS_CFLAGS} ${NOFPU}
