#include <mios/metric.h>
#include <mios/cli.h>

#include <sys/param.h>

#include <math.h>
#include "irq.h"

struct metric_slist metrics;

void
metric_init(metric_t *m, const metric_def_t *def)
{
  m->def = def;
  m->min = INFINITY;
  m->max = -INFINITY;
  SLIST_INSERT_HEAD(&metrics, m, link);
}

void
metric_collect(metric_t *m, float v)
{
  m->min = MIN(m->min, v);
  m->max = MAX(m->max, v);
  m->sum += v;
  m->sumsum += v * v;
  m->count++;
}



static error_t
cmd_metric(cli_t *cli, int argc, char **argv)
{
  const metric_t *m;
  cli_printf(cli, "Name             Mean       Min        Max        Stddev     Samples\n");

  SLIST_FOREACH(m, &metrics, link) {
    const metric_def_t *md = m->def;

    int q = irq_forbid(md->irq_level);
    float min = m->min;
    float max = m->max;
    float sum = m->sum;
    float sumsum = m->sumsum;
    unsigned int count = m->count;
    irq_permit(q);

    float mean = sum / count;
    float var = (sumsum - sum * sum / count) / count;
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

