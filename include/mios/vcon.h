#pragma once

// Virtual console.
//
// A vcon decouples a console (a bidirectional byte source/sink such as a TCU
// channel from smux, or a local shell run via cli_on_stream) from the physical
// transport a user attaches with. Output is buffered in a scrollback ring so a
// client that attaches late still sees recent history, and so we are not forced
// to spend one USB CDC-ACM per console.
//
// Multiple clients may attach the same vcon simultaneously (shared-session):
// output is mirrored to every attached terminal and their keystrokes are merged
// into a single input stream toward the backend.

#include <stddef.h>
#include <stdint.h>

struct stream;

typedef struct vcon vcon_t;
typedef struct vcon_client vcon_client_t;

// Create a vcon. `name` is referenced, not copied (pass a string literal or
// other long-lived storage). `scrollback` and `input` are buffer sizes in
// bytes. Intended for init/boot; panics on OOM.
vcon_t *vcon_create(const char *name, size_t scrollback, size_t input);

// Create a vcon with a MIOS shell bound to its backend. Attaching to it gives a
// normal interactive shell (with scrollback). Returns NULL on failure.
vcon_t *vcon_create_shell(const char *name, size_t scrollback, size_t input);

// Stream for the console backend (the data producer/consumer):
//   write() -> appended to scrollback and mirrored to all attached clients,
//              never blocks (drops on a congested client link).
//   read()  -> keystrokes injected by attached clients; blocks / pollable.
struct stream *vcon_backend(vcon_t *vc);

// Attach a client terminal. The cursor starts at the oldest buffered byte so
// the first vcon_client_output() replays scrollback. The caller drives I/O from
// its own thread via vcon_client_output()/vcon_client_wait() -- vcon spawns no
// thread of its own. Returns a handle, or NULL on failure.
vcon_client_t *vcon_attach(vcon_t *vc, struct stream *term);
void vcon_detach(vcon_client_t *vcc);

// Non-blocking: copy up to `size` bytes of this client's pending console output
// into buf, advancing its cursor. Returns bytes copied (0 if none pending).
size_t vcon_client_output(vcon_client_t *vcc, void *buf, size_t size);

// Block until this client has pending output, or its terminal has input ready.
void vcon_client_wait(vcon_client_t *vcc);

// Permanently bind a terminal stream to a vcon: spawns a thread that pumps
// console output to the terminal and terminal input back to the console for the
// life of the system. Use for a dedicated USB port (no attach/detach, no escape
// key). The vcon may still have other clients attached concurrently.
void vcon_bind(vcon_t *vc, struct stream *term);

// Inject client keystrokes toward the backend. Returns bytes accepted (may be
// short if the input buffer is full).
size_t vcon_input(vcon_t *vc, const void *buf, size_t len);

// Called when a client types something. For backends that are not a thread
// sitting in vcon_backend()'s read(): without this they have no way to
// learn a keystroke arrived, and the alternative is a thread per vcon
// whose only job is to poll.
//
// Runs on whichever thread called vcon_input(), with the vcon lock held,
// so it must not block and must not call back into vcon. Raising an event
// (pushpull_wakeup, net_task_raise) is what it is for. Holding the lock is
// deliberate: it is what lets a backend clear the callback and then free
// itself without racing a call already in flight.
void vcon_set_backend_notify(vcon_t *vc, void (*cb)(void *opaque),
                             void *opaque);

// Look at up to `size` bytes of pending client input without consuming
// it, then consume what was actually used. Two steps so a backend that
// cannot take the bytes right now (no buffer, say) leaves them queued
// rather than dropping keystrokes.
size_t vcon_input_peek(vcon_t *vc, void *buf, size_t size);
void vcon_input_consume(vcon_t *vc, size_t len);

// Discard pending client input. For a backend that has just reconnected:
// what is queued was typed at a session that no longer exists, and the
// tail of a half-typed command is worse than nothing in a fresh shell.
void vcon_input_flush(vcon_t *vc);

// Registry helpers (vcons are created at init and never destroyed).
vcon_t *vcon_find(const char *name);
vcon_t *vcon_first(void);
vcon_t *vcon_next(vcon_t *vc);
const char *vcon_name(const vcon_t *vc);
size_t vcon_scrollback_used(vcon_t *vc);
int vcon_client_count(vcon_t *vc);
