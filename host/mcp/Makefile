MCP_SRC = \
	mcp_server.c \
	mcp_transport.c \
	mcp_devices.c \
	mcp_tool_flash.c \
	mcp_tool_cli.c \
	mcp_tool_sigcapture.c \
	cJSON.c \
	../mios_image.c \
	../flash/flash.c \
	../flash/flash_jlink.c \
	../flash/flash_dfu.c \
	../flash/flash_nrfdfu.c \
	../flash/flash_openocd.c \
	../jlink/jlink.c \
	../jlink/dap.c \
	../jlink/nrf54l.c \
	../dsig/dsig.c \
	../dsig/dsig_transport.c \
	../dsig/dsig_udp.c \
	../dsig/dsig_unix.c \
	../dsig/dsig_cansock.c \
	../dsig/dsig_usb.c \
	../dsig/dsig_vllp.c \
	../dsig/vllp.c

mios-mcp: ${MCP_SRC}
	@echo "\tHOSTCC\t$@"
	cc -I.. -I. -I../flash -I../dsig \
		-D_GNU_SOURCE -O2 -Wall -Werror \
		-o $@ ${MCP_SRC} \
		$$(pkg-config --libs --cflags libusb-1.0) -lpthread -lz

clean:
	rm -f mios-mcp
