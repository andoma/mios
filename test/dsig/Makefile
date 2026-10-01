#
# Tests for the host DSIG bus (host/dsig/dsig.c).
#
#   make            build dsig_test
#   make run        build and run every case
#   make asan       build and run under AddressSanitizer
#
#   ARGS="unsub_from_own_callback" make run     run one case
#

TOP  := ../..
DSIG := $(TOP)/host/dsig
O    := build

CC := gcc
CFLAGS := -O2 -g -Wall -Werror -I$(DSIG) -pthread
LDFLAGS := -pthread

# Only the bus itself, so no libusb and no transports
SRCS := dsig_test.c $(DSIG)/dsig.c

BIN := $(O)/dsig_test

all: $(BIN)

$(O):
	mkdir -p $(O)

$(BIN): $(SRCS) $(DSIG)/dsig.h | $(O)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

$(O)/dsig_test_asan: $(SRCS) $(DSIG)/dsig.h | $(O)
	$(CC) $(CFLAGS) -O1 -fsanitize=address -fno-omit-frame-pointer \
	  $(SRCS) $(LDFLAGS) -fsanitize=address -o $@

run: $(BIN)
	$(BIN) $(ARGS)

asan: $(O)/dsig_test_asan
	$(O)/dsig_test_asan $(ARGS)

clean:
	rm -rf $(O)

.PHONY: all run asan clean
