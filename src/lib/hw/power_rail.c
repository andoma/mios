#include <mios/power_rail.h>
#include <mios/type_macros.h>
#include <mios/task.h>
#include <mios/cli.h>
#include <mios/mios.h>
#include <mios/rpc.h>

#include <math.h>
#include <string.h>
#include <unistd.h>

SLIST_HEAD(power_rail_slist, power_rail);
static struct power_rail_slist power_rails;
static mutex_t power_rail_mutex;


static void
power_rail_alert_message(struct alert_source *as, struct stream *output)
{
  power_rail_t *pr = container_of(as, power_rail_t, pr_alert);
  const power_rail_class_t *prc = pr->pr_class;

  const char *del = "";

  if(as->as_code & POWER_RAIL_OFF) {
    stprintf(output, "%sAdministratively disabled", del);
    del = ", ";
  }

  if(as->as_code & POWER_RAIL_FAULT) {
    stprintf(output, "%sFault detected by hardware", del);
    del = ", ";
  }

  if(as->as_code & POWER_RAIL_TEMP) {
    stprintf(output, "%sOver-temperature", del);
    del = ", ";
  }

  if(as->as_code & POWER_RAIL_FAN) {
    stprintf(output, "%sOver-temperature", del);
    del = ", ";
  }

  const float V = pr->pr_measured_voltage;
  const float I = pr->pr_measured_current;

  if(as->as_code & POWER_RAIL_OV_ALERT) {
    stprintf(output, "%sOver-voltage %.2f V > %.2f V Nominal:%.2f V",
             del,
             V, prc->prc_voltage_nominal + prc->prc_voltage_alert_deviation,
             prc->prc_voltage_nominal);
    del = ", ";
  }

  if(as->as_code & POWER_RAIL_UV_ALERT) {
    stprintf(output, "%sUnder-voltage %.2f V < %.2f V Nominal:%.2f V",
             del,
             V, prc->prc_voltage_nominal - prc->prc_voltage_alert_deviation,
             prc->prc_voltage_nominal);
    del = ", ";
  }

  if(as->as_code & POWER_RAIL_OC_ALERT) {
    stprintf(output, "%sOver-current %.3f A > %.3f A",
             del, I, prc->prc_over_current_alert);
    del = ", ";
  }

  if(as->as_code & POWER_RAIL_UC_ALERT) {
    stprintf(output, "%sUnder-current %.3f A < %.3f ",
             del, I, prc->prc_under_current_alert);
    del = ", ";
  }

  if(as->as_code & POWER_RAIL_OC_WARNING) {
    stprintf(output, "%sOver-current %.3f A > %.3f A",
             del, I, prc->prc_over_current_warning);
    del = ", ";
  }

  if(as->as_code & POWER_RAIL_UC_WARNING) {
    stprintf(output, "%sUnder-current %.3f A < %.3f A",
             del, I, prc->prc_under_current_warning);
    del = ", ";
  }

  if(as->as_code & POWER_RAIL_OV_WARNING) {
    stprintf(output, "%sOver-voltage %.2f V > %.2f V Nominal:%.2f V",
             del,
             V, prc->prc_voltage_nominal + prc->prc_voltage_warning_deviation,
             prc->prc_voltage_nominal);
    del = ", ";
  }

  if(as->as_code & POWER_RAIL_UV_WARNING) {
    stprintf(output, "%sUnder-voltage %.2f V < %.2f V Nominal:%.2f V",
             del,
             V, prc->prc_voltage_nominal - prc->prc_voltage_warning_deviation,
             prc->prc_voltage_nominal);
    del = ", ";
  }
}


static event_level_t
power_rail_alert_level(struct alert_source *as)
{
  if(as->as_code & (POWER_RAIL_TEMP |
                    POWER_RAIL_FAN |
                    POWER_RAIL_OV_ALERT |
                    POWER_RAIL_UV_ALERT |
                    POWER_RAIL_OC_ALERT |
                    POWER_RAIL_UC_ALERT |
                    POWER_RAIL_FAULT))
    return LOG_ALERT;
  return LOG_WARNING;
}


static bool
is_rail_on(power_rail_t *pr)
{
  uint64_t now = clock_get();

  while(pr != NULL) {
    if(pr->pr_class->prc_set_enable && !pr->pr_on)
      return false;

    if(now < pr->pr_last_change + 100000)
      return false;

    pr = pr->pr_parent;
  }
  return true;
}


void
power_rail_refresh_alert(power_rail_t *pr)
{
  const power_rail_class_t *prc = pr->pr_class;

  const float V = pr->pr_measured_voltage;
  const float Vh = prc->prc_voltage_hysteresis;
  const float Vn = prc->prc_voltage_nominal;

  uint32_t set = 0;
  uint32_t clr = 0;

  if(V > Vn + prc->prc_voltage_alert_deviation + Vh)
    set |= POWER_RAIL_OV_ALERT;
  if(V < Vn + prc->prc_voltage_alert_deviation - Vh)
    clr |= POWER_RAIL_OV_ALERT;
  if(V > Vn + prc->prc_voltage_warning_deviation + Vh)
    set |= POWER_RAIL_OV_WARNING;
  if(V < Vn + prc->prc_voltage_warning_deviation - Vh)
    clr |= POWER_RAIL_OV_WARNING;
  if(V < Vn - prc->prc_voltage_warning_deviation - Vh)
    set |= POWER_RAIL_UV_WARNING;
  if(V > Vn - prc->prc_voltage_warning_deviation + Vh)
    clr |= POWER_RAIL_UV_WARNING;
  if(V < Vn - prc->prc_voltage_alert_deviation - Vh)
    set |= POWER_RAIL_UV_ALERT;
  if(V > Vn - prc->prc_voltage_alert_deviation + Vh)
    clr |= POWER_RAIL_UV_ALERT;

  const float I = pr->pr_measured_current;
  const float Ih = prc->prc_current_hysteresis;

  if(I > prc->prc_over_current_alert + Ih)
    set |= POWER_RAIL_OC_ALERT;
  if(I < prc->prc_over_current_alert - Ih)
    clr |= POWER_RAIL_OC_ALERT;
  if(I > prc->prc_over_current_warning + Ih)
    set |= POWER_RAIL_OC_WARNING;
  if(I < prc->prc_over_current_warning - Ih)
    clr |= POWER_RAIL_OC_WARNING;
  if(I < prc->prc_under_current_warning - Ih)
    set |= POWER_RAIL_UC_WARNING;
  if(I > prc->prc_under_current_warning + Ih)
    clr |= POWER_RAIL_UC_WARNING;
  if(I < prc->prc_under_current_alert - Ih)
    set |= POWER_RAIL_UC_ALERT;
  if(I > prc->prc_under_current_alert + Ih)
    clr |= POWER_RAIL_UC_ALERT;

  if(isfinite(prc->prc_voltage_nominal)) {
    if(set & (POWER_RAIL_OV_ALERT |
              POWER_RAIL_UV_ALERT)) {
      pr->pr_sw_power_good = 0;
    } else {
      pr->pr_sw_power_good = 1;
    }
  }

  if(!is_rail_on(pr)) {
    clr |=
      POWER_RAIL_OC_ALERT |
      POWER_RAIL_OC_WARNING |
      POWER_RAIL_UC_WARNING |
      POWER_RAIL_UC_ALERT |
      POWER_RAIL_UV_WARNING |
      POWER_RAIL_UV_ALERT;
  }

  if(pr->pr_hw_power_good == 0) {
    set |= POWER_RAIL_FAULT;
  } else if(pr->pr_hw_power_good == 1) {
    clr |= POWER_RAIL_FAULT;
  }

  uint32_t code = (pr->pr_alert.as_code | set) &~ clr;
  alert_set(&pr->pr_alert, code);
}


static void
power_rail_alert_refcount(struct alert_source *as, int value)
{
  power_rail_t *pr = container_of(as, power_rail_t, pr_alert);
  if(pr->pr_class->prc_refcount)
    pr->pr_class->prc_refcount(pr, value);
}


static const alert_class_t power_rail_alert_class = {
  .ac_message = power_rail_alert_message,
  .ac_level = power_rail_alert_level,
  .ac_refcount = power_rail_alert_refcount,
};


static void
power_rail_refcount(power_rail_t *pr, int value)
{
  if(pr->pr_class->prc_refcount != NULL)
    pr->pr_class->prc_refcount(pr, value);
}


static int
power_rail_cmp(const power_rail_t *a, const power_rail_t *b)
{
  return strcmp(a->pr_name, b->pr_name);
}


void
power_rail_register(power_rail_t *pr, const power_rail_class_t *prc,
                    const char *name, power_rail_t *parent)
{
  pr->pr_name = name;
  pr->pr_measured_voltage = NAN;
  pr->pr_measured_current = NAN;
  pr->pr_class = prc;
  pr->pr_hw_power_good = POWER_RAIL_POWER_GOOD_UNKNOWN;
  pr->pr_sw_power_good = POWER_RAIL_POWER_GOOD_UNKNOWN;
  power_rail_refcount(pr, 1);

  if(parent) {
    power_rail_refcount(parent, 1);
    pr->pr_parent = parent;
  }

  mutex_lock(&power_rail_mutex);
  SLIST_INSERT_SORTED(&power_rails, pr, pr_link, power_rail_cmp);
  mutex_unlock(&power_rail_mutex);

  alert_register(&pr->pr_alert, &power_rail_alert_class, name);
}

void
power_rail_unregister(power_rail_t *pr)
{
  panic("power_rail_unregister() NOT IMPLEMETED");
}



void
power_rail_set_measured_voltage(power_rail_t *pr, float voltage)
{
  pr->pr_measured_voltage = voltage;
}

void
power_rail_set_measured_current(power_rail_t *pr, float current)
{
  pr->pr_measured_current = current;
}

void
power_rail_set_power_good(power_rail_t *pr, bool power_good)
{
  pr->pr_hw_power_good = power_good;
}

error_t
power_rail_set_on(power_rail_t *pr, bool on)
{
  if(pr->pr_on == on)
    return 0;

  if(pr->pr_class->prc_set_enable == NULL)
    return ERR_NOT_IMPLEMENTED;

  error_t err = pr->pr_class->prc_set_enable(pr, on);
  if(!err) {
    pr->pr_on = on;
    pr->pr_last_change = clock_get();
    return 1;
  }
  return err;
}


power_rail_t *
power_rail_get_next(power_rail_t *cur)
{

  mutex_lock(&power_rail_mutex);
  power_rail_t *pr = cur ? SLIST_NEXT(cur, pr_link) : SLIST_FIRST(&power_rails);
  if(pr)
    power_rail_refcount(pr, 1);
  mutex_unlock(&power_rail_mutex);
  if(cur)
    power_rail_refcount(cur, -1);
  return pr;
}






static const char *
power_good_str(int8_t v)
{
  if(v == -1)
    return "---";
  if(v == 0)
    return "No";
  return "Yes";
}


static error_t
cmd_rail(cli_t *cli, int argc, char **argv)
{
  power_rail_t *pr = NULL;

  if(argc == 3) {
    bool target = false;
    if(!strcmp(argv[2], "on") || !strcmp(argv[2], "1")) {
      target = true;
    } else if(!strcmp(argv[2], "off") || !strcmp(argv[2], "0")) {
      target = false;
    } else {
      return ERR_INVALID_ARGS;
    }

    int cnt = 0;
    while((pr = power_rail_get_next(pr)) != NULL) {
      if(!glob(pr->pr_name, argv[1]))
        continue;
      if(pr->pr_on == target)
        continue;
      if(power_rail_set_on(pr, target) == 1)
        cnt++;
    }
    cli_printf(cli, "%d rails switched\n", cnt);
    return 0;
  }

  const char *pat = argc > 1 ? argv[1] : NULL;

  cli_printf(cli, "                                  PwrGood\n");
  cli_printf(cli, "Name      Voltage  Current  Ctrl  HW  SW   Alert\n");
  cli_printf(cli, "========================================================\n");
  while((pr = power_rail_get_next(pr)) != NULL) {
    if(pat && !glob(pr->pr_name, pat))
      continue;

    cli_printf(cli, "%-9s ", pr->pr_name);
    if(isfinite(pr->pr_measured_voltage)) {
      cli_printf(cli, "%-8.2f ", pr->pr_measured_voltage);
    } else {
      cli_printf(cli, "---      ");
    }

    if(isfinite(pr->pr_measured_current)) {
      cli_printf(cli, "%-8.3f ", pr->pr_measured_current);
    } else {
      cli_printf(cli, "---      ");
    }

    if(pr->pr_class->prc_set_enable) {
      cli_printf(cli, "%-5s ", pr->pr_on ? "ON" : "OFF");
    } else {
      cli_printf(cli, "---   ");
    }

    cli_printf(cli, "%-3s %-3s  ",
               power_good_str(pr->pr_hw_power_good),
               power_good_str(pr->pr_sw_power_good));

    if(pr->pr_alert.as_code) {
      alert_source_t *as = &pr->pr_alert;
      const alert_class_t *ac = as->as_class;
      cli_printf(cli, "%s ", alert_level_to_string(ac->ac_level(as)));
      ac->ac_message(as, cli->cl_stream);
    }

    cli_printf(cli, "\n");
  }
  cli_printf(cli, "\n");
  return 0;
}

CLI_CMD_DEF("rail", cmd_rail);


static error_t
rpc_set_power_rail(rpc_result_t *rr, const char *rail, int on)
{
  power_rail_t *pr = NULL;
  int count = 0;
  while((pr = power_rail_get_next(pr)) != NULL) {
    if(!glob(pr->pr_name, rail))
      continue;
    if(power_rail_set_on(pr, on) == 1)
      count++;
  }
  rr->type = RPC_TYPE_INT;
  rr->i32 = count;
  return 0;
}

RPC_DEF("set_power_rail(si)", rpc_set_power_rail);
