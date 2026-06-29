# nRF54L multiprotocol radio architecture

Living design doc for running BLE and IEEE 802.15.4 (Thread, later Matter)
concurrently on the single nRF54L 2.4 GHz radio.

## Goal & motivation

One radio, multiple protocols alive at once via hardware-timed time-division.
The headline target is **one BLE connection + a Thread router, simultaneously**.

The driving use case is **OTA upgrading Thread devices from a normal computer**.
Laptops have BLE, not 802.15.4, so the nRF54L becomes a **BLE-attached Thread
access point**:

```
  MacBook/Linux  <--BLE-->  nRF54L  <--Thread (802.15.4)-->  mesh devices
```

BLE is the uplink from a general-purpose computer; Thread is the mesh we push
images into. So concurrent BLE+Thread is not a nicety, it is the core feature.

## Decisions taken

- **Native Thread stack**, not OpenThread. Consistent with mios (the whole
  IPv4/TCP/UDP/DHCP/NTP/PTP/HTTP stack is in-house; littlefs is the only
  outside dependency and it is not network code). The full Thread stack is
  **deferred**; near-term work is the radio layer + concurrency.
- Headline concurrency: **1 BLE connection + Thread router** (rx-on-when-idle,
  the demanding case).
- First milestone: **a live BLE connection while dumping incoming Thread
  frames** off the same radio. Lowest layers first.

## Why time-division works (not simultaneity)

One PHY at a time; switch fast enough that neither stack notices. The two are
asymmetric, which is what makes it tractable:

- **BLE is sparse, periodic, hard-deadline.** A connection touches the radio
  ~1-2 ms every connInterval (e.g. 30 ms) at anchor points that must be hit.
- **802.15.4 is elastic.** CSMA/CA + MAC acks/retransmits mean a missed slot is
  recovered, not fatal. Thread fills the gaps and tolerates preemption.

Multiple BLE connections are the same problem: each central times its own
connection independently and they do not coordinate, so the peripheral is the
sole arbiter. On overlap it honors one event and skips the other; BLE tolerates
skips until the supervision timeout (many intervals). So every BLE connection
is just another timed client in the same calendar as Thread.

## Layering

```
  radio-core     sole HW owner: RADIO + TIMER10 + DPPIC10 + reserved GRTC CCs.
       |          PHY presets (radio_use_ble / radio_use_154), save/restore on switch.
  arbiter        timeslot scheduler: priority, deadlines, preemption, blackout.
       |          launches slots at absolute GRTC time via DPPI; IRQ dispatch by owner.
  clients        BLE link-layer | 802.15.4 MAC | raw/diagnostic
       |
  stacks         l2cap/GATT/SMP (BLE) | 6LoWPAN/IPv6/MLE/routing (Thread, later)
       |
  application     OTA bridge | Matter (long horizon)
```

## Timing infrastructure

- **GRTC** (12 compare channels, each with PUBLISH_COMPARE -> DPPI) is the
  global absolute timebase, already our systim clock. The arbiter reserves a
  couple of CC channels to launch a slot at an exact time via DPPI, so slot
  starts are jitter-free and the CPU can sleep until the GRTC interrupt. systim
  keeps its SYSCOUNTER read path. CC-channel allocation is a shared concern.
- **TIMER10** (radio power domain, 1 MHz) handles intra-slot sub-µs timing:
  the BLE T_IFS turnaround and RX anchor timestamping. Already in use by BLE.

## The timeslot model (target design)

A client submits a request: `{earliest_start, latest_start, duration(+ext),
priority, radio-program, callbacks}`. Two flavors:

- **Timed** (BLE anchors, scan windows): must start in the window or it is a
  miss. High priority, non-preemptable once committed.
- **Background** (802.15.4 rx-on-idle): "the radio whenever nothing timed needs
  it." Low priority, freely preempted.

Before each committed timed slot the arbiter reserves
`[T - switch - guard, T + duration]` (the **blackout**): it will not start a
background RX it cannot finish, and aborts one in flight if a higher-priority
timed slot arrives. Priority rises for a connection approaching its supervision
timeout so it never actually drops.

## Per-protocol roadmap

**BLE** (evolve current LL into a cooperative client):
- now: peripheral, advertising, 1 connection, l2cap.
- next: GATT/ATT server, SMP + LE Secure Connections (mios has AES/ECDSA/SHA).
- later: multiple connections, central + scanning, extended adv, 2M / Coded PHY.

**802.15.4 MAC**:
- now: RX proven (sniffed live Thread).
- next: TX, CCA + CSMA-CA, hardware auto-ACK (MHMU + FRAMESTART), energy scan,
  address filtering, promiscuous/sniffer.
- later: AES-CCM* security (mios AES), indirect TX + CSL for sleepy children.

**Thread (native, deferred):** 15.4 MAC -> 6LoWPAN -> IPv6 -> MLE -> routing ->
CoAP. **Matter (long horizon):** commission over BLE, operate over Thread.

## Coexistence policy (separate from mechanism)

- Modes: BLE-only, 15.4-only, concurrent. Single-client overhead ~= 0 (no
  switching when only one client is registered).
- Tunable priority profiles (commissioning favors BLE; steady-state favors the
  Thread router with BLE connection slots reserved). Clients register/unregister
  at runtime.

## Power (future)

- HFXO on-demand per slot (GRTC-timed XOSTART ahead of the slot, off when idle).
  Today it is kept on continuously.
- CPU WFI between slots, woken by GRTC compare. DC/DC + RAM retention + LFXO.

## Observability

- Frames/events logged from IRQ context via `netlog` / `netlog_hexdump`
  (net_log.c: enqueue a pbuf in IRQ, a net_task drains to the event log /
  console). Never `cli_printf` from the radio IRQ.
- Scheduler + per-client stats (slots granted/denied/preempted/missed; per
  protocol RX/TX/CRC). Future: PCAP-over-MCP sniffer into Wireshark.

## Milestones (each HW-testable)

- **M0 (done):** BLE peripheral; 15.4 RX proven on live Thread (ch25, PAN abf1).
- **M1 (in progress):** radio-core + minimal arbiter (1 timed BLE + 1 background
  15.4). BLE connection stays up while 15.4 dumps Thread frames concurrently.
- **M2:** full 15.4 client (RX/TX/CCA/auto-ACK/energy scan), 15.4-only mode.
- **M3:** generalize the arbiter (GRTC/DPPI-timed slots, priority/preempt tuned).
- **M4:** power (on-demand HFXO + CPU sleep between slots).
- **M5:** native Thread stack on the 15.4 client.
- **M6:** GATT/ATT + SMP; multiple BLE connections.
- **M7:** OTA bridge (image in over BLE, out over Thread); later Matter.

## M1 implementation notes

- `nrf54l_radio_core.{c,h}`: shared RADIO register map, HFXO start,
  `radio_use_ble()` / `radio_use_154()` (apply full PHY preset, idempotent).
- Arbiter owns radio-IRQ dispatch: a single `irq_138` checks the current owner
  and calls the BLE or 15.4 handler. Switching owner re-applies the PHY preset
  and re-points INTENSET, so a 15.4 frame never lands in the BLE END handler.
- BLE LL: at each radio activity (adv burst, connection window open) it acquires
  the radio (suspend 15.4, `radio_use_ble()`); when idle until the next anchor it
  releases (resume 15.4 background RX). Reclaim happens on the existing
  pre-anchor window timer, which gives margin for the config switch.
- 15.4 client: background RX on ch25, frames dumped via `netlog_hexdump`.
