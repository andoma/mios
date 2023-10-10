GLOBALDEPS += ${SRC}/lib/crypto/crypto.mk

SRCS +=	\
	${SRC}/lib/crypto/sha1.c

${MO}/src/lib/crypto/%.o : CFLAGS += ${NOFPU}
