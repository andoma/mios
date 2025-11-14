#pragma once

#include <stdbool.h>

#include "alert.h"

#define POWER_RAIL_DEFAULT_ON                     0x1
#define POWER_RAIL_SHUTDOWN_ON_OVER_CURRENT_ALERT 0x2
#define POWER_RAIL_SHUTDOWN_ON_OVER_VOLTAGE_ALERT 0x4


#define POWER_RAIL_POWER_GOOD_UNKNOWN -1

#define POWER_RAIL_OV_ALERT   0x00000001
#define POWER_RAIL_OV_WARNING 0x00000002
#define POWER_RAIL_UV_WARNING 0x00000004
#define POWER_RAIL_UV_ALERT   0x00000008

#define POWER_RAIL_OC_ALERT   0x00000010
#define POWER_RAIL_OC_WARNING 0x00000020
#define POWER_RAIL_UC_WARNING 0x00000040
#define POWER_RAIL_UC_ALERT   0x00000080

#define POWER_RAIL_FAULT      0x00000100
#define POWER_RAIL_OFF        0x00000200  // Can be set to generate a warning
#define POWER_RAIL_TEMP_WARN  0x00000400
#define POWER_RAIL_TEMP_CRIT  0x00000800
#define POWER_RAIL_FAN        0x00001000

typedef struct power_rail {
  SLIST_ENTRY(power_rail) pr_link;
  const struct power_rail_class *pr_class;
  const char *pr_name;
  struct power_rail *pr_parent;
  int64_t pr_last_change;

  float pr_measured_voltage;
  float pr_measured_current;

  alert_source_t pr_alert;
  int8_t pr_hw_power_good; // -1 = unknown (POWER_RAIL_POWER_GOOD_UNKNOWN)
  int8_t pr_sw_power_good; // -1 = unknown (POWER_RAIL_POWER_GOOD_UNKNOWN)
  bool pr_on;

} power_rail_t;


typedef struct power_rail_class {

  float prc_voltage_nominal;
  float prc_voltage_warning_deviation;
  float prc_voltage_alert_deviation;
  float prc_voltage_hysteresis;

  float prc_over_current_alert;
  float prc_over_current_warning;
  float prc_under_current_warning;
  float prc_under_current_alert;
  float prc_current_hysteresis;

  uint32_t prc_flags;

  void (*prc_refcount)(struct power_rail *pr, int value);

  error_t (*prc_set_enable)(struct power_rail *pr, bool on);

} power_rail_class_t;



void power_rail_register(power_rail_t *pr, const power_rail_class_t *prc,
                         const char *name, power_rail_t *parent);

void power_rail_unregister(power_rail_t *pr);

void power_rail_set_measured_voltage(power_rail_t *pr, float voltage);

void power_rail_set_measured_current(power_rail_t *pr, float current);

void power_rail_set_power_good(power_rail_t *pr, bool power_good);

error_t power_rail_set_on(power_rail_t *pr, bool on);

void power_rail_refresh_alert(power_rail_t *pr);
