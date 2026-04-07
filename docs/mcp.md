# MIOS MCP Server

MCP (Model Context Protocol) server that allows AI assistants to interact
with MIOS devices over USB. Supports CLI command execution, memory reads,
firmware flashing (DFU and ST-Link), and signal capture.

## Device-side setup

Add the USB interfaces to your platform's interface queue:

```c
#include <usb/usb.h>

struct usb_interface_queue q;
STAILQ_INIT(&q);

usb_cdc_create_shell(&q);            // Optional: serial console
usb_dfu_runtime_create(&q);          // Optional: in-app DFU detach
usb_mcp_create(&q, 0x01);            // MCP command interface (subclass 0x01)

your_platform_usb_create(vid, pid, manufacturer, product, &q);
```

The MCP command interface uses USB vendor class (0xFF) with subclass
0x01. Sigcapture uses subclass 192. These are separate USB interfaces.

Rebuild and flash your firmware.

## Building the MCP server

```
make -C host/mcp
```

Produces `host/mcp/mios-mcp`. Requires `libusb-1.0` development
headers (`apt install libusb-1.0-0-dev` on Debian/Ubuntu).

## Claude Code integration

Add to `~/.claude.json`:

```json
{
  "mcpServers": {
    "mios": {
      "command": "/path/to/mios/host/mcp/mios-mcp"
    }
  }
}
```

Restart Claude Code. The following tools become available:

### configure

Set the USB VID/PID used to find the MIOS device. Affects all
subsequent tool calls. Default VID is 0x6666, PID is 0 (match any).

```
configure(vid: 0x1234, pid: 0x5678)
```

### cli

Send a CLI command to the device via the USB MCP interface and get
the text output back.

```
cli(command: "uptime")
cli(command: "i2c scan 0", timeout_ms: 10000)
```

### read_memory

Read raw memory from the device (max 32 bytes per call). Returns
a hex dump.

```
read_memory(address: 0x08000000, length: 32)
```

### flash_dfu

Flash firmware via USB DFU. Automatically handles DFU Runtime detach
if the device is running (requires `usb_dfu_runtime_create` on device).

```
flash_dfu(elf_path: "/path/to/build.elf")
flash_dfu(elf_path: "/path/to/build.elf", force: true)
```

### flash_stlink

Flash firmware via a running OpenOCD instance (TCL interface, default
port 6666). Auto-detects flash vs RAM targets from the ELF load address.

```
flash_stlink(elf_path: "/path/to/build.elf")
flash_stlink(elf_path: "/path/to/build.elf", host: "127.0.0.1", port: 6666)
```

### sigcapture_list

List recent signal captures stored in memory. A background thread
continuously receives captures from the device and stores up to 16
in a ring buffer. Returns metadata only (no sample data).

```
sigcapture_list()
```

### sigcapture_save

Export a capture to CSV for analysis. The CSV has a `time_s` column
(relative to trigger) followed by one column per channel with scaled
values. Use the capture ID from `sigcapture_list`.

```
sigcapture_save(id: 42, path: "/tmp/capture.csv")
```

The CSV can be loaded with Python/pandas/matplotlib for analysis.

## USB MCP command protocol

The MCP command interface (`usb_mcp_create`, subclass 0x01) uses
64-byte bulk transfers with a single-byte message type prefix:

| Type | Direction    | Name             | Payload                          |
|------|------------- |------------------|----------------------------------|
| 0x01 | Host->Device | CLI Execute      | Command string (max 63 bytes)    |
| 0x02 | Device->Host | CLI Response     | Output text (up to 63 bytes)     |
| 0x03 | Device->Host | CLI Complete     | int32_t error code (LE)          |
| 0x04 | Host->Device | Read Memory      | uint32_t addr + uint32_t length  |
| 0x05 | Device->Host | Memory Response  | Raw data bytes                   |

CLI flow: host sends 0x01, device sends N x 0x02 packets with output
text, then 0x03 with the error code.

## Sigcapture protocol

Sigcapture uses a separate vendor-class USB interface (subclass 192)
with a single bulk IN endpoint. The device pushes capture data after
a trigger event. The MCP server receives this in a background thread.

Packet dispatch by size:
- 10 bytes: preamble (channels, depth, sample rate, trigger offset)
- 20 bytes: channel descriptor (name, unit, scale)
- 64 bytes: data (columnar int16_t samples)
- 1 byte: trailer (0xfe, capture complete)
