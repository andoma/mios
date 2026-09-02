#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "peer.h"
#include "tst.h"

#include <stdio.h>
#include <string.h>
#include <syslog.h>

static const char *
level_name(int level)
{
  switch(level & 7) {
  case LOG_EMERG:   return "EMERG";
  case LOG_ALERT:   return "ALERT";
  case LOG_CRIT:    return "CRIT";
  case LOG_ERR:     return "ERR";
  case LOG_WARNING: return "WARN";
  case LOG_NOTICE:  return "NOTICE";
  case LOG_INFO:    return "INFO";
  case LOG_DEBUG:   return "DEBUG";
  }
  return "?";
}

void
peer_init_common(peer_t *p)
{
  pthread_mutex_init(&p->log_mutex, NULL);
}

void
peer_log_cb(void *opaque, int level, const char *msg)
{
  peer_t *p = opaque;
  level &= 7;
  pthread_mutex_lock(&p->log_mutex);
  p->log_count[level]++;
  snprintf(p->log_last[level], sizeof(p->log_last[level]), "%s", msg);
  pthread_mutex_unlock(&p->log_mutex);
  tst_verbosef("vllp[%s]: %s", level_name(level), msg);
}

void
peer_log_reset(peer_t *p)
{
  pthread_mutex_lock(&p->log_mutex);
  memset(p->log_count, 0, sizeof(p->log_count));
  pthread_mutex_unlock(&p->log_mutex);
}

int
peer_log_count_at_most(peer_t *p, int max_level)
{
  int n = 0;
  pthread_mutex_lock(&p->log_mutex);
  for(int i = 0; i <= max_level && i < 8; i++)
    n += p->log_count[i];
  pthread_mutex_unlock(&p->log_mutex);
  return n;
}

const char *
peer_log_last(peer_t *p, int level)
{
  return p->log_last[level & 7];
}

void
peer_set_faults(peer_t *p, int dir, const fault_cfg_t *cfg)
{
  linksim_set_faults(p->ls, dir, cfg);
}

void
peer_set_faults_both(peer_t *p, const fault_cfg_t *cfg)
{
  linksim_set_faults(p->ls, PEER_DIR_C2S, cfg);
  linksim_set_faults(p->ls, PEER_DIR_S2C, cfg);
}

void
peer_clear_faults(peer_t *p)
{
  linksim_set_faults(p->ls, PEER_DIR_C2S, NULL);
  linksim_set_faults(p->ls, PEER_DIR_S2C, NULL);
}

int
peer_wait_connected(peer_t *p, int64_t timeout_us)
{
  int64_t deadline = tst_now_us() + timeout_us;
  while(!vllp_is_connected(p->v)) {
    if(tst_now_us() > deadline)
      return -1;
    tst_sleep_us(5000);
  }
  return 0;
}
