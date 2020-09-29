#pragma once

#include <io.h>

typedef enum {
  STDBY_RC   = 0x00,
  STDBY_XOSC = 0x01,
} RadioStandbyModes_t;

typedef enum {
  USE_LDO   = 0x00,
  USE_DCDC  = 0x01,
} RadioRegulatorModes_t;

typedef enum {
  PACKET_TYPE_GFSK     = 0x00,
  PACKET_TYPE_LORA     = 0x01,
  PACKET_TYPE_RANGING  = 0x02,
  PACKET_TYPE_FLRC     = 0x03,
  PACKET_TYPE_BLE      = 0x04
} RadioPacketTypes_t;


typedef enum {
  RADIO_RAMP_02_US  = 0x00,
  RADIO_RAMP_04_US  = 0x20,
  RADIO_RAMP_06_US  = 0x40,
  RADIO_RAMP_08_US  = 0x60,
  RADIO_RAMP_10_US  = 0x80,
  RADIO_RAMP_12_US  = 0xA0,
  RADIO_RAMP_16_US  = 0xC0,
  RADIO_RAMP_20_US  = 0xE0,
} RadioRampTimes_t;


typedef enum {
  LORA_CAD_01_SYMBOL  = 0x00,
  LORA_CAD_02_SYMBOL  = 0x20,
  LORA_CAD_04_SYMBOL  = 0x40,
  LORA_CAD_08_SYMBOL  = 0x60,
  LORA_CAD_16_SYMBOL  = 0x80,
} RadioLoRaCadSymbols_t;

typedef enum {
  GFSK_BLE_BR_2_000_BW_2_4  = 0x04,
  GFSK_BLE_BR_1_600_BW_2_4  = 0x28,
  GFSK_BLE_BR_1_000_BW_2_4  = 0x4C,
  GFSK_BLE_BR_1_000_BW_1_2  = 0x45,
  GFSK_BLE_BR_0_800_BW_2_4  = 0x70,
  GFSK_BLE_BR_0_800_BW_1_2  = 0x69,
  GFSK_BLE_BR_0_500_BW_1_2  = 0x8D,
  GFSK_BLE_BR_0_500_BW_0_6  = 0x86,
  GFSK_BLE_BR_0_400_BW_1_2  = 0xB1,
  GFSK_BLE_BR_0_400_BW_0_6  = 0xAA,
  GFSK_BLE_BR_0_250_BW_0_6  = 0xCE,
  GFSK_BLE_BR_0_250_BW_0_3  = 0xC7,
  GFSK_BLE_BR_0_125_BW_0_3  = 0xEF,
} RadioGfskBleBitrates_t;

typedef enum {
  GFSK_BLE_MOD_IND_0_35  =  0,
  GFSK_BLE_MOD_IND_0_50  =  1,
  GFSK_BLE_MOD_IND_0_75  =  2,
  GFSK_BLE_MOD_IND_1_00  =  3,
  GFSK_BLE_MOD_IND_1_25  =  4,
  GFSK_BLE_MOD_IND_1_50  =  5,
  GFSK_BLE_MOD_IND_1_75  =  6,
  GFSK_BLE_MOD_IND_2_00  =  7,
  GFSK_BLE_MOD_IND_2_25  =  8,
  GFSK_BLE_MOD_IND_2_50  =  9,
  GFSK_BLE_MOD_IND_2_75  = 10,
  GFSK_BLE_MOD_IND_3_00  = 11,
  GFSK_BLE_MOD_IND_3_25  = 12,
  GFSK_BLE_MOD_IND_3_50  = 13,
  GFSK_BLE_MOD_IND_3_75  = 14,
  GFSK_BLE_MOD_IND_4_00  = 15,
} RadioGfskBleModIndexes_t;

typedef enum {
  FLRC_BR_2_600_BW_2_4 = 0x04,
  FLRC_BR_2_080_BW_2_4 = 0x28,
  FLRC_BR_1_300_BW_1_2 = 0x45,
  FLRC_BR_1_040_BW_1_2 = 0x69,
  FLRC_BR_0_650_BW_0_6 = 0x86,
  FLRC_BR_0_520_BW_0_6 = 0xAA,
  FLRC_BR_0_325_BW_0_3 = 0xC7,
  FLRC_BR_0_260_BW_0_3 = 0xEB,
} RadioFlrcBitrates_t;

typedef enum {
  FLRC_CR_1_2 = 0x00,
  FLRC_CR_3_4 = 0x02,
  FLRC_CR_1_0 = 0x04,
} RadioFlrcCodingRates_t;

typedef enum {
  RADIO_MOD_SHAPING_BT_OFF = 0x00,
  RADIO_MOD_SHAPING_BT_1_0 = 0x10,
  RADIO_MOD_SHAPING_BT_0_5 = 0x20,
} RadioModShapings_t;

typedef enum {
  LORA_SF5  = 0x50,
  LORA_SF6  = 0x60,
  LORA_SF7  = 0x70,
  LORA_SF8  = 0x80,
  LORA_SF9  = 0x90,
  LORA_SF10 = 0xA0,
  LORA_SF11 = 0xB0,
  LORA_SF12 = 0xC0,
} RadioLoRaSpreadingFactors_t;

typedef enum {
  LORA_BW_0200 = 0x34,
  LORA_BW_0400 = 0x26,
  LORA_BW_0800 = 0x18,
  LORA_BW_1600 = 0x0A,
} RadioLoRaBandwidths_t;

typedef enum {
  LORA_CR_4_5    = 0x01,
  LORA_CR_4_6    = 0x02,
  LORA_CR_4_7    = 0x03,
  LORA_CR_4_8    = 0x04,
  LORA_CR_LI_4_5 = 0x05,
  LORA_CR_LI_4_6 = 0x06,
  LORA_CR_LI_4_7 = 0x07,
} RadioLoRaCodingRates_t;

typedef enum {
  PREAMBLE_LENGTH_04_BITS = 0x00,
  PREAMBLE_LENGTH_08_BITS = 0x10,
  PREAMBLE_LENGTH_12_BITS = 0x20,
  PREAMBLE_LENGTH_16_BITS = 0x30,
  PREAMBLE_LENGTH_20_BITS = 0x40,
  PREAMBLE_LENGTH_24_BITS = 0x50,
  PREAMBLE_LENGTH_28_BITS = 0x60,
  PREAMBLE_LENGTH_32_BITS = 0x70,
} RadioPreambleLengths_t;

typedef enum
{
  FLRC_NO_SYNCWORD            = 0x00,
  FLRC_SYNCWORD_LENGTH_4_BYTE = 0x04,
} RadioFlrcSyncWordLengths_t;

typedef enum {
  GFSK_SYNCWORD_LENGTH_1_BYTE = 0x00,
  GFSK_SYNCWORD_LENGTH_2_BYTE = 0x02,
  GFSK_SYNCWORD_LENGTH_3_BYTE = 0x04,
  GFSK_SYNCWORD_LENGTH_4_BYTE = 0x06,
  GFSK_SYNCWORD_LENGTH_5_BYTE = 0x08,
} RadioSyncWordLengths_t;

typedef enum {
  RADIO_RX_MATCH_SYNCWORD_OFF   = 0x00,
  RADIO_RX_MATCH_SYNCWORD_1     = 0x10,
  RADIO_RX_MATCH_SYNCWORD_2     = 0x20,
  RADIO_RX_MATCH_SYNCWORD_1_2   = 0x30,
  RADIO_RX_MATCH_SYNCWORD_3     = 0x40,
  RADIO_RX_MATCH_SYNCWORD_1_3   = 0x50,
  RADIO_RX_MATCH_SYNCWORD_2_3   = 0x60,
  RADIO_RX_MATCH_SYNCWORD_1_2_3 = 0x70,
} RadioSyncWordRxMatchs_t;

typedef enum {
  RADIO_PACKET_FIXED_LENGTH    = 0x00,
  RADIO_PACKET_VARIABLE_LENGTH = 0x20,
} RadioPacketLengthModes_t;

typedef enum {
  RADIO_CRC_OFF     = 0x00,
  RADIO_CRC_1_BYTES = 0x10,
  RADIO_CRC_2_BYTES = 0x20,
  RADIO_CRC_3_BYTES = 0x30,
} RadioCrcTypes_t;

typedef enum {
  RADIO_WHITENING_ON  = 0x00,
  RADIO_WHITENING_OFF = 0x08,
} RadioWhiteningModes_t;

typedef enum {
  LORA_PACKET_VARIABLE_LENGTH = 0x00,
  LORA_PACKET_FIXED_LENGTH    = 0x80,
  LORA_PACKET_EXPLICIT        = LORA_PACKET_VARIABLE_LENGTH,
  LORA_PACKET_IMPLICIT        = LORA_PACKET_FIXED_LENGTH,
} RadioLoRaPacketLengthsModes_t;

typedef enum {
  LORA_CRC_ON  = 0x20,
  LORA_CRC_OFF = 0x00,
} RadioLoRaCrcModes_t;

typedef enum {
  LORA_IQ_NORMAL   = 0x40,
  LORA_IQ_INVERTED = 0x00,
} RadioLoRaIQModes_t;

typedef enum {
  RANGING_IDCHECK_LENGTH_08_BITS = 0x00,
  RANGING_IDCHECK_LENGTH_16_BITS,
  RANGING_IDCHECK_LENGTH_24_BITS,
  RANGING_IDCHECK_LENGTH_32_BITS,
} RadioRangingIdCheckLengths_t;

typedef enum {
  RANGING_RESULT_RAW      = 0x00,
  RANGING_RESULT_AVERAGED = 0x01,
  RANGING_RESULT_DEBIASED = 0x02,
  RANGING_RESULT_FILTERED = 0x03,
} RadioRangingResultTypes_t;

typedef enum {
  BLE_PAYLOAD_LENGTH_MAX_31_BYTES  = 0x00,
  BLE_PAYLOAD_LENGTH_MAX_37_BYTES  = 0x20,
  BLE_TX_TEST_MODE                 = 0x40,
  BLE_PAYLOAD_LENGTH_MAX_255_BYTES = 0x80,
} RadioBleConnectionStates_t;

typedef enum {
  BLE_CRC_OFF = 0x00,
  BLE_CRC_3B  = 0x10,
} RadioBleCrcFields_t;

typedef enum {
    BLE_PRBS_9       = 0x00,
    BLE_PRBS_15      = 0x0C,
    BLE_EYELONG_1_0  = 0x04,
    BLE_EYELONG_0_1  = 0x18,
    BLE_EYESHORT_1_0 = 0x08,
    BLE_EYESHORT_0_1 = 0x1C,
    BLE_ALL_1        = 0x10,
    BLE_ALL_0        = 0x14,
} RadioBlePacketTypes_t;

typedef enum {
  IRQ_RADIO_NONE                          = 0x0000,
  IRQ_TX_DONE                             = 0x0001,
  IRQ_RX_DONE                             = 0x0002,
  IRQ_SYNCWORD_VALID                      = 0x0004,
  IRQ_SYNCWORD_ERROR                      = 0x0008,
  IRQ_HEADER_VALID                        = 0x0010,
  IRQ_HEADER_ERROR                        = 0x0020,
  IRQ_CRC_ERROR                           = 0x0040,
  IRQ_RANGING_SLAVE_RESPONSE_DONE         = 0x0080,
  IRQ_RANGING_SLAVE_REQUEST_DISCARDED     = 0x0100,
  IRQ_RANGING_MASTER_RESULT_VALID         = 0x0200,
  IRQ_RANGING_MASTER_RESULT_TIMEOUT       = 0x0400,
  IRQ_RANGING_SLAVE_REQUEST_VALID         = 0x0800,
  IRQ_CAD_DONE                            = 0x1000,
  IRQ_CAD_ACTIVITY_DETECTED               = 0x2000,
  IRQ_RX_TX_TIMEOUT                       = 0x4000,
  IRQ_PREAMBLE_DETECTED                   = 0x8000,
  IRQ_RADIO_ALL                           = 0xFFFF,
} RadioIrqMasks_t;

typedef enum {
  RADIO_TICK_SIZE_0015_US                 = 0x00,
  RADIO_TICK_SIZE_0062_US                 = 0x01,
  RADIO_TICK_SIZE_1000_US                 = 0x02,
  RADIO_TICK_SIZE_4000_US                 = 0x03,
}RadioTickSizes_t;

typedef enum {
  RADIO_RANGING_ROLE_SLAVE                = 0x00,
  RADIO_RANGING_ROLE_MASTER               = 0x01,
}RadioRangingRoles_t;

typedef enum {
  RADIO_GET_STATUS                        = 0xC0,
  RADIO_WRITE_REGISTER                    = 0x18,
  RADIO_READ_REGISTER                     = 0x19,
  RADIO_WRITE_BUFFER                      = 0x1A,
  RADIO_READ_BUFFER                       = 0x1B,
  RADIO_SET_SLEEP                         = 0x84,
  RADIO_SET_STANDBY                       = 0x80,
  RADIO_SET_FS                            = 0xC1,
  RADIO_SET_TX                            = 0x83,
  RADIO_SET_RX                            = 0x82,
  RADIO_SET_RXDUTYCYCLE                   = 0x94,
  RADIO_SET_CAD                           = 0xC5,
  RADIO_SET_TXCONTINUOUSWAVE              = 0xD1,
  RADIO_SET_TXCONTINUOUSPREAMBLE          = 0xD2,
  RADIO_SET_PACKETTYPE                    = 0x8A,
  RADIO_GET_PACKETTYPE                    = 0x03,
  RADIO_SET_RFFREQUENCY                   = 0x86,
  RADIO_SET_TXPARAMS                      = 0x8E,
  RADIO_SET_CADPARAMS                     = 0x88,
  RADIO_SET_BUFFERBASEADDRESS             = 0x8F,
  RADIO_SET_MODULATIONPARAMS              = 0x8B,
  RADIO_SET_PACKETPARAMS                  = 0x8C,
  RADIO_GET_RXBUFFERSTATUS                = 0x17,
  RADIO_GET_PACKETSTATUS                  = 0x1D,
  RADIO_GET_RSSIINST                      = 0x1F,
  RADIO_SET_DIOIRQPARAMS                  = 0x8D,
  RADIO_GET_IRQSTATUS                     = 0x15,
  RADIO_CLR_IRQSTATUS                     = 0x97,
  RADIO_CALIBRATE                         = 0x89,
  RADIO_SET_REGULATORMODE                 = 0x96,
  RADIO_SET_SAVECONTEXT                   = 0xD5,
  RADIO_SET_AUTOTX                        = 0x98,
  RADIO_SET_AUTOFS                        = 0x9E,
  RADIO_SET_LONGPREAMBLE                  = 0x9B,
  RADIO_SET_UARTSPEED                     = 0x9D,
  RADIO_SET_RANGING_ROLE                  = 0xA3,
} RadioCommands_t;



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
