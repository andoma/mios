# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Mios is a lightweight embedded operating system written in C for microcontrollers and SoCs. It supports ARM Cortex-M, ARMv8 (AArch64), and RISC-V 64 architectures. The codebase is ~65K lines of C with no external RTOS dependencies.

## Build Commands

```bash
# Build for a specific platform (default: lm3s811evb)
make PLATFORM=stm32h7-nucleo144

# Build all supported platforms (this is what CI runs)
make -j$(nproc) allplatforms

# Generate stripped binary for flashing
make PLATFORM=stm32h7-nucleo144 bin

# Clean build artifacts
make PLATFORM=stm32h7-nucleo144 clean

# Build with timestamp
make PLATFORM=stm32h7-nucleo144 BTS=1

# Host-side CLI test (compiles shell for host)
make cli_test    # build only
make cli_run     # build and run interactive
```

Output goes to `build.${PLATFORM}/` (e.g., `build.stm32h7-nucleo144/build.elf`).

## Supported Platforms

lm3s811evb, stm32f405-feather, stm32g0-nucleo64, stm32f407g-disc1, bluefruit-nrf52, stm32f439-nucleo144, stm32g4-usb, vexpress-a9, stm32h7-nucleo144. Additional platforms (aarch64-virt, spike, t234 variants, nrf52, stm32wb55-nucleo64) exist but are not in the `allplatforms` CI target.

## Compiler Flags

- `-Wall -Werror` — all warnings are errors
- `-nostdinc` — no standard includes; Mios provides its own libc
- `-Wframe-larger-than=192` — stack frame size limit
- `.clang-format` disables auto-formatting (`DisableFormat: true`)

## Architecture

### Build System

Makefile-based with per-component `.mk` files. Each platform has a `.mk` that includes its CPU architecture `.mk` and enables features. Feature flags (`ENABLE_NET_IPV4`, `ENABLE_NET_CAN`, `ENABLE_METRIC`, etc.) default to `no` and are set by platform `.mk` files. A `config.h` is auto-generated from these flags.

Platform `.mk` path: `src/platform/${PLATFORM}/${PLATFORM}.mk`

### Kernel (`src/kernel/`)

- **Task scheduler**: 32 priority levels (0=idle, higher=higher priority). Supports lightweight tasks (run function) and full threads (dedicated stack). Mutexes and condition variables for synchronization.
- **Device framework**: Reference-counted device objects with parent-child hierarchy and power management callbacks.
- **Driver framework**: Drivers registered via `DRIVER(probefn, prio)` macro using linker sections for automatic discovery.
- Application entry point: weak `main()` function.

### Hardware Abstraction (`include/mios/`)

Consistent APIs across all platforms for GPIO, I2C, SPI, and UART. Platform-specific implementations live in `src/platform/`.

### Networking (`src/net/`)

IPv4 (TCP/UDP/DHCP/mDNS/NTP), CAN/FDCAN, BLE (L2CAP), and custom protocols: MBUS (multidrop bus, spec in `docs/mbus-v2.txt`) and VLLP (virtual link layer, spec in `docs/vllp.txt`). Services layer provides echo, shell, OTA updates, telnet, RPC over network.

### Key Patterns

- **Constructor-based init**: `__attribute__((constructor(PRIORITY)))` for boot ordering (100-102 = clock/core, 110 = console, 800+ = late init).
- **Linker section arrays**: CLI commands (`clicmd.*`), RPC definitions (`rpcdef`), services (`servicedef`), drivers (`driver.*`) are collected via linker sections.
- **Error handling**: Negative integers for errors (`ERR_TIMEOUT = -2`, etc.), zero for success. Defined in `include/mios/error.h`.
- **Packet buffers**: `pbuf` for network packet management.
- **Stream interface**: Generic I/O stream abstraction used for printf, logging, etc.

### Libraries (`src/lib/`)

Custom libc, crypto (RSA/ECDSA/AES/SHA), LittleFS filesystem, USB stack (CDC/HID/DFU), RPC with CBOR serialization, fixed-point math (libfixmath submodule).

### Toolchains

- ARM Cortex-M: `arm-none-eabi-`
- AArch64: `aarch64-none-elf-` (Linux) / `aarch64-elf-` (Darwin)
- RISC-V: `riscv64-linux-gnu-`

## CI

GitHub Actions runs `make -j$(nproc) allplatforms` on Ubuntu with `gcc-arm-none-eabi`. There are no automated tests beyond successful compilation of all platforms.
