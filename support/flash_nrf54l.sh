#!/usr/bin/env bash
#
# Flash an ELF into nRF54L RRAM via a running OpenOCD's TCL interface.
#
# This OpenOCD build has no flash driver for the nRF54L resistive-RAM
# (`flash banks` is empty), so the normal `program`/flash_stlink path fails.
# RRAM is bit-alterable memory: with RRAMC write-enabled it is written like
# RAM, but the controller goes busy after each commit, so block writes
# overrun and drop words. We therefore:
#   1. load the image into RAM (no timing constraints) via load_image,
#   2. copy RAM -> RRAM one word at a time, polling RRAMC.READY between
#      writes, from a Tcl proc that runs inside OpenOCD.
#
# Usage: support/flash_nrf54l.sh [build.<platform>/mios.full.elf] [tcl_port]
#
set -euo pipefail

ELF="${1:-build.nrf54l15-dk/mios.full.elf}"
PORT="${2:-6666}"
HOST=127.0.0.1
STAGE=0x20010000          # RAM staging address
RRAMC_CONFIG=0x5004B500   # WEN bit0, WRITEBUFSIZE bits[13:8]
RRAMC_READY=0x5004B400    # 1 = ready

BIN="$(mktemp --suffix=.bin)"
trap 'rm -f "$BIN"' EXIT
arm-none-eabi-objcopy -O binary "$ELF" "$BIN"
BYTES=$(stat -c%s "$BIN")
WORDS=$(( (BYTES + 3) / 4 ))
echo "Image: $ELF ($BYTES bytes, $WORDS words)"

ocd() {
  exec 3<>"/dev/tcp/$HOST/$PORT" || { echo "No OpenOCD TCL on $HOST:$PORT" >&2; exit 1; }
  local c
  for c in "$@"; do
    printf '%s\x1a' "$c" >&3
    IFS= read -r -d $'\x1a' -u 3 resp
    [ -n "$resp" ] && printf '%s\n' "$resp"
  done
  exec 3>&- 3<&-
}

read -r -d '' PROC <<EOF || true
proc prog_rram {n} {
  set words [read_memory $STAGE 32 \$n]
  mww $RRAMC_CONFIG 1
  set addr 0
  foreach w \$words {
    mww \$addr \$w
    while {[lindex [read_memory $RRAMC_READY 32 1] 0] == 0} {}
    incr addr 4
  }
  mww $RRAMC_CONFIG 0
  return "programmed \$n words"
}
EOF

ocd "halt" \
    "load_image $BIN $STAGE bin" \
    "$PROC" \
    "prog_rram $WORDS" \
    "verify_image $BIN 0x00000000 bin" \
    "reset run"
echo "Done."
