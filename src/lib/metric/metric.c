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
  m->min = INFINITY;
  m->max = -INFINITY;
  m->mean = 0;
  m->m2 = 0;
  m->count = 0;
  m->enabled = enable;
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



static error_t
cmd_metric(cli_t *cli, int argc, char **argv)
{
  const metric_t *m;
  cli_printf(cli, "Name             Mean       Min        Max        Stddev     Samples\n");

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

    cli_printf(cli, "%-16s %-10f %-10f %-10f %-10f %d\n",
               md->name,
               mean,
               min,
               max,
               stddev,
               count);
  }
  return 0;
}

CLI_CMD_DEF("metric", cmd_metric);

