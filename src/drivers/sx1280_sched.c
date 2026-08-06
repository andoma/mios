#include "sx1280_i.h"
#include "sx1280_sched.h"

#include <mios/task.h>
#include <mios/eventlog.h>

#include <unistd.h>

// Insert sorted by ss_time (head = earliest)
static void
sched_insert(sx1280_t *s, sx1280_slot_t *slot, int64_t time)
{
  slot->ss_time = time;
  slot->ss_queued = 1;

  sx1280_slot_t *cur = LIST_FIRST(&s->sched_slots);
  if(cur == NULL || time < cur->ss_time) {
    LIST_INSERT_HEAD(&s->sched_slots, slot, ss_link);
    return;
  }
  while(LIST_NEXT(cur, ss_link) != NULL &&
        LIST_NEXT(cur, ss_link)->ss_time <= time)
    cur = LIST_NEXT(cur, ss_link);
  LIST_INSERT_AFTER(cur, slot, ss_link);
}

// Highest-priority slot that is due, unless a higher-priority slot
// would become due before it completes. Returns NULL and sets
// *wakeup to the next interesting time if nothing is runnable.
static sx1280_slot_t *
sched_pick(sx1280_t *s, int64_t now, int64_t *wakeup)
{
  sx1280_slot_t *best = NULL, *ss;

  *wakeup = INT64_MAX;

  LIST_FOREACH(ss, &s->sched_slots, ss_link) {
    if(ss->ss_time > now) {
      if(ss->ss_time < *wakeup)
        *wakeup = ss->ss_time;
      continue;
    }
    if(best == NULL || ss->ss_prio > best->ss_prio)
      best = ss;
  }

  if(best == NULL)
    return NULL;

  LIST_FOREACH(ss, &s->sched_slots, ss_link) {
    if(ss != best && ss->ss_prio > best->ss_prio &&
       ss->ss_time < now + best->ss_duration) {
      // Runnable, but would collide with a more important slot
      *wakeup = ss->ss_time;
      return NULL;
    }
  }
  return best;
}

__attribute__((noreturn))
static void *
sched_thread(void *arg)
{
  sx1280_t *s = arg;

  error_t err = sx1280_sched_recover(s);
  if(err)
    evlog(LOG_ERR, "%s: radio not responding: %s", s->name,
          error_to_string(err));

  mutex_lock(&s->sched_mutex);
  while(1) {
    int64_t now = clock_get();
    int64_t wakeup;
    sx1280_slot_t *slot = sched_pick(s, now, &wakeup);

    if(slot == NULL) {
      if(wakeup == INT64_MAX)
        cond_wait(&s->sched_cond, &s->sched_mutex);
      else if(cond_wait_timeout(&s->sched_cond, &s->sched_mutex, wakeup)) {}
      continue;
    }

    LIST_REMOVE(slot, ss_link);
    slot->ss_queued = 0;
    s->sched_cancelled = NULL;
    mutex_unlock(&s->sched_mutex);

    const int64_t next = slot->ss_execute(slot, s, now);

    mutex_lock(&s->sched_mutex);
    if(next > 0 && !slot->ss_queued && s->sched_cancelled != slot)
      sched_insert(s, slot, next);
  }
}

void
sx1280_sched_submit(sx1280_t *s, sx1280_slot_t *slot, int64_t time)
{
  mutex_lock(&s->sched_mutex);
  if(!slot->ss_queued) {
    sched_insert(s, slot, time);
    cond_signal(&s->sched_cond);
  }
  mutex_unlock(&s->sched_mutex);
}

void
sx1280_sched_cancel(sx1280_t *s, sx1280_slot_t *slot)
{
  mutex_lock(&s->sched_mutex);
  if(slot->ss_queued) {
    LIST_REMOVE(slot, ss_link);
    slot->ss_queued = 0;
  } else {
    // May be executing right now; block its re-submission
    s->sched_cancelled = slot;
  }
  mutex_unlock(&s->sched_mutex);
}

int
sx1280_sched_set_mode(sx1280_t *s, const void *token)
{
  const int changed = s->sched_mode != token;
  s->sched_mode = token;
  return changed;
}

error_t
sx1280_sched_recover(sx1280_t *s)
{
  s->sched_mode = NULL;
  return sx1280_reset(s);
}

void
sx1280_sched_init(sx1280_t *s)
{
  mutex_init(&s->sched_mutex, "radiosched");
  task_waitable_init(&s->sched_cond, "radiosched");
  LIST_INIT(&s->sched_slots);
  // Above everything else including the net thread (prio 10):
  // connection-event anchors are the hardest deadline in the system
  thread_create(sched_thread, s, 1024, "radio", 0, 11);
}
