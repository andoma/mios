# VLLP findings

What the framework turned up, and what was changed in response. Bugs
were confirmed against `src/net/vllp.c` (MCU) and `host/dsig/vllp.c`
(host) at the start of this work.

## Fixed

### 1. MCU reassembly dropped multi-pbuf messages (correctness)

`vllp_channel_receive` guarded its "allocate a second pbuf" path with
`if(to_copy > len)` after `to_copy = MIN(avail, len)`, which is never
true. Any inbound message that spilled past one 512-byte pbuf was
silently truncated, failed CRC, and tore the link down. Rewrote
reassembly to reserve the next pbuf before copying and to fail cleanly
(retransmit) when out of buffers. Now exercised by `echo_sizes` up to
4000 bytes.

### 2. Buffer exhaustion could wedge the device (robustness)

The IPv4/Ethernet output path (`udp.c`, `ether.c`, `igmp.c`) called
`pbuf_prepend(..., wait=1)` on the net thread. That thread is also the
one that frees buffers, so under exhaustion it blocked forever waiting
for a buffer only it could release: a permanent wedge, no panic, no
recovery. Made those allocations non-blocking and drop-on-failure.
Separately, VLLP reassembly now refuses the last few pbufs
(`VLLP_PBUF_RESERVE`) and caps a single message at `VLLP_MAX_MESSAGE_PBUFS`,
closing the offending channel with `ERR_MTU_EXCEEDED` instead of starving
the system. The link now rides through buffer pressure instead of dying.

### 3. Channel close lifecycle (correctness)

- Host: a channel could queue two CLOSEs, and on peer-initiated close it
  left queued data ahead of the CLOSE response, so the response arrived
  only after the whole backlog drained (3s forced-close timeouts under
  load). The host now purges a channel's queue on peer close and sends
  the response immediately; a responder frees the channel as soon as its
  CLOSE is queued.
- MCU: on disconnect, channels with no live session are destroyed
  directly (a CLOSE has nowhere to go), the CMC queue is flushed, and the
  net-task for a channel is cancelled before the channel is freed
  (`net_task_cancel`, newly exported from `net_core.c`).

### 4. Prompt acknowledgement (throughput / spec)

Both sides delayed ACKs on a timer. A received data fragment now sets an
`ack_pending` flag and is acknowledged in the next `tx` pass: piggybacked
on a data frame if the next one goes out on the same channel, otherwise
as a pure ACK. This restores the peer's per-channel flow bit promptly and
removes the artificial per-fragment delay.

### 5. MCU advertised "no flow" for pull-only channels (spec)

`vllp_refresh_local_flow_status` cleared a channel's rx-flow-bit whenever
the app had no `may_push` callback. Pull-only services (chargen) have
none, so the MCU told the peer "do not send here" on channels that were
perfectly fine. A missing `may_push` now means "always ready".

### 6. MCU deadlocked receiving on more channels than it had pbufs (robustness)

Ten channels streaming into the MCU at once (`multi_tx`) stalled the whole
link: `vexpress-a9` ships 8 packet buffers (`PBUF_DEFAULT_COUNT`), and ten
concurrent inbound messages each hold a buffer during reassembly, so the
pool exhausts and no message completes. Raised the pool to 48 on
`vexpress-a9` (`-DPBUF_DEFAULT_COUNT=48`). The general rule, worth stating
for real deployments: size the pbuf pool to at least the maximum number of
channels that receive concurrently, plus headroom. A stronger fix would be
for the MCU to advertise rx-flow-off under buffer pressure so senders pause
rather than spin; noted as a follow-up.

## Open — needs a design decision

### Concurrent bidirectional origination wedges the link

`multi_bidir` (and any load where both peers originate data frames at the
same time) hangs until the 3s link timeout, repeatedly.

Root cause: VLLP's S/E sequence is a single 1-bit stop-and-wait sequence
**shared across the whole link** (spec: "The S/E bits are shared by all
channels on the link"), and both peers may originate. When only one side
originates at a time it works — `multi_rx` (10 channels server→client),
`multi_tx` (10 channels client→server), and `echo_pipelined` (naturally
alternating request/response) all pass. But when both sides have data to
originate concurrently — e.g. server `chargen` down while client
`discard` runs up, or bulk one way plus echo the other — each side flips
the shared S bit independently, the receivers' expected-sequence
bookkeeping desyncs, neither side accepts the other's data, and the link
stalls until it times out and resets. Under continuous load it never
stays up.

This is not a regression: the unmodified firmware fails `multi_bidir`
identically. It is a protocol/implementation gap, not a local bug, so it
is left for a design decision rather than patched blindly. The fix needs
an origination-arbitration rule both implementations honour, for example:

- treat the link as one shared stop-and-wait pipe with at most one
  data frame in flight from *either* peer, and have a peer defer its own
  origination while it owes an ACK or the peer has an outstanding frame; or
- give each peer its own sequence bit (S for client-origin, a second bit
  for server-origin) so the two directions no longer share one counter.

`multi_bidir` is tagged `bidir` and excluded from the default run so the
suite is green; run `ARGS="multi_bidir" make run` to reproduce.

## Verified correct

### MCU 3-second inactivity timeout

The spec requires a peer to tear the link down after 3s without received
data. Freezing a connected client (SIGSTOP on the real `dsig` process, not
its wrapper) makes the MCU server go `Disconnected -- timeout` within ~3s,
as required. The `link_timeout` scenario asserts this server-side drop only
on the `sim` backend: over QEMU user-mode networking the guest shares a
multicast group and its receive path cannot be cleanly blacked out by the
link sim, so that half of the check is sim-only. The client-side timeout
and full recovery are checked on both backends.

## Not a protocol issue

Reordering and bit-flips (`reorder`, `corrupt`) reset the link by design:
a corrupt frame fails CRC, and the 1-bit sequence cannot represent a
reordered fragment. The framework only requires that corrupt data is
never accepted and that the link recovers once the fault stops; both
hold. Very slow links (8-byte MTU at 30ms latency) are slow, not broken;
the scenarios scale their expectations to the link.

## Update: virtual-time host-stack harness (Option B)

The VLLP tests were re-architected to run in-process under `PLATFORM=host`
in virtual time (`build.host/mios.elf vllp`), with the **real production
host client** (`host/dsig/vllp.c`) driving the **real mios server**
(`src/net/vllp.c`) over a virtual CAN interface. No QEMU, no UDP; the full
connect + echo (sizes 1..2000) + 200-message chargen download + 200-message
discard upload runs in ~20 ms.

How the real host client runs in host-mios: `vllp.c` compiles a second time
with `-DVLLP_SIM` (see `host/dsig/vllp_sim.h`), which renames its public
API to `hvllp_*` (avoiding the guest's `vllp_*`), replaces pthreads with a
single cooperative pump, swaps `CLOCK_MONOTONIC` for the virtual clock, and
gives it a private allocator (sim threads must not call mios malloc). The
production pthread build is behind `#ifndef VLLP_SIM` and is byte-unchanged.

### 7. echo service spun the tx loop during partial reassembly (fixed)

`echo_pull()` raised `PUSHPULL_EVENT_PUSH` even when it had nothing to
return. While a multi-fragment echo request was still being reassembled,
the server's tx loop polled `echo_pull`, got NULL plus a re-armed PUSH,
and looped forever. QEMU masked it (the client runs concurrently and
breaks the spin by delivering the next fragment); the lockstep virtual-time
harness surfaced it deterministically. Fixed in `svc_echo.c`: only raise
PUSH when a buffer is actually returned.

### 8. stray CMC open-response reset the whole link (fixed, host client)

`cmc_handle_open_response()` returned `VLLP_ERR_BAD_STATE` for an
open-response naming an unknown channel or a channel not in OPEN_SENT.
That error propagates to `vllp_handle_rx` -> `vllp_disconnect`, so a single
late/stale CMC frame from a just-reset session tears the link down again --
a reset cascade that stops the link recovering under sustained corruption.
The MCU server already ignores frames for unknown channels; the host client
now does the same (log at INFO, return 0). Surfaced by the virtual-time
`corrupt` phase; fixed in `host/dsig/vllp.c`.

## Virtual-time suite coverage (build.host/mios.elf vllp)

The in-process host-stack suite now covers, all with the real host client
vs the real mios server over vcan:
echo across sizes 0..2000; 8-deep pipelined echo; 10 concurrent echo
channels; chargen download (200 msgs); discard upload (200 msgs);
bidirectional (chargen down + discard up interleaved); and fault phases
with drop/dup/corrupt injected in both directions -- 5% loss, 20% loss,
10% duplication (all ride through with no lost or corrupted data), and 1%
corruption (link resets on CRC failure, never accepts corrupt data, and
recovers). Whole suite runs in ~3s virtual time.
