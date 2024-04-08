#include <mios/metric.h>
#include <mios/cli.h>

#include <sys/param.h>

#include <math.h>
#include "irq.h"

struct metric_slist metrics;

void
metric_reset(metric_t *m, int enable)
{
  int q = irq_forbid(m->def->irq_level);
  m->mean = 0;
  m->m2 = 0;
  m->count = 0;
  m->enabled = enable;
  m->alert_lockout = m->def->alert_lockout_duration;
  irq_permit(q);
}

void
metric_init(metric_t *m, const metric_def_t *def, uint8_t enabled)
{
  m->def = def;
  m->min = INFINITY;
  m->max = -INFINITY;
  m->enabled = enabled;
  SLIST_INSERT_HEAD(&metrics, m, link);
}

void
metric_collect(metric_t *m, float v)
{
  if(!m->enabled)
    return;
  m->min = MIN(m->min, v);
  m->max = MAX(m->max, v);

  m->count++;
  float delta = v - m->mean;
  m->mean += delta / m->count;
  float delta2 = v - m->mean;
  m->m2 += delta * delta2;
}


int
metric_update_fault(metric_t *m, float v)
{
  const metric_def_t *md = m->def;

  uint8_t clr = 0;
  uint8_t set = 0;

  if(v > md->hi_error + md->hysteresis) {
    set |= METRIC_HI_ERROR;
  } else if(v < md->hi_error - md->hysteresis) {
    clr |= METRIC_HI_ERROR;
  }

  if(v > md->hi_warning + md->hysteresis) {
    set |= METRIC_HI_WARNING;
  } else if(v < md->hi_warning - md->hysteresis) {
    clr |= METRIC_HI_WARNING;
  }

  if(v < md->lo_warning - md->hysteresis) {
    set |= METRIC_LO_WARNING;
  } else if(v > md->lo_warning + md->hysteresis) {
    clr |= METRIC_LO_WARNING;
  }

  if(v < md->lo_error - md->hysteresis) {
    set |= METRIC_LO_ERROR;
  } else if(v > md->lo_error + md->hysteresis) {
    clr |= METRIC_LO_ERROR;
  }

  if(m->enabled && !m->alert_lockout) {
    set |= set << 4;
    clr |= clr << 4;
  }

  uint8_t new_bits = (m->raised_alerts | set) & ~clr;
  uint8_t changed = new_bits ^ m->raised_alerts;
  m->raised_alerts = new_bits;
  return changed;
}


static error_t
cmd_metric(cli_t *cli, int argc, char **argv)
{
  const metric_t *m;
  cli_printf(cli, "Name         Mean       Min        Max        Stddev     Samples\n");

  SLIST_FOREACH(m, &metrics, link) {
    if(!m->enabled)
      continue;
    const metric_def_t *md = m->def;

    int q = irq_forbid(md->irq_level);
    float min = m->min;
    float max = m->max;
    float mean = m->mean;
    float m2 = m->m2;
    unsigned int count = m->count;
    irq_permit(q);

    float var = m2 / count;
    float stddev = sqrtf(var);

    cli_printf(cli, "%-10s %c %-10f %-10f %-10f %-10f %d %c%c%c%c\n",
               md->name,
               md->unit,
               mean,
               min,
               max,
               stddev,
               count,
               m->raised_alerts & METRIC_HI_ERROR ? 'H' : ' ',
               m->raised_alerts & METRIC_HI_WARNING ? 'h' : ' ',
               m->raised_alerts & METRIC_LO_WARNING ? 'l' : ' ',
               m->raised_alerts & METRIC_LO_ERROR ? 'L' : ' ');
  }
  return 0;
}

CLI_CMD_DEF("metric", cmd_metric);

