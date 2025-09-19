GLOBALDEPS += ${SRC}/lib/crypto/crypto.mk

SRCS +=	\
	${SRC}/lib/crypto/sha1.c \
	${SRC}/lib/crypto/sha512.c

${MOS}/lib/crypto/%.o : CFLAGS += ${NOFPU} -Wno-frame-larger-than

