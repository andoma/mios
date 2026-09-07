#pragma once

// SX1280 command opcodes (Semtech SX1280/SX1281 datasheet, chapter 11)

#define SX1280_GET_STATUS             0xc0
#define SX1280_WRITE_REGISTER         0x18
#define SX1280_READ_REGISTER          0x19
#define SX1280_WRITE_BUFFER           0x1a
#define SX1280_READ_BUFFER            0x1b
#define SX1280_SET_SLEEP              0x84
#define SX1280_SET_STANDBY            0x80
#define SX1280_SET_FS                 0xc1
#define SX1280_SET_TX                 0x83
#define SX1280_SET_RX                 0x82
#define SX1280_SET_RXDUTYCYCLE        0x94
#define SX1280_SET_CAD                0xc5
#define SX1280_SET_TXCONTINUOUSWAVE   0xd1
#define SX1280_SET_TXCONTINUOUSPREAMBLE 0xd2
#define SX1280_SET_PACKETTYPE         0x8a
#define SX1280_GET_PACKETTYPE         0x03
#define SX1280_SET_RFFREQUENCY        0x86
#define SX1280_SET_TXPARAMS           0x8e
#define SX1280_SET_CADPARAMS          0x88
#define SX1280_SET_BUFFERBASEADDRESS  0x8f
#define SX1280_SET_MODULATIONPARAMS   0x8b
#define SX1280_SET_PACKETPARAMS       0x8c
#define SX1280_GET_RXBUFFERSTATUS     0x17
#define SX1280_GET_PACKETSTATUS       0x1d
#define SX1280_GET_RSSIINST           0x1f
#define SX1280_SET_DIOIRQPARAMS       0x8d
#define SX1280_GET_IRQSTATUS          0x15
#define SX1280_CLR_IRQSTATUS          0x97
#define SX1280_SET_REGULATORMODE      0x96
#define SX1280_SET_SAVECONTEXT        0xd5
#define SX1280_SET_AUTOTX             0x98
#define SX1280_SET_AUTOFS             0x9e
#define SX1280_SET_LONGPREAMBLE       0x9b
#define SX1280_SET_RANGING_ROLE       0xa3

// GetStatus[7:5] circuit mode
#define SX1280_MODE_STDBY_RC          0x2
#define SX1280_MODE_STDBY_XOSC        0x3
#define SX1280_MODE_FS                0x4
#define SX1280_MODE_RX                0x5
#define SX1280_MODE_TX                0x6

// SetStandby argument
#define SX1280_STDBY_RC               0x00
#define SX1280_STDBY_XOSC             0x01

// SetRegulatorMode argument
#define SX1280_USE_LDO                0x00
#define SX1280_USE_DCDC               0x01

// SetPacketType argument
#define SX1280_PACKET_TYPE_GFSK       0x00
#define SX1280_PACKET_TYPE_LORA       0x01
#define SX1280_PACKET_TYPE_RANGING    0x02
#define SX1280_PACKET_TYPE_FLRC       0x03
#define SX1280_PACKET_TYPE_BLE        0x04

// IRQ bits (SetDioIrqParams / GetIrqStatus / ClrIrqStatus)
#define SX1280_IRQ_TX_DONE                    0x0001
#define SX1280_IRQ_RX_DONE                    0x0002
#define SX1280_IRQ_SYNCWORD_VALID             0x0004
#define SX1280_IRQ_SYNCWORD_ERROR             0x0008
#define SX1280_IRQ_HEADER_VALID               0x0010
#define SX1280_IRQ_HEADER_ERROR               0x0020
#define SX1280_IRQ_CRC_ERROR                  0x0040
#define SX1280_IRQ_RANGING_SLAVE_RESP_DONE    0x0080
#define SX1280_IRQ_RANGING_SLAVE_REQ_DISCARD  0x0100
#define SX1280_IRQ_RANGING_MASTER_RES_VALID   0x0200
#define SX1280_IRQ_RANGING_MASTER_TIMEOUT     0x0400
#define SX1280_IRQ_RANGING_SLAVE_REQ_VALID    0x0800
#define SX1280_IRQ_CAD_DONE                   0x1000
#define SX1280_IRQ_CAD_DETECTED               0x2000
#define SX1280_IRQ_RX_TX_TIMEOUT              0x4000
#define SX1280_IRQ_PREAMBLE_DETECTED          0x8000

// Time-out tick sizes (SetTx / SetRx period base)
#define SX1280_TICK_SIZE_15_6_US      0x00
#define SX1280_TICK_SIZE_62_5_US      0x01
#define SX1280_TICK_SIZE_1_MS         0x02
#define SX1280_TICK_SIZE_4_MS         0x03

// Registers
#define SX1280_REG_LNA_REGIME         0x0891
#define SX1280_REG_FLRC_SYNCWORD1     0x09cf
#define SX1280_REG_FLRC_SYNCWORD2     0x09d4
#define SX1280_REG_FLRC_SYNCWORD3     0x09d9

// Whitening seed: not in the datasheet, from Semtech's driver sources
// (REG_LR_WHITSEEDBASEADDR). BLE seeds it per channel.
#define SX1280_REG_WHITENING_SEED     0x09c5
#define SX1280_REG_CRC_INIT           0x09c7 // 3 bytes, MSB first (BLE)
#define SX1280_REG_CRC_SEED           0x09c8 // 2 bytes, MSB first (GFSK/FLRC)
#define SX1280_REG_BLE_ACCESS_ADDR    0x09cf // 4 bytes, MSB first

// FLRC modulation parameters (datasheet tables 13-31..13-33)
#define SX1280_FLRC_BR_1_300_BW_1_2   0x45
#define SX1280_FLRC_BR_1_000_BW_1_2   0x69
#define SX1280_FLRC_BR_0_650_BW_0_6   0x86
#define SX1280_FLRC_BR_0_520_BW_0_6   0xaa
#define SX1280_FLRC_BR_0_325_BW_0_3   0xc7
#define SX1280_FLRC_BR_0_260_BW_0_3   0xeb

#define SX1280_FLRC_CR_1_2            0x00
#define SX1280_FLRC_CR_3_4            0x02
#define SX1280_FLRC_CR_1_0            0x04

#define SX1280_FLRC_BT_DIS            0x00
#define SX1280_FLRC_BT_1_0            0x10
#define SX1280_FLRC_BT_0_5            0x20

// FLRC packet parameters (datasheet tables 13-34..13-42)
// AGC preamble: value = (bits / 4 - 1) << 4. Minimum is 8 bits at
// 1.3Mb/s, 16 bits at every other bit rate.
#define SX1280_FLRC_PREAMBLE_8_BITS   0x10
#define SX1280_FLRC_PREAMBLE_16_BITS  0x30
#define SX1280_FLRC_PREAMBLE_24_BITS  0x50
#define SX1280_FLRC_PREAMBLE_32_BITS  0x70

#define SX1280_FLRC_SYNC_NOSYNC       0x00 // 21-bit preamble only
#define SX1280_FLRC_SYNC_LEN_P32S     0x04 // + 32-bit sync word

#define SX1280_FLRC_RX_MATCH_SYNC_OFF 0x00
#define SX1280_FLRC_RX_MATCH_SYNC_1   0x10
#define SX1280_FLRC_RX_MATCH_SYNC_2   0x20
#define SX1280_FLRC_RX_MATCH_SYNC_3   0x40

#define SX1280_FLRC_PACKET_FIXED_LENGTH    0x00
#define SX1280_FLRC_PACKET_VARIABLE_LENGTH 0x20

#define SX1280_FLRC_CRC_OFF           0x00
#define SX1280_FLRC_CRC_2_BYTE        0x20
#define SX1280_FLRC_CRC_3_BYTE        0x30

// FLRC has no whitener; packetParam7 must always say disabled
#define SX1280_FLRC_WHITENING_OFF     0x08

#define SX1280_FLRC_PAYLOAD_MIN       6
#define SX1280_FLRC_PAYLOAD_MAX       127

// BLE modulation parameters (1Mb/s uncoded PHY)
#define SX1280_BLE_BR_1_000_BW_1_2    0x45
#define SX1280_BLE_MOD_IND_0_5        0x01
#define SX1280_BLE_BT_0_5             0x20

// BLE packet parameters
#define SX1280_BLE_PAYLOAD_MAX_37     0x20 // packetParam1: adv channel PDUs
#define SX1280_BLE_PAYLOAD_MAX_255    0x80 // packetParam1: DLE data PDUs
#define SX1280_BLE_CRC_3B             0x10 // packetParam2
#define SX1280_BLE_WHITENING_ENABLE   0x00 // packetParam4
#define SX1280_BLE_WHITENING_DISABLE  0x08

#define SX1280_BLE_ADV_ACCESS_ADDR    0x8e89bed6
#define SX1280_BLE_ADV_CRC_INIT       0x555555

// PLL step: 52 MHz / 2^18 => reg = Hz * 2^18 / 52e6
#define SX1280_HZ_TO_FREQ(hz) \
  ((uint32_t)(((uint64_t)(hz) << 18) / 52000000))
