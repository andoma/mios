#pragma once

#include "peer.h"

typedef struct scenario_opts {
  double duration_scale;   /* multiplies every scenario's nominal duration */
  uint64_t seed;
} scenario_opts_t;

typedef void (*scenario_fn)(peer_t *p, const scenario_opts_t *o);

typedef struct scenario {
  const char *name;
  const char *tags;        /* comma separated: quick,long,faults,... */
  const char *desc;
  scenario_fn fn;
} scenario_t;

extern const scenario_t scenarios[];
extern const int num_scenarios;
