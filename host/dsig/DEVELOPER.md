# host/dsig — developer guide

A small C library for talking to mios devices from a Linux host. It
exposes two layers:

- **DSIG** — fire-and-forget pub/sub on 32-bit signal IDs. Drop-in mirror
  of the guest-side `dsig_*` API in `src/net/dsig.c`.
- **VLLP** — reliable framed sub-channels on top of a DSIG signal pair.
  See `docs/vllp.txt` for the wire protocol; this file only covers the
  host C bindings.

The library is plain C99 + pthreads. There is no link against any
non-system library; just compile the `.c` files you need.

## Layout

```
host/dsig/
├── dsig.h / dsig.c              core pub/sub bus
├── dsig_udp.h / dsig_udp.c      UDP multicast transport (matches
│                                src/net/dsig_udp.c on the guest)
├── dsig_cansock.h /             Linux SocketCAN transport
│   dsig_cansock.c
├── dsig_vllp.h / dsig_vllp.c    runs a VLLP endpoint on a dsig_t
├── vllp.h / vllp.c              VLLP client/server core
├── vllp_logstream.{c,h}         streaming log service
├── vllp_alertstream.{c,h}       alert state-machine service
├── vllp_telnetd.{c,h}           telnet bridge service
├── vllp_term.{c,h}              interactive shell over a channel
├── vllp_rpc.{c,h}               request/response RPC
├── vllp_ota.{c,h}               firmware-over-the-air
├── dsig_tool.c                  the `dsig` CLI
└── Makefile                     builds libdsig.a and dsig
```

Build everything with `make`. The static lib (`build/libdsig.a`) and the
CLI (`build/dsig`) come out together.

## Mental model

```
       application code
              |
              |  dsig_sub / dsig_send / dsig_emitter_*
              v
        +-----------+
        |  dsig_t   |    bus thread, sub list, emitter scheduler
        +-----+-----+
              |  tx callback              ^
              |                           |  dsig_input()
              v                           |
        +-----------+   wire             /
        | transport |--------------------/
        +-----------+   (UDP / cansock / ...)
```

VLLP fits in by claiming two signal IDs on a bus:

```
host VLLP client          host VLLP server
        \                    /
   txid=A,rxid=B        txid=B,rxid=A
         \              /
          +--- dsig ---+
```

Anything VLLP transmits goes out on the bus's `txid` signal; anything
received on `rxid` is fed back into VLLP via a `dsig_sub`.

## Quickstart: pub/sub over UDP

```c
#include "dsig.h"
#include "dsig_udp.h"

static void on_msg(void *opaque, uint32_t signal,
                   const void *data, size_t len) {
    printf("got 0x%08x (%zu)\n", signal, len);
}

int main(void) {
    dsig_udp_t *udp = dsig_udp_create(NULL, 0, NULL);   // defaults
    dsig_t     *bus = dsig_create(dsig_udp_tx, udp);
    dsig_udp_start(udp, bus);

    dsig_sub(bus, 0x1234, 0xffffffff, /*ttl_ms=*/0, on_msg, NULL);
    dsig_send(bus, 0x5678, "hi", 2);

    pause();   // wait for traffic
}
```

Compile with `-I host/dsig host/dsig/build/libdsig.a -lpthread`.

The mask works the same way as the guest's `dsig_sub`: a frame matches
when `(received & mask) == signal`. A mask of `0` listens to every
signal; a mask of `0xffffffff` filters to exactly one.

### Subscription TTL

Pass `ttl_ms > 0` to `dsig_sub` to get a watchdog. Every matching frame
rearms the timer; when it expires the callback fires once with `data ==
NULL, len == 0` and the subscription stays in place, disarmed until the
next matching frame arrives. Same semantics as the guest's `dsig_sub`.

### Periodic publish

```c
dsig_emitter_t *e = dsig_emitter_create(bus, 0x10, /*refresh_ms=*/100);
dsig_emitter_update(e, &state, sizeof(state));   // emits now + every 100ms
...
dsig_emitter_update(e, &state, sizeof(state));   // updates payload, resets timer
dsig_emitter_set_refresh(e, 250);                // change rate
dsig_emitter_destroy(e);                         // stops
```

`refresh_ms == 0` disables auto-repeat; the emitter only fires on
`dsig_emitter_update()`.

## Transports

### UDP (`dsig_udp.h`)

Matches the guest-side `src/net/dsig_udp.c` byte-for-byte: each datagram
carries `[u32 LE signal_id][payload]`. Defaults to multicast group
`239.255.213.22:54550`. Multicast loopback is enabled so two processes
on the same host can talk to each other.

```c
dsig_udp_t *udp = dsig_udp_create("239.255.213.22", 0xd516, /*ifname=*/NULL);
```

Passing `bind_ifname` joins the group on a specific interface and forces
outgoing multicast out the same one — useful when the host has several
NICs.

### Linux SocketCAN (`dsig_cansock.h`)

Uses raw CAN-FD frames. Signal id maps directly to `can_id`, payload to
`frame.data` (≤ 64 bytes). Defaults to `$IFC` or `can0`. Loopback is
**disabled** on the socket, so a single process talking to itself will
not hear its own emits — bring up two processes or two hosts.

### Adding your own

A transport is just a TX function (`dsig_tx_fn`) and an rx thread (or
event source) that calls `dsig_input(bus, signal, data, len)` for every
received frame. Look at `dsig_udp.c` as the smallest worked example — it
fits in ~140 lines.

## VLLP

`vllp.h` is the protocol implementation; `dsig_vllp.h` is a thin glue
layer that runs a VLLP endpoint on top of a `dsig_t`.

### Client (host attaches to a device)

```c
#include "dsig_vllp.h"
#include "vllp_logstream.h"

static void on_log(void *_, int level, uint32_t seq,
                   int64_t ms_ago, const char *msg) {
    printf("[%d] -%lldms: %s\n", level, (long long)ms_ago, msg);
}

dsig_vllp_t *dv = dsig_vllp_client_create(
    bus,
    /*txid=*/0x201, /*rxid=*/0x200,   // host POV; device is the mirror
    /*mtu=*/64, /*timeout=*/3,
    /*vllp_flags=*/VLLP_FDCAN_ADAPTATION,
    /*log_opaque=*/NULL, /*log_cb=*/NULL);

vllp_logstream_t *ls = vllp_logstream_create(dsig_vllp_get_vllp(dv),
                                             NULL, on_log);
```

`vllp_flags` should include `VLLP_FDCAN_ADAPTATION` whenever MTU > 8
(see `docs/vllp.txt` "CAN network adaptation" for what that does). Pass
`0` for legacy 8-byte CAN.

### Server (host pretends to be the device)

```c
static open_channel_result_t on_open(void *_, const char *name,
                                     vllp_channel_t *vc) {
    if (!strcmp(name, "echo")) {
        return (open_channel_result_t){ .opaque=vc, .rx=echo_rx };
    }
    return (open_channel_result_t){ .error = VLLP_ERR_NOT_FOUND };
}

dsig_vllp_t *dv = dsig_vllp_server_create(bus, 0x200, 0x201, 64, 3,
                                          VLLP_FDCAN_ADAPTATION,
                                          NULL, NULL, on_open);
```

### Layering existing services

Anything in `vllp_*.{c,h}` plugs onto the `vllp_t` returned by
`dsig_vllp_get_vllp(dv)`:

| Header                | Purpose                                        |
| --------------------- | ---------------------------------------------- |
| `vllp_logstream.h`    | streaming logs out of the device               |
| `vllp_alertstream.h`  | mark/raise/sweep alert lifecycle              |
| `vllp_telnetd.h`      | expose a VLLP shell channel as a TCP telnet   |
| `vllp_term.h`         | take over stdin/stdout for an interactive shell |
| `vllp_rpc.h`          | request/response with CBOR payloads          |
| `vllp_ota.h`          | push firmware images                          |

These are unchanged from before the `host/dsig/` move; their headers
document the entry points.

### Direction conventions, once more

Two endpoints, one signal pair:

| Endpoint    | `txid` is…                | `rxid` is…              |
| ----------- | -------------------------- | ----------------------- |
| host client | what we *send* (= device's rx) | what we *recv* (= device's tx) |
| device      | what the device *sends* | what the device *recv*  |

When wiring against a guest that did
`vllp_server_create(0x200, 0x201, ...)`, a host client passes
`txid=0x201, rxid=0x200`. The `dsig` CLI follows the same host POV.

## Threading and lifetimes

- Each `dsig_t` owns one internal thread. Subscribers' callbacks fire
  from either that thread (TTL timeouts) or the transport's rx thread
  (data frames). They MUST be reentrant w.r.t. each other and to any
  caller that holds an external lock.
- All public functions on `dsig_t`, `dsig_sub_t`, and `dsig_emitter_t`
  are safe to call from any thread, including from inside a callback.
- `dsig_destroy()` joins the bus thread; you must destroy all transports
  attached to a bus before destroying the bus itself, in this order:
  `dsig_udp_destroy(udp); dsig_destroy(bus);` (or
  `dsig_cansock_destroy()` analogously).
- `dsig_vllp_destroy()` unsubscribes from the bus before tearing down
  the underlying `vllp_t`, so it can run regardless of what's currently
  on the wire.
- `vllp.c`'s own thread runs independently of the dsig bus thread.

## CLI tool

`build/dsig` wraps everything above. `dsig --help` (or just `dsig` with
no args) prints a self-contained reference with examples. The CLI
matches the library 1:1 so it doubles as a worked example — see
`dsig_tool.c`.

## Wire format reference

DSIG-over-UDP: `[u32 LE signal][payload]` in a single UDP datagram.
DSIG-over-CAN(FD): `signal -> can_id`, `payload -> frame.data`. These
are the only two on-wire formats the host knows about; everything else
is VLLP framing layered inside the payload (see `docs/vllp.txt`).

The corresponding guest decoders are `src/net/dsig_udp.c` (UDP) and
`src/net/can/can.c` (CAN).
