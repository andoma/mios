GLOBALDEPS += ${SRC}/lib/crypto/crypto.mk

SRCS +=	\
	${SRC}/lib/crypto/sha1.c \
	${SRC}/lib/crypto/sha512.c \
	${SRC}/lib/crypto/aes.c

# BLE LE Secure Connections crypto: AES-CMAC and P-256 ECDH (micro-ecc).
SRCS-${ENABLE_NET_BLE} += \
	${SRC}/lib/crypto/aes_cmac.c \
	${SRC}/lib/crypto/micro-ecc/uECC.c

${MOS}/lib/crypto/%.o : CFLAGS += ${NOFPU} -Wno-frame-larger-than

# micro-ecc: P-256 only, C implementation (no asm), quiet its warnings.
${MOS}/lib/crypto/micro-ecc/%.o : CFLAGS += \
	-I${SRC}/lib/crypto/micro-ecc -DuECC_ASM=0 \
	-DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 \
	-DuECC_SUPPORTS_secp224r1=0 -DuECC_SUPPORTS_secp256r1=1 \
	-DuECC_SUPPORTS_secp256k1=0 -Wno-frame-larger-than -w

