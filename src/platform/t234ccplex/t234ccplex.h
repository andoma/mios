#define GIC_GICD_BASE 0x0f400000

#define CACHE_LINE_SIZE 64

#define PBUF_DATA_SIZE 1536

#define EVENTLOG_SIZE 4096

#ifndef __ASSEMBLER__
/*
 * Read MAC address #index (0 = the module's first/Ethernet MAC) from the module
 * (CVM) EEPROM customer block. Returns 0 and fills mac[6] in big-endian order on
 * success, -1 if no valid customer block / not enough addresses.
 */
int t234_eeprom_mac_address(unsigned int index, unsigned char mac[6]);
#endif
