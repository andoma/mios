#include <mios/climate_zone.h>
#include <mios/type_macros.h>
#include <mios/task.h>
#include <mios/cli.h>
#include <mios/mios.h>
#include <mios/rpc.h>

#include <math.h>
#include <string.h>
#include <unistd.h>

SLIST_HEAD(climate_zone_slist, climate_zone);
static struct climate_zone_slist climate_zones;
static mutex_t climate_zone_mutex;


static void
climate_zone_alert_message(const struct alert_source *as, struct stream *output)
{
  climate_zone_t *cz = container_of(as, climate_zone_t, cz_alert);
  const climate_zone_class_t *czc = cz->cz_class;

  const float T = cz->cz_measured_temperature;

  const char *del = "";

  if(as->as_code & CLIMATE_ZONE_OT_ERROR) {
    stprintf(output, "%sOver-temperature error %.1f > %.1f",
             del, T, czc->czc_over_temp_error);
    del = ", ";
  } else if(as->as_code & CLIMATE_ZONE_OT_WARNING) {
    stprintf(output, "%sOver-temperature warning %.1f > %.1f",
             del, T, czc->czc_over_temp_warning);
    del = ", ";
  }

  if(as->as_code & CLIMATE_ZONE_UT_ERROR) {
    stprintf(output, "%sUnder-temperature error %.1f < %.1f",
             del, T, czc->czc_under_temp_error);
    del = ", ";
  } else if(as->as_code & CLIMATE_ZONE_UT_WARNING) {
    stprintf(output, "%sUnder-temperature warning %.1f < %.1f",
             del, T, czc->czc_under_temp_warning);
    del = ", ";
  }

  const float RH = cz->cz_measured_rh;

  if(as->as_code & CLIMATE_ZONE_ORH_ERROR) {
    stprintf(output, "%sOver-humidity error %.1f > %.1f",
             del, RH, czc->czc_over_rh_error);
    del = ", ";
  } else if(as->as_code & CLIMATE_ZONE_ORH_WARNING) {
    stprintf(output, "%sOver-humidity warning %.1f > %.1f",
             del, RH, czc->czc_over_rh_warning);
    del = ", ";
  }

  if(as->as_code & CLIMATE_ZONE_URH_ERROR) {
    stprintf(output, "%sUnder-humidity error %.1f < %.1f",
             del, RH, czc->czc_under_rh_error);
    del = ", ";
  } else if(as->as_code & CLIMATE_ZONE_URH_WARNING) {
    stprintf(output, "%sUnder-humidity warning %.1f < %.1f",
             del, RH, czc->czc_under_rh_warning);
    del = ", ";
  }


  const float RPM = cz->cz_measured_fan_rpm;

  if(as->as_code & CLIMATE_ZONE_OF_ERROR) {
    stprintf(output, "%sFan too fast error %.1f > %.1f",
             del, RPM, czc->czc_over_fan_rpm_error);
    del = ", ";
  } else if(as->as_code & CLIMATE_ZONE_OF_WARNING) {
    stprintf(output, "%sFan too fast warning %.1f > %.1f",
             del, RPM, czc->czc_over_fan_rpm_warning);
    del = ", ";
  }

  if(as->as_code & CLIMATE_ZONE_UF_ERROR) {
    stprintf(output, "%sFan too slow error %.1f < %.1f",
             del, RPM, czc->czc_under_fan_rpm_error);
    del = ", ";
  } else if(as->as_code & CLIMATE_ZONE_UF_WARNING) {
    stprintf(output, "%sFan too slow warning %.1f < %.1f",
             del, RPM, czc->czc_under_fan_rpm_warning);
    del = ", ";
  }

}


static event_level_t
climate_zone_alert_level(const struct alert_source *as)
{
  if(as->as_code & (CLIMATE_ZONE_OT_ERROR |
                    CLIMATE_ZONE_UT_ERROR |
                    CLIMATE_ZONE_ORH_ERROR |
                    CLIMATE_ZONE_URH_ERROR |
                    CLIMATE_ZONE_OF_ERROR |
                    CLIMATE_ZONE_UF_ERROR))
    return LOG_ERR;
  return LOG_WARNING;
}

__attribute__((always_inline))
static inline void
setclr(uint32_t *set, uint32_t *clr, int shift,
       float v,
       float oe,
       float ow,
       float uw,
       float ue,
       float h)
{
  if(v > oe + h)
    *set |= 1 << shift;
  if(v < oe - h)
    *clr |= 1 << shift;
  if(v > ow + h)
    *set |= 2 << shift;
  if(v < ow - h)
    *clr |= 2 << shift;
  if(v < uw - h)
    *set |= 4 << shift;
  if(v > uw + h)
    *clr |= 4 << shift;
  if(v < ue - h)
    *set |= 8 << shift;
  if(v > ue + h)
    *clr |= 8 << shift;
}



void
climate_zone_refresh_alert(climate_zone_t *cz)
{
  const climate_zone_class_t *czc = cz->cz_class;

  uint32_t set = 0;
  uint32_t clr = 0;

  setclr(&set, &clr, 0, cz->cz_measured_temperature,
         czc->czc_over_temp_error,
         czc->czc_over_temp_warning,
         czc->czc_under_temp_warning,
         czc->czc_under_temp_error,
         czc->czc_temp_hysteresis);

  uint32_t code = (cz->cz_alert.as_code | set) &~ clr;
  alert_set(&cz->cz_alert, code);
}

static void
climate_zone_alert_refcount(struct alert_source *as, int value)
{
  climate_zone_t *cz = container_of(as, climate_zone_t, cz_alert);
  if(cz->cz_class->czc_refcount)
    cz->cz_class->czc_refcount(cz, value);
}


static const alert_class_t climate_zone_alert_class = {
  .ac_message = climate_zone_alert_message,
  .ac_level = climate_zone_alert_level,
  .ac_refcount = climate_zone_alert_refcount,
};


static void
climate_zone_refcount(climate_zone_t *cz, int value)
{
  if(cz->cz_class->czc_refcount != NULL)
    cz->cz_class->czc_refcount(cz, value);
}


static int
climate_zone_cmp(const climate_zone_t *a, const climate_zone_t *b)
{
  return strcmp(a->cz_name, b->cz_name);
}


void
climate_zone_register(climate_zone_t *cz, const climate_zone_class_t *czc,
                      const char *name)
{
  cz->cz_name = name;

  cz->cz_measured_temperature = NAN;
  cz->cz_measured_rh = NAN;
  cz->cz_measured_fan_rpm = NAN;
  cz->cz_class = czc;

  mutex_lock(&climate_zone_mutex);
  SLIST_INSERT_SORTED(&climate_zones, cz, cz_link, climate_zone_cmp);
  mutex_unlock(&climate_zone_mutex);

  alert_register(&cz->cz_alert, &climate_zone_alert_class, name);
}

void
climate_zone_unregister(climate_zone_t *cz)
{
  panic("climate_zone_unregister() not implemented, "
        "please also fix climate_zone_get_next()");
}



void
climate_zone_set_measured_temperature(climate_zone_t *cz, float temperature)
{
  cz->cz_measured_temperature = temperature;
}

void
climate_zone_set_measured_rh(climate_zone_t *cz, float rh)
{
  cz->cz_measured_rh = rh;
}

void
climate_zone_set_measured_fan_rpm(climate_zone_t *cz, float rpm)
{
  cz->cz_measured_fan_rpm = rpm;
}



__attribute__((noinline))
climate_zone_t *
climate_zone_get_next(climate_zone_t *cur)
{
  mutex_lock(&climate_zone_mutex);
  /* TODO: If we ever remove climate_zones
     this must be rewritten as how device_get_next() works
  */
  climate_zone_t *cz = cur ? SLIST_NEXT(cur, cz_link) : SLIST_FIRST(&climate_zones);
  if(cz)
    climate_zone_refcount(cz, 1);
  mutex_unlock(&climate_zone_mutex);
  if(cur)
    climate_zone_refcount(cur, -1);
  return cz;
}


static error_t
cmd_climate(cli_t *cli, int argc, char **argv)
{
  climate_zone_t *cz = NULL;

  cli_printf(cli, "Name                °C        RH        Fan Speed\n");
  cli_printf(cli, "=================================================\n");
  while((cz = climate_zone_get_next(cz)) != NULL) {
    cli_printf(cli, "%-20s", cz->cz_name);

    if(isfinite(cz->cz_measured_temperature))
      cli_printf(cli, "%-10.2f", cz->cz_measured_temperature);
    else
      cli_printf(cli, "%-10s", "-");

    if(isfinite(cz->cz_measured_rh))
      cli_printf(cli, "%-10.2f", cz->cz_measured_rh);
    else
      cli_printf(cli, "%-10s", "-");

    if(isfinite(cz->cz_measured_fan_rpm))
      cli_printf(cli, "%-10.2f", cz->cz_measured_fan_rpm);
    else
      cli_printf(cli, "%-10s", "-");
    cli_printf(cli, "\n");
  }
  return 0;
}

CLI_CMD_DEF("climate", cmd_climate);

