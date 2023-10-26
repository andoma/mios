GLOBALDEPS += ${SRC}/lib/rpc/rpc.mk

SRCS-${ENABLE_RPC} += \
	${SRC}/lib/rpc/rpc_core.c \
	${SRC}/lib/rpc/rpc_cbor.c \

