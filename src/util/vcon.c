#include <mios/vcon.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/cli.h>

#include <sys/queue.h>
#include <sys/param.h>

#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#include "vcon_ring.h"
#include "irq.h"

// Each attached client owns an output thread that follows its own cursor into
// the scrollback ring and does *blocking* writes to its terminal. The producer
// (backend write) only appends to the ring and bumps vc_total, so it never
// blocks on a slow terminal and never touches client terminals directly.
// Replaying scrollback on attach is just starting the cursor at the oldest
// buffered byte -- no special case, and not size-limited like a NO_WAIT dump.

struct vcon_client {
  LIST_ENTRY(vcon_client) vcc_link;
  vcon_t *vcc_vc;
  stream_t *vcc_term;
  size_t vcc_cursor;     // logical offset of next byte to send
};

LIST_HEAD(vcon_client_list, vcon_client);

struct vcon {
  stream_t vc_backend;   // Must be first; backend stream casts to vcon_t

  LIST_ENTRY(vcon) vc_link;
  const char *vc_name;

  mutex_t vc_mutex;      // Guards everything below

  size_t vc_total;       // Total bytes ever written to scrollback
  cond_t vc_out_cond;    // Signalled on new output or client stop
  struct vcon_client_list vc_clients;

  // Input fifo: keystrokes from clients toward the backend
  uint8_t *vc_in;
  size_t vc_in_size;
  size_t vc_in_head;     // write index
  size_t vc_in_tail;     // read index
  size_t vc_in_used;
  cond_t vc_in_cond;     // Signalled when input becomes available

  // Scrollback (output history). MUST be last: vc_sb.buf is a flexible array
  // allocated in the same block as the vcon (see vcon_create).
  vcon_ring_t vc_sb;
};


static LIST_HEAD(, vcon) g_vcons = LIST_HEAD_INITIALIZER(g_vcons);


static ssize_t
vcon_backend_write(stream_t *s, const void *buf, size_t size, int flags)
{
  vcon_t *vc = (vcon_t *)s;

  if(size == 0)
    return 0; // flush, nothing buffered on our side

  mutex_lock(&vc->vc_mutex);
  vcon_ring_append(&vc->vc_sb, buf, size);
  vc->vc_total += size;
  cond_broadcast(&vc->vc_out_cond);
  mutex_unlock(&vc->vc_mutex);
  return size;
}


static ssize_t
vcon_backend_read(stream_t *s, void *buf, size_t size, size_t required)
{
  vcon_t *vc = (vcon_t *)s;
  uint8_t *b = buf;

  mutex_lock(&vc->vc_mutex);

  size_t i = 0;
  while(i < size) {
    while(vc->vc_in_used == 0) {
      if(i >= required) {
        mutex_unlock(&vc->vc_mutex);
        return i;
      }
      cond_wait(&vc->vc_in_cond, &vc->vc_mutex);
    }

    size_t n = MIN(size - i, vc->vc_in_used);
    for(size_t k = 0; k < n; k++) {
      b[i + k] = vc->vc_in[vc->vc_in_tail];
      vc->vc_in_tail = (vc->vc_in_tail + 1) % vc->vc_in_size;
    }
    vc->vc_in_used -= n;
    i += n;
  }

  mutex_unlock(&vc->vc_mutex);
  return i;
}


static task_waitable_t *
vcon_backend_poll(stream_t *s, poll_type_t type)
{
  vcon_t *vc = (vcon_t *)s;

  if(type != POLL_STREAM_READ)
    return NULL; // backend write never blocks

  irq_forbid(IRQ_LEVEL_SWITCH);

  if(vc->vc_in_used)
    return NULL;
  return &vc->vc_in_cond;
}


static const stream_vtable_t vcon_backend_vtable = {
  .write = vcon_backend_write,
  .read = vcon_backend_read,
  .poll = vcon_backend_poll,
};


vcon_t *
vcon_create(const char *name, size_t scrollback, size_t input)
{
  // Created at init; OOM here panics (calloc/malloc), which is the right
  // contract for boot-time console setup. The scrollback buffer is folded into
  // this allocation via vc_sb's trailing flexible array.
  vcon_t *vc = calloc(1, sizeof(vcon_t) + scrollback);

  vc->vc_backend.vtable = &vcon_backend_vtable;
  vc->vc_name = name;
  mutex_init(&vc->vc_mutex, "vcon");
  task_waitable_init(&vc->vc_out_cond, "vconout");
  task_waitable_init(&vc->vc_in_cond, "vconin");
  LIST_INIT(&vc->vc_clients);

  vc->vc_sb.size = scrollback;

  if(input) {
    vc->vc_in = malloc(input);
    vc->vc_in_size = input;
  }

  LIST_INSERT_HEAD(&g_vcons, vc, vc_link);
  return vc;
}


__attribute__((noreturn))
static void *
vcon_shell_thread(void *arg)
{
  vcon_t *vc = arg;
  stream_t *s = vcon_backend(vc);
  while(1) {
    // Returns only on stream error; the backend never reports EOF, so this
    // normally blocks forever waiting for input from an attached client.
    cli_on_stream(s, '>');
  }
}


vcon_t *
vcon_create_shell(const char *name, size_t scrollback, size_t input)
{
  vcon_t *vc = vcon_create(name, scrollback, input);
  if(vc == NULL)
    return NULL;

  thread_create_shell(vcon_shell_thread, vc, name, vcon_backend(vc));
  return vc;
}


stream_t *
vcon_backend(vcon_t *vc)
{
  return &vc->vc_backend;
}


// Caller must hold vc_mutex. Resyncs the cursor if it fell behind the scrollback
// window and returns the number of bytes pending for this client.
static size_t
client_pending(vcon_t *vc, vcon_client_t *vcc)
{
  size_t oldest = vc->vc_total - vc->vc_sb.used;
  if(vcc->vcc_cursor < oldest)
    vcc->vcc_cursor = oldest; // fell behind the window; skip the lost gap
  return vc->vc_total - vcc->vcc_cursor;
}


vcon_client_t *
vcon_attach(vcon_t *vc, stream_t *term)
{
  // Runtime, user-triggered: tolerate OOM by returning NULL (callers handle it).
  vcon_client_t *vcc = xalloc(sizeof(vcon_client_t), 0, MEM_CLEAR | MEM_MAY_FAIL);
  if(vcc == NULL)
    return NULL;

  vcc->vcc_vc = vc;
  vcc->vcc_term = term;

  mutex_lock(&vc->vc_mutex);
  // Start at the oldest buffered byte so the first drain replays scrollback.
  vcc->vcc_cursor = vc->vc_total - vc->vc_sb.used;
  LIST_INSERT_HEAD(&vc->vc_clients, vcc, vcc_link);
  mutex_unlock(&vc->vc_mutex);

  return vcc;
}


void
vcon_detach(vcon_client_t *vcc)
{
  vcon_t *vc = vcc->vcc_vc;

  mutex_lock(&vc->vc_mutex);
  LIST_REMOVE(vcc, vcc_link);
  mutex_unlock(&vc->vc_mutex);

  free(vcc);
}


size_t
vcon_client_output(vcon_client_t *vcc, void *buf, size_t size)
{
  vcon_t *vc = vcc->vcc_vc;

  mutex_lock(&vc->vc_mutex);
  size_t avail = client_pending(vc, vcc);
  size_t n = MIN(avail, size);
  if(n) {
    vcon_ring_copy(&vc->vc_sb, avail, buf, n);
    vcc->vcc_cursor += n;
  }
  mutex_unlock(&vc->vc_mutex);
  return n;
}


__attribute__((noreturn))
static void *
vcon_bind_thread(void *arg)
{
  vcon_client_t *vcc = arg;
  vcon_t *vc = vcc->vcc_vc;
  stream_t *term = vcc->vcc_term;
  uint8_t buf[64];

  while(1) {
    int progress = 0;

    ssize_t r = stream_read(term, buf, sizeof(buf), 0); // non-blocking
    if(r > 0) {
      vcon_input(vc, buf, r);
      progress = 1;
    }

    size_t n = vcon_client_output(vcc, buf, sizeof(buf));
    if(n) {
      stream_write(term, buf, n, 0);
      progress = 1;
    }

    if(!progress)
      vcon_client_wait(vcc);
  }
}


void
vcon_bind(vcon_t *vc, stream_t *term)
{
  vcon_client_t *vcc = vcon_attach(vc, term);
  if(vcc == NULL)
    return;
  thread_create(vcon_bind_thread, vcc, 1024, "vconbind", TASK_DETACHED, 4);
}


void
vcon_client_wait(vcon_client_t *vcc)
{
  vcon_t *vc = vcc->vcc_vc;

  mutex_lock(&vc->vc_mutex);

  if(client_pending(vc, vcc) == 0) {
    // Atomically sleep until output is appended (vc_out_cond, broadcast under
    // vc_mutex -> no lost wakeup) or the terminal has input ready.
    pollset_t ps[2] = {
      { .obj = vcc->vcc_term,   .type = POLL_STREAM_READ },
      { .obj = &vc->vc_out_cond, .type = POLL_WAITABLE },
    };
    poll(ps, 2, &vc->vc_mutex, INT64_MAX);
  }

  mutex_unlock(&vc->vc_mutex);
}


size_t
vcon_input(vcon_t *vc, const void *buf, size_t len)
{
  const uint8_t *u8 = buf;

  mutex_lock(&vc->vc_mutex);

  size_t n = 0;
  while(n < len && vc->vc_in_used < vc->vc_in_size) {
    vc->vc_in[vc->vc_in_head] = u8[n++];
    vc->vc_in_head = (vc->vc_in_head + 1) % vc->vc_in_size;
    vc->vc_in_used++;
  }

  if(n)
    cond_signal(&vc->vc_in_cond);

  mutex_unlock(&vc->vc_mutex);
  return n;
}


vcon_t *
vcon_find(const char *name)
{
  vcon_t *vc;
  LIST_FOREACH(vc, &g_vcons, vc_link) {
    if(!strcmp(vc->vc_name, name))
      return vc;
  }
  return NULL;
}


vcon_t *
vcon_first(void)
{
  return LIST_FIRST(&g_vcons);
}


vcon_t *
vcon_next(vcon_t *vc)
{
  return LIST_NEXT(vc, vc_link);
}


const char *
vcon_name(const vcon_t *vc)
{
  return vc->vc_name;
}


size_t
vcon_scrollback_used(vcon_t *vc)
{
  mutex_lock(&vc->vc_mutex);
  size_t used = vc->vc_sb.used;
  mutex_unlock(&vc->vc_mutex);
  return used;
}


int
vcon_client_count(vcon_t *vc)
{
  int n = 0;
  mutex_lock(&vc->vc_mutex);
  vcon_client_t *vcc;
  LIST_FOREACH(vcc, &vc->vc_clients, vcc_link)
    n++;
  mutex_unlock(&vc->vc_mutex);
  return n;
}
