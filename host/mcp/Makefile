MCP_SRC = \
	mcp_server.c \
	mcp_tool_dfu.c \
	mcp_tool_stlink.c \
	mcp_tool_cli.c \
	mcp_tool_sigcapture.c \
	cJSON.c \
	../mios_image.c

mios-mcp: ${MCP_SRC}
	@echo "\tHOSTCC\t$@"
	cc -I.. -I. \
		-D_GNU_SOURCE -O2 -Wall -Werror \
		-o $@ ${MCP_SRC} \
		$$(pkg-config --libs --cflags libusb-1.0) -lpthread

clean:
	rm -f mios-mcp
