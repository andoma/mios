#
# VLLP test framework. Builds the host dsig/vllp library, the vexpress-a9
# firmware and the vllp_test runner.
#
#   make            build everything
#   make run        run all scenarios on all backends (sim + qemu, MTU 64 + 8)
#   make run-sim    host-vs-host only (no qemu needed)
#   make run-qemu   qemu only
#
#   ARGS="-v quick" make run     pass extra arguments to vllp_test
#

TOP  := ../..
DSIG := $(TOP)/host/dsig
O    := build

CC := gcc
CFLAGS := -O2 -g -Wall -Werror -I$(DSIG) -pthread
LDFLAGS := -pthread

LIB := $(DSIG)/build/libdsig.a
FIRMWARE := $(TOP)/build.vexpress-a9/mios.elf

SRCS := main.c tst.c linksim.c peer.c peer_sim.c peer_qemu.c refserver.c \
        scenarios.c
OBJS := $(SRCS:%.c=$(O)/%.o)

BIN := $(O)/vllp_test

all: $(BIN)

$(O):
	mkdir -p $(O)

$(O)/%.o: %.c | $(O)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(LIB):
	$(MAKE) -C $(DSIG)

# Always ask the sub-make; it knows if the lib is stale
.PHONY: lib firmware
lib:
	$(MAKE) --no-print-directory -C $(DSIG)

firmware:
	$(MAKE) --no-print-directory -C $(TOP) PLATFORM=vexpress-a9

$(BIN): $(OBJS) lib
	$(CC) $(OBJS) $(LIB) $(LDFLAGS) -lusb-1.0 -o $@

run: $(BIN) firmware
	$(BIN) -f $(FIRMWARE) $(ARGS)

run-sim: $(BIN)
	$(BIN) -b sim $(ARGS)

run-qemu: $(BIN) firmware
	$(BIN) -b qemu -f $(FIRMWARE) $(ARGS)

clean:
	rm -rf $(O)

-include $(OBJS:%.o=%.d)

.PHONY: all run run-sim run-qemu clean
