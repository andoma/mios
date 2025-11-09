#pragma once

#include <stdbool.h>

#include "alert.h"

// These bits are set in cz_alert.as_code

// Temperature alert bits
#define CLIMATE_ZONE_OT_ERROR    0x00000001
#define CLIMATE_ZONE_OT_WARNING  0x00000002
#define CLIMATE_ZONE_UT_WARNING  0x00000004
#define CLIMATE_ZONE_UT_ERROR    0x00000008

// Relative Humidity alert bits
#define CLIMATE_ZONE_ORH_ERROR   0x00000010
#define CLIMATE_ZONE_ORH_WARNING 0x00000020
#define CLIMATE_ZONE_URH_WARNING 0x00000040
#define CLIMATE_ZONE_URH_ERROR   0x00000080

// Fan-speed alerts bits
#define CLIMATE_ZONE_OF_ERROR    0x00000100
#define CLIMATE_ZONE_OF_WARNING  0x00000200
#define CLIMATE_ZONE_UF_WARNING  0x00000400
#define CLIMATE_ZONE_UF_ERROR    0x00000800

// Alert bits for indicating that the sensor itself has a problem
// Those needs to be passed in to climate_zone_refresh_alert()
#define CLIMATE_ZONE_TEMP_SENSE_ERROR 0x00010000
#define CLIMATE_ZONE_RH_SENSE_ERROR   0x00020000
#define CLIMATE_ZONE_FAN_SENSE_ERROR  0x00040000

typedef struct climate_zone {
  SLIST_ENTRY(climate_zone) cz_link;
  const struct climate_zone_class *cz_class;
  const char *cz_name;

  // These are NaN if not measured
  float cz_measured_temperature;
  float cz_measured_rh; // Relative Humidity
  float cz_measured_fan_rpm;

  alert_source_t cz_alert;

} climate_zone_t;

typedef struct climate_zone_class {

  float czc_over_temp_error;
  float czc_over_temp_warning;
  float czc_under_temp_warning;
  float czc_under_temp_error;
  float czc_temp_hysteresis;

  float czc_over_rh_error;
  float czc_over_rh_warning;
  float czc_under_rh_warning;
  float czc_under_rh_error;
  float czc_rh_hysteresis;

  float czc_over_fan_rpm_error;
  float czc_over_fan_rpm_warning;
  float czc_under_fan_rpm_warning;
  float czc_under_fan_rpm_error;
  float czc_fan_rpm_hysteresis;

  void (*czc_refcount)(struct climate_zone *cz, int value);

} climate_zone_class_t;

void climate_zone_register(climate_zone_t *cz, const climate_zone_class_t *prc,
                           const char *name);

void climate_zone_set_measured_temperature(climate_zone_t *cz, float temp);

void climate_zone_set_measured_rh(climate_zone_t *cz, float rh);

void climate_zone_set_measured_fan_rpm(climate_zone_t *cz, float fan_rpm);

/*
 Pass CLIMATE_ZONE_*_SENSE_ERROR bits to sense_set and sense_clr
 to signal problems with the sensor itself
*/
void climate_zone_refresh_alert(climate_zone_t *cz, uint32_t sense_set,
                                uint32_t sense_clr);
