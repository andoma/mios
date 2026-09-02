# Prototype: build mios as a shared object and drive it from a glibc harness.
#
#   make        build mios.so + harness
#   make run    build and run the harness against mios.so
#
MIOS  := ../..
SO    := $(MIOS)/build.hostlib/mios.elf   # a shared object despite the name

.PHONY: all run so clean
all: harness so

# Always defer to the mios build; it tracks the app source itself.
so:
	$(MAKE) -C $(MIOS) PLATFORM=hostlib HOSTLIB_APP=$(abspath thermostat.c)

harness: harness.c
	$(CC) -O2 -Wall -Wextra -o harness harness.c -ldl

run: all
	./harness $(SO)

clean:
	rm -f harness
