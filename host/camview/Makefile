#
# camview: low latency viewer for the H.264/RTP stream from the
# STM32N6570-DK camera (see docs/stm32n6-camera.md)
#
# Needs libavcodec, libavutil and SDL2 development packages
# (Debian/Ubuntu: libavcodec-dev libavutil-dev libsdl2-dev)
#
# Usage:
#   make
#   ./camview [-p port] [-o]
#

PKGS := libavcodec libavutil sdl2

CC := gcc
CFLAGS := -O2 -g -Wall -Werror $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS))

camview: camview.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f camview

.PHONY: clean
