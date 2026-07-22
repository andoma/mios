SRC = \
	main.c \
	flash.c \
	flash_jlink.c \
	flash_dfu.c \
	flash_openocd.c \
	flash_nrfdfu.c \
	../jlink/jlink.c \
	../jlink/dap.c \
	../jlink/nrf54l.c \
	../mios_image.c

DEPS = flash.h \
	../jlink/jlink.h ../jlink/dap.h ../jlink/nrf54l.h \
	../mios_image.h ../dfu.c

mios-flash: ${SRC} ${DEPS}
	@echo "\tHOSTCC\t$@"
	cc -I.. -I. \
		-D_GNU_SOURCE -O2 -Wall -Werror \
		-o $@ ${SRC} \
		$$(pkg-config --libs --cflags libusb-1.0)

clean:
	rm -f mios-flash
