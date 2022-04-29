#include <mios/timer.h>

static int
timer_cmp(const timer_t *a, const timer_t *b)
{
  return a->t_expire > b->t_expire;
}


void
timer_arm_on_queue(timer_t *t, uint64_t expire, struct timer_list *tl)
{
  if(t->t_expire)
    LIST_REMOVE(t, t_link);

  t->t_expire = expire;
  LIST_INSERT_SORTED(tl, t, t_link, timer_cmp);
}


int
timer_disarm(timer_t *t)
{
  if(!t->t_expire)
    return 1;
  LIST_REMOVE(t, t_link);
  t->t_expire = 0;
  return 0;
}


void
timer_dispatch(struct timer_list *tl, uint64_t now)
{
  while(1) {
    timer_t *t = LIST_FIRST(tl);
    if(t == NULL)
      break;
    uint64_t expire = t->t_expire;
    if(expire > now)
      break;
    LIST_REMOVE(t, t_link);
    t->t_expire = 0;
    t->t_cb(t->t_opaque, expire);
  }
}


void
timer_init(timer_t *t, void (*cb)(void *opaque, uint64_t expire),
           void *opaque, const char *name, uint64_t deadline)
{
  t->t_cb = cb;
  t->t_opaque = opaque;
  t->t_name = name;
  timer_arm_abs(t, deadline);
}
