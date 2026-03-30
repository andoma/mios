ENABLE_LITTLEFS ?= no

GLOBALDEPS += ${SRC}/lib/littlefs.mk

SRCS-${ENABLE_LITTLEFS} += \
	${SRC}/lib/littlefs/lfs.c \
	${SRC}/lib/littlefs/lfs_util.c \
	${SRC}/lib/fs.c \
	${SRC}/lib/fs_stream.c \
	${SRC}/lib/fs_copy.c \

LITTLEFS_CFLAGS += -include ${SRC}/lib/littlefs_mios.h

${MOS}/lib/littlefs/%.o : CFLAGS += \
	-Wframe-larger-than=192 ${LITTLEFS_CFLAGS} ${NOFPU}

${MOS}/lib/fs.o : CFLAGS += ${LITTLEFS_CFLAGS} ${NOFPU}
${MOS}/lib/fs_stream.o : CFLAGS += ${NOFPU}
${MOS}/lib/fs_copy.o : CFLAGS += ${NOFPU}
