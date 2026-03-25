#include <mios/device.h>
#include <mios/cli.h>
#include <mios/task.h>
#include <mios/mios.h>
#include <mios/stream.h>

#include <string.h>
#include <stdio.h>

#include "irq.h"

static STAILQ_HEAD(, device) devices = STAILQ_HEAD_INITIALIZER(devices);
static mutex_t devs_mutex = MUTEX_INITIALIZER("devss");


void
device_register(device_t *d)
{
  atomic_set(&d->d_refcount, 1);

  mutex_lock(&devs_mutex);
  STAILQ_INSERT_TAIL(&devices, d, d_link);
  mutex_unlock(&devs_mutex);
}

void
device_unregister(device_t *d)
{
  mutex_lock(&devs_mutex);
  STAILQ_REMOVE(&devices, d, device, d_link);
  mutex_unlock(&devs_mutex);
  device_release(d);
}


void
device_release(device_t *d)
{
  if(atomic_dec(&d->d_refcount))
    return;

  if(d->d_class->dc_dtor == NULL)
    panic("Release device %s with no dtor", d->d_name);
  d->d_class->dc_dtor(d);
}


void
device_retain(device_t *d)
{
  atomic_inc(&d->d_refcount);
}


device_t *
device_get_next(device_t *cur)
{
  device_t *d;
  mutex_lock(&devs_mutex);

  if(cur == NULL) {
    d = STAILQ_FIRST(&devices);
  } else {
    STAILQ_FOREACH(d, &devices, d_link) {
      if(d == cur) {
        break;
      }
    }
    if(d)
      d = STAILQ_NEXT(d, d_link);
  }

  if(d)
    device_retain(d);
  mutex_unlock(&devs_mutex);
  if(cur)
    device_release(cur);
  return d;
}


// Prefixed stream wrapper — intercepts writes and prepends a tree
// indentation prefix + 2 spaces at the start of each line.

typedef struct {
  stream_t s;
  stream_t *inner;
  char prefix[64];
  int prefix_len;
  int at_sol;
} prefixed_stream_t;

static ssize_t
prefixed_write(stream_t *s, const void *buf, size_t size, int flags)
{
  prefixed_stream_t *ps = (prefixed_stream_t *)s;
  const char *p = buf;
  size_t remaining = size;

  while(remaining > 0) {
    if(ps->at_sol) {
      stream_write(ps->inner, ps->prefix, ps->prefix_len, 0);
      ps->at_sol = 0;
    }

    // Find next newline
    const char *nl = NULL;
    for(size_t i = 0; i < remaining; i++) {
      if(p[i] == '\n') { nl = p + i; break; }
    }
    if(nl) {
      size_t chunk = nl - p + 1;
      stream_write(ps->inner, p, chunk, 0);
      p += chunk;
      remaining -= chunk;
      ps->at_sol = 1;
    } else {
      stream_write(ps->inner, p, remaining, 0);
      remaining = 0;
    }
  }
  return size;
}

static const stream_vtable_t prefixed_stream_vtable = {
  .write = prefixed_write,
};

static void
dev_print_node(prefixed_stream_t *ps, device_t *d,
               const char *connector)
{
  stprintf(ps->inner, "%.*s%s\033[1m%s [%s]\033[0m%s\n",
           ps->prefix_len, ps->prefix,
           connector, d->d_name,
           d->d_class->dc_class_name ?: "<unset>",
           d->d_flags & DEVICE_F_DEBUG ? " +debug" : "");
}

static int
dev_count_children(device_t *parent)
{
  device_t *d;
  int count = 0;
  mutex_lock(&devs_mutex);
  STAILQ_FOREACH(d, &devices, d_link) {
    if(d->d_parent == parent)
      count++;
  }
  mutex_unlock(&devs_mutex);
  return count;
}

// Like device_get_next() but only returns children of 'parent'.
// parent==NULL returns root devices (d_parent==NULL).
static device_t *
device_get_next_child(device_t *parent, device_t *cur)
{
  device_t *d;
  mutex_lock(&devs_mutex);

  if(cur == NULL) {
    d = STAILQ_FIRST(&devices);
  } else {
    STAILQ_FOREACH(d, &devices, d_link) {
      if(d == cur)
        break;
    }
    if(d)
      d = STAILQ_NEXT(d, d_link);
  }

  // Skip until we find a child of parent
  while(d && d->d_parent != parent)
    d = STAILQ_NEXT(d, d_link);

  if(d)
    device_retain(d);
  mutex_unlock(&devs_mutex);
  if(cur)
    device_release(cur);
  return d;
}

// UTF-8 box drawing bytes
#define BOX_VERT      "\xe2\x94\x82"       // │
#define BOX_VERT_RIGHT "\xe2\x94\x9c"      // ├
#define BOX_UP_RIGHT   "\xe2\x94\x94"      // └
#define BOX_HORIZ      "\xe2\x94\x80"      // ─

static void
dev_print_info(prefixed_stream_t *ps, device_t *d)
{
  if(!d->d_class->dc_print_info)
    return;

  int prev_len = ps->prefix_len;
  int has_children = dev_count_children(d);

  // Info lines get "│ " prefix if device has children, else "  "
  if(ps->prefix_len + 5 <= (int)sizeof(ps->prefix)) {
    if(has_children) {
      memcpy(ps->prefix + ps->prefix_len, BOX_VERT " ", 4);
      ps->prefix_len += 4;
    } else {
      memcpy(ps->prefix + ps->prefix_len, "  ", 2);
      ps->prefix_len += 2;
    }
  }

  ps->at_sol = 1;
  d->d_class->dc_print_info(d, &ps->s);
  stprintf(ps->inner, "%.*s\n", ps->prefix_len, ps->prefix);

  ps->prefix_len = prev_len;
}

static void
dev_print_tree(prefixed_stream_t *ps, device_t *parent)
{
  int count = dev_count_children(parent);

  device_t *d = NULL;
  int idx = 0;
  while((d = device_get_next_child(parent, d)) != NULL) {

    idx++;
    int is_last = (idx == count);

    dev_print_node(ps, d, is_last ? BOX_UP_RIGHT BOX_HORIZ " "
                                  : BOX_VERT_RIGHT BOX_HORIZ " ");

    int prev_len = ps->prefix_len;

    // Continuation prefix for children: "│  " or "   "
    if(ps->prefix_len + 5 <= (int)sizeof(ps->prefix)) {
      if(is_last) {
        memcpy(ps->prefix + ps->prefix_len, "   ", 3);
        ps->prefix_len += 3;
      } else {
        memcpy(ps->prefix + ps->prefix_len, BOX_VERT "  ", 5);
        ps->prefix_len += 5;
      }
    }

    dev_print_info(ps, d);
    dev_print_tree(ps, d);

    ps->prefix_len = prev_len;
  }
}

static error_t
cmd_dev(cli_t *cli, int argc, char **argv)
{
  device_t *d = NULL;
  if(argc == 3) {
    while((d = device_get_next(d)) != NULL) {
      if(strcmp(d->d_name, argv[1]))
        continue;
      const char *cmd = argv[2];
      if(!strcmp(cmd, "+debug"))
        d->d_flags |= DEVICE_F_DEBUG;
      if(!strcmp(cmd, "-debug"))
        d->d_flags &= ~DEVICE_F_DEBUG;
    }
    return 0;
  }

  prefixed_stream_t ps = {
    .s.vtable = &prefixed_stream_vtable,
    .inner = cli->cl_stream,
    .at_sol = 1,
  };

  d = NULL;
  while((d = device_get_next_child(NULL, d)) != NULL) {
    if(argc == 2 && strcmp(d->d_name, argv[1]))
      continue;

    ps.prefix_len = 0;
    dev_print_node(&ps, d, "");
    dev_print_info(&ps, d);
    dev_print_tree(&ps, d);
  }
  return 0;
}

CLI_CMD_DEF("dev", cmd_dev);


void
device_power_state(device_power_state_t state)
{
  device_t *d;
  STAILQ_FOREACH(d, &devices, d_link) {
    if(d->d_class->dc_power_state)
      d->d_class->dc_power_state(d, state);
  }
}


error_t
device_shutdown(device_t *parent)
{
  device_t *d;
  STAILQ_FOREACH(d, &devices, d_link) {
    if(d->d_parent == parent && d->d_class->dc_shutdown) {
      error_t err = d->d_class->dc_shutdown(d);
      if(err)
        return err;
    }
  }
  return 0;
}
