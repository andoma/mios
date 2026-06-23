GLOBALDEPS += ${SRC}/lib/diag/diag.mk

SRCS-${ENABLE_SIGCAPTURE} += \
	${SRC}/lib/diag/sigcapture.c

SRCS-${ENABLE_SIGCAPTURE_VLLP} += \
	${SRC}/lib/diag/sigcapture_vllp.c
