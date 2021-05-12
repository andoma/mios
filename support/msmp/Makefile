
SRCS = msmp.c hdlc.c

CFLAGS += -O2 -Wall -Werror -I.

msmp: ${SRCS} Makefile
	$(CC) ${CFLAGS} -o $@ ${SRCS} -lz -lpthread
