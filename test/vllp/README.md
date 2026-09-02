# VLLP test & verification framework

Exercises the two independent VLLP implementations against each other and
against a spec-derived checker, under fault injection:

- **host client** — `host/dsig/vllp.c` (the Linux side).
- **MCU server** — `src/net/vllp.c`, built for `vexpress-a9` and run under
  `qemu-system-arm`, reached over DSIG-UDP through slirp.
- **reference server** — a spec-faithful host server (`refserver.c`) used
  for the in-process backend.

Every frame in both directions passes through a link simulator
(`linksim.c`) that can drop, duplicate, corrupt, delay, reorder, and
black out traffic, so the same scenarios run over a perfect link or a
hostile one on any backend.

## Backends

| backend | client            | peer under test          | needs QEMU |
| ------- | ----------------- | ------------------------ | ---------- |
| `sim`   | host `vllp.c`     | host `refserver.c`       | no         |
| `qemu`  | host `vllp.c`     | MCU `src/net/vllp.c`     | yes        |

Both run at MTU 64 (FDCAN-sized) and MTU 8 (classic-CAN-sized). The two
device servers are `vllp_server_create(0x200,0x201,64,3)` and
`vllp_server_create(0x210,0x211,8,3)` in
`src/platform/vexpress-a9/main.c`.

## Build & run

```bash
make            # builds host/dsig lib, vexpress-a9 firmware, and vllp_test
make run        # every default scenario, both backends, both MTUs
make run-sim    # host-vs-host only (no QEMU needed) -- fast
make run-qemu   # MCU only

# pass args through:
ARGS="-v -m 64 quick"  make run-sim
ARGS="-b qemu chargen discard multi_rx"  make run
```

`qemu-system-arm` must be on `$PATH` for the qemu backend
(`apt install qemu-system-arm`).

### Options (`build/vllp_test -h`)

```
-b sim|qemu|both   backend (default both)
-m 64|8|both       MTU (default both)
-s SCALE           multiply every scenario's duration
-S SEED            fault-injection seed (runs are reproducible per seed)
-k                 keep going on a peer after a scenario fails
-v                 verbose (per-frame vllp log, per-message detail)
-T                 write a decoded frame trace per scenario to the log dir
-l                 list scenarios
```

Logs, per-scenario frame traces (`-T`), and server-side diagnostics for
failed scenarios (`show_vllp`, `log`, `ps`, `show_malloc` captured over
the QEMU console) land in `build/logs/`.

## Scenarios

Grouped by tag; run a tag or a name as an argument.

- **quick**: link bring-up, echo across sizes 0..4000, pipelined echo,
  unknown-service rejection, all 14 channels at once, open/echo/close churn.
- **bandwidth**: chargen (download), discard (upload), `multi_rx` (10
  channels server→client), `multi_tx` (10 channels client→server).
- **long**: idle (keepalive only, no spurious drops).
- **link**: reconnect with a channel still open on the server; link
  timeout under blackout and recovery.
- **faults**: 1/5/20% loss, 10% duplication, 30ms latency, loss+dup+delay.
  Data integrity and eventual delivery must hold; the link must not drop.
- **chaos**: reordering and bit-flips. The link may reset (CRC failure or
  the 1-bit sequence cannot represent reordering), but corrupt data must
  never be accepted and the link must recover.
- **bidir** (excluded from the default run): `multi_bidir`, bulk both ways
  at once. Currently wedges — see `FINDINGS.md`.

Every scenario ends by asserting the link is connected and idle with no
channels open on either side; `settle()` restarts the client between
scenarios so each starts from a clean session.

## Status

`make run-sim` and `make run-qemu` are green for all default scenarios at
both MTUs. The one known-failing case is `multi_bidir`; see `FINDINGS.md`
for that and for the MCU bugs this framework found and fixed.
