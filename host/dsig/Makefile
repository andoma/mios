#
# Builds the host-side DSIG + VLLP library and a 'dsig' CLI tool for
# subscribing to and publishing DSIG signals (default transport: UDP
# multicast, matching src/net/dsig_udp.c on the guest).
#
# Usage:
#   make                  # build libdsig.a and the 'dsig' binary
#   make clean
#

O := build
CC := gcc
CFLAGS := -O2 -g -Wall -Werror -I. -I.. $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0)

# vllp_ota.c needs mios_image.c, which lives one level up (shared with
# host/flash and host/mcp).
VPATH := ..

LIB_SRCS := \
	dsig.c \
	dsig_udp.c \
	dsig_unix.c \
	dsig_cansock.c \
	dsig_usb.c \
	dsig_vllp.c \
	vllp.c \
	vllp_alertstream.c \
	vllp_logstream.c \
	vllp_ota.c \
	vllp_rpc.c \
	vllp_telnetd.c \
	vllp_term.c \
	mios_image.c

LIB_OBJS := $(LIB_SRCS:%.c=$(O)/%.o)

LIB := $(O)/libdsig.a

TOOL := $(O)/dsig
TOOL_OBJS := $(O)/dsig_tool.o

all: $(TOOL)

$(O):
	mkdir -p $(O)

$(O)/%.o: %.c | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

$(TOOL): $(TOOL_OBJS) $(LIB)
	$(CC) $(TOOL_OBJS) $(LIB) -lpthread $(LIBUSB_LIBS) -o $@

clean:
	rm -rf $(O)

.PHONY: all clean
