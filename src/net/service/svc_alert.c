#include <mios/service.h>
#include <mios/alert.h>
#include <mios/task.h>
#include <mios/stream.h>
#include <mios/ghook.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#include <sys/queue.h>

#include <net/pbuf.h>

LIST_HEAD(alert_pub_list, alert_pub);

#define ALERT_PUB_BEGIN 1
#define ALERT_PUB_RAISE 2
#define ALERT_PUB_END 3

typedef struct alert_pub {
  LIST_ENTRY(alert_pub) ap_link;
  pushpull_t *ap_pp;
  pbuf_t *ap_pbuf;
  cond_t ap_cond;
  uint8_t ap_refresh;
  uint8_t ap_stop;
} alert_pub_t;

static mutex_t alert_pub_mutex = MUTEX_INITIALIZER("alert");
static struct alert_pub_list pubs;


typedef struct {
  struct stream s;
  pbuf_t *pb;
  pushpull_t *pp;
} stream_to_pbuf_t;

static ssize_t
stream_to_pbuf_write(struct stream *s, const void *buf, size_t size, int flags)
{
  stream_to_pbuf_t *stp = (stream_to_pbuf_t *)s;
  stp->pb = pbuf_write(stp->pb, buf, size, stp->pp, 1);
  return size;
}

static const stream_vtable_t stream_to_pbuf = {
  .write = stream_to_pbuf_write
};


static int
alert_pub_send(alert_pub_t *ap, pbuf_t *pb)
{
  mutex_lock(&alert_pub_mutex);
  assert(ap->ap_pbuf == NULL);
  ap->ap_pbuf = pb;
  while(ap->ap_pbuf && !ap->ap_stop) {
    cond_wait(&ap->ap_cond, &alert_pub_mutex);
  }
  return ap->ap_stop;
}

static int
alert_pub_send_cmd(alert_pub_t *ap, int cmd)
{
  mutex_unlock(&alert_pub_mutex);
  pbuf_t *pb = pbuf_make(ap->ap_pp->preferred_offset, 1);
  uint8_t *u8 = pbuf_append(pb, 1);
  u8[0] = cmd;
  return alert_pub_send(ap, pb);
}

static void
alert_pub_raise(alert_pub_t *ap, const alert_source_t *as)
{
  stream_to_pbuf_t stp;
  stp.s.vtable = &stream_to_pbuf;
  stp.pp = ap->ap_pp;
  mutex_unlock(&alert_pub_mutex);
  stp.pb = pbuf_make(ap->ap_pp->preferred_offset, 1);
  uint8_t *u8 = pbuf_append(stp.pb, 3);
  const size_t keylen = strlen(as->as_key);

  u8[0] = ALERT_PUB_RAISE;
  u8[1] = as->as_class->ac_level(as);
  u8[2] = keylen;
  stp.pb = pbuf_write(stp.pb, as->as_key, keylen, ap->ap_pp, 1);
  as->as_class->ac_message(as, &stp.s);
  alert_pub_send(ap, stp.pb);
}

__attribute__((noreturn))
static void *
alert_pub_thread(void *arg)
{
  alert_pub_t *ap = arg;

  mutex_lock(&alert_pub_mutex);

  while(1) {
    if(ap->ap_stop)
      break;

    if(!ap->ap_refresh) {
      cond_wait(&ap->ap_cond, &alert_pub_mutex);
      continue;
    }

    // Clear at once as we will unlock and need to redo
    // if something changes.
    ap->ap_refresh = 0;

    if(alert_pub_send_cmd(ap, ALERT_PUB_BEGIN))
      break;

    alert_source_t *as = NULL;
    while((as = alert_get_next(as)) != NULL) {

      // There is no public release method for alert_source
      // so wee must iterate all, even if we're going to stop
      if(ap->ap_stop)
        continue;

      if(!as->as_code)
        continue; // Not raised at the moment

      alert_pub_raise(ap, as);
    }

    if(alert_pub_send_cmd(ap, ALERT_PUB_END))
      break;
  }

  LIST_REMOVE(ap, ap_link);
  mutex_unlock(&alert_pub_mutex);
  ap->ap_pp->net->event(ap->ap_pp->net_opaque, PUSHPULL_EVENT_CLOSE);

  if(ap->ap_pbuf)
    pbuf_free(ap->ap_pbuf);

  free(ap);
  thread_exit(NULL);
}


static void
alert_pub_alert_updated(ghook_type_t type)
{
  if(type != GHOOK_ALERT_UPDATED)
    return;

  alert_pub_t *ap;

  mutex_lock(&alert_pub_mutex);
  LIST_FOREACH(ap, &pubs, ap_link) {
    ap->ap_refresh = 1;
    cond_signal(&ap->ap_cond);
  }
  mutex_unlock(&alert_pub_mutex);
}

GHOOK(alert_pub_alert_updated);


static pbuf_t *
ap_pull(void *opaque)
{
  alert_pub_t *ap = opaque;
  pbuf_t *pb;

  mutex_lock(&alert_pub_mutex);
  pb = ap->ap_pbuf;
  ap->ap_pbuf = NULL;
  cond_signal(&ap->ap_cond);
  mutex_unlock(&alert_pub_mutex);
  return pb;
}

static void
ap_close(void *opaque, const char *reason)
{
  alert_pub_t *ap = opaque;

  mutex_lock(&alert_pub_mutex);
  ap->ap_stop = 1;
  cond_signal(&ap->ap_cond);
  mutex_unlock(&alert_pub_mutex);
}

static const pushpull_app_fn_t alert_pp_fn = {
  .pull = ap_pull,
  .close = ap_close
};


static error_t
alert_open(pushpull_t *pp)
{
  alert_pub_t *ap = xalloc(sizeof(alert_pub_t), 0, MEM_CLEAR | MEM_MAY_FAIL);
  if(ap == NULL)
    return ERR_NO_MEMORY;

  pp->app_opaque = ap;
  pp->app = &alert_pp_fn;

  ap->ap_refresh = 1;
  ap->ap_pp = pp;
  cond_init(&ap->ap_cond, "alert");

  thread_create(alert_pub_thread, ap, 0, "alertpub", TASK_DETACHED, 5);

  mutex_lock(&alert_pub_mutex);
  LIST_INSERT_HEAD(&pubs, ap, ap_link);
  mutex_unlock(&alert_pub_mutex);
  return 0;
}


SERVICE_DEF_PUSHPULL("alert", 0, 0, alert_open);
