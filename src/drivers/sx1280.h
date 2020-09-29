#pragma once

#include <io.h>

#include "sx1280_reg.h"


typedef struct {
  gpio_t gpio_nss;     // GPIO pin for NSS (Chip Select)
  gpio_t gpio_busy;    // GPIO connected to BUSY on sx1280
  gpio_t gpio_reset;   // GPIO connected to RESET on sx1280
  gpio_t gpio_irq;     // GPIO connected to DIO0 on sx1280

  uint32_t frequency; // in Hz

  int8_t output_gain;  // in dB (-18 to +13)

  RadioFlrcBitrates_t br;
  RadioFlrcCodingRates_t cr;
  RadioModShapings_t ms;
  RadioPreambleLengths_t pl;
  RadioFlrcSyncWordLengths_t swl;
  RadioSyncWordRxMatchs_t rxm;
  RadioPacketLengthModes_t lm;
  RadioCrcTypes_t crctype;
  RadioWhiteningModes_t wm;

} sx1280_config_t;


typedef struct sx1280 sx1280_t;

sx1280_t *sx1280_create(spi_t *bus, const sx1280_config_t *config);

error_t sx1280_send(sx1280_t *dev,
                    const void *hdr, size_t hdr_len,
                    const void *payload, size_t payload_len,
                    int wait);
