#include <stddef.h>
#include <stdint.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

// Bootloader for reflashing main NRF52 Flash from an external SPI flash

#define UART_BASE        0x40002000
#define WDT_BASE         0x40010000
#define NVMC_BASE        0x4001e000
#define GPIO_BASE        0x50000000

#define WDT_TASKS_START (WDT_BASE + 0x000)
#define WDT_CRV         (WDT_BASE + 0x504)
#define WDT_RREN        (WDT_BASE + 0x508)
#define WDT_CONFIG      (WDT_BASE + 0x50c)
#define WDT_RR(x)       (WDT_BASE + 0x600 + (x) * 4)
#define WDT_RESET_VALUE  0x6E524635


#define NVMC_READY     (NVMC_BASE + 0x400)
#define NVMC_CONFIG    (NVMC_BASE + 0x504)
#define NVMC_ERASEPAGE (NVMC_BASE + 0x508)
#define NVMC_ERASEALL  (NVMC_BASE + 0x50c)
#define NVMC_ERASEPCR0 (NVMC_BASE + 0x510)
#define NVMC_ERASEUICR (NVMC_BASE + 0x514)
#define NVMC_ICACHECNF (NVMC_BASE + 0x540)
#define NVMC_IHIT      (NVMC_BASE + 0x548)
#define NVMC_IMIS      (NVMC_BASE + 0x54c)


#define GPIO_PIN_CNF(x) (GPIO_BASE + 0x700 + (x) * 4)
#define GPIO_OUTSET     (GPIO_BASE + 0x508)
#define GPIO_OUTCLR     (GPIO_BASE + 0x50c)


#define SPI_EVENTS_READY (SPI_BASE + 0x108)
#define SPI_ENABLE       (SPI_BASE + 0x500)
#define SPI_PSEL_SCK     (SPI_BASE + 0x508)
#define SPI_PSEL_MOSI    (SPI_BASE + 0x50c)
#define SPI_PSEL_MISO    (SPI_BASE + 0x510)
#define SPI_RXD          (SPI_BASE + 0x518)
#define SPI_TXD          (SPI_BASE + 0x51C)
#define SPI_FREQUENCY    (SPI_BASE + 0x524)
#define SPI_CONFIG       (SPI_BASE + 0x554)

#define UART_ENABLE   (UART_BASE + 0x500)
#define UART_PSELTXD  (UART_BASE + 0x50c)
#define UART_TXD      (UART_BASE + 0x51c)
#define UART_BAUDRATE (UART_BASE + 0x524)
#define UART_TX_TASK  (UART_BASE + 0x8)
#define UART_TX_RDY   (UART_BASE + 0x11c)



__attribute__((always_inline))
static inline void
reg_wr(uint32_t addr, uint32_t value)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  *ptr = value;
}

__attribute__((always_inline))
static inline uint32_t
reg_rd(uint32_t addr)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  return *ptr;
}



__attribute__((section("bldata")))
static const uint32_t crc32table[256] = {
  0xd202ef8d, 0xa505df1b, 0x3c0c8ea1, 0x4b0bbe37, 0xd56f2b94, 0xa2681b02,
  0x3b614ab8, 0x4c667a2e, 0xdcd967bf, 0xabde5729, 0x32d70693, 0x45d03605,
  0xdbb4a3a6, 0xacb39330, 0x35bac28a, 0x42bdf21c, 0xcfb5ffe9, 0xb8b2cf7f,
  0x21bb9ec5, 0x56bcae53, 0xc8d83bf0, 0xbfdf0b66, 0x26d65adc, 0x51d16a4a,
  0xc16e77db, 0xb669474d, 0x2f6016f7, 0x58672661, 0xc603b3c2, 0xb1048354,
  0x280dd2ee, 0x5f0ae278, 0xe96ccf45, 0x9e6bffd3, 0x0762ae69, 0x70659eff,
  0xee010b5c, 0x99063bca, 0x000f6a70, 0x77085ae6, 0xe7b74777, 0x90b077e1,
  0x09b9265b, 0x7ebe16cd, 0xe0da836e, 0x97ddb3f8, 0x0ed4e242, 0x79d3d2d4,
  0xf4dbdf21, 0x83dcefb7, 0x1ad5be0d, 0x6dd28e9b, 0xf3b61b38, 0x84b12bae,
  0x1db87a14, 0x6abf4a82, 0xfa005713, 0x8d076785, 0x140e363f, 0x630906a9,
  0xfd6d930a, 0x8a6aa39c, 0x1363f226, 0x6464c2b0, 0xa4deae1d, 0xd3d99e8b,
  0x4ad0cf31, 0x3dd7ffa7, 0xa3b36a04, 0xd4b45a92, 0x4dbd0b28, 0x3aba3bbe,
  0xaa05262f, 0xdd0216b9, 0x440b4703, 0x330c7795, 0xad68e236, 0xda6fd2a0,
  0x4366831a, 0x3461b38c, 0xb969be79, 0xce6e8eef, 0x5767df55, 0x2060efc3,
  0xbe047a60, 0xc9034af6, 0x500a1b4c, 0x270d2bda, 0xb7b2364b, 0xc0b506dd,
  0x59bc5767, 0x2ebb67f1, 0xb0dff252, 0xc7d8c2c4, 0x5ed1937e, 0x29d6a3e8,
  0x9fb08ed5, 0xe8b7be43, 0x71beeff9, 0x06b9df6f, 0x98dd4acc, 0xefda7a5a,
  0x76d32be0, 0x01d41b76, 0x916b06e7, 0xe66c3671, 0x7f6567cb, 0x0862575d,
  0x9606c2fe, 0xe101f268, 0x7808a3d2, 0x0f0f9344, 0x82079eb1, 0xf500ae27,
  0x6c09ff9d, 0x1b0ecf0b, 0x856a5aa8, 0xf26d6a3e, 0x6b643b84, 0x1c630b12,
  0x8cdc1683, 0xfbdb2615, 0x62d277af, 0x15d54739, 0x8bb1d29a, 0xfcb6e20c,
  0x65bfb3b6, 0x12b88320, 0x3fba6cad, 0x48bd5c3b, 0xd1b40d81, 0xa6b33d17,
  0x38d7a8b4, 0x4fd09822, 0xd6d9c998, 0xa1def90e, 0x3161e49f, 0x4666d409,
  0xdf6f85b3, 0xa868b525, 0x360c2086, 0x410b1010, 0xd80241aa, 0xaf05713c,
  0x220d7cc9, 0x550a4c5f, 0xcc031de5, 0xbb042d73, 0x2560b8d0, 0x52678846,
  0xcb6ed9fc, 0xbc69e96a, 0x2cd6f4fb, 0x5bd1c46d, 0xc2d895d7, 0xb5dfa541,
  0x2bbb30e2, 0x5cbc0074, 0xc5b551ce, 0xb2b26158, 0x04d44c65, 0x73d37cf3,
  0xeada2d49, 0x9ddd1ddf, 0x03b9887c, 0x74beb8ea, 0xedb7e950, 0x9ab0d9c6,
  0x0a0fc457, 0x7d08f4c1, 0xe401a57b, 0x930695ed, 0x0d62004e, 0x7a6530d8,
  0xe36c6162, 0x946b51f4, 0x19635c01, 0x6e646c97, 0xf76d3d2d, 0x806a0dbb,
  0x1e0e9818, 0x6909a88e, 0xf000f934, 0x8707c9a2, 0x17b8d433, 0x60bfe4a5,
  0xf9b6b51f, 0x8eb18589, 0x10d5102a, 0x67d220bc, 0xfedb7106, 0x89dc4190,
  0x49662d3d, 0x3e611dab, 0xa7684c11, 0xd06f7c87, 0x4e0be924, 0x390cd9b2,
  0xa0058808, 0xd702b89e, 0x47bda50f, 0x30ba9599, 0xa9b3c423, 0xdeb4f4b5,
  0x40d06116, 0x37d75180, 0xaede003a, 0xd9d930ac, 0x54d13d59, 0x23d60dcf,
  0xbadf5c75, 0xcdd86ce3, 0x53bcf940, 0x24bbc9d6, 0xbdb2986c, 0xcab5a8fa,
  0x5a0ab56b, 0x2d0d85fd, 0xb404d447, 0xc303e4d1, 0x5d677172, 0x2a6041e4,
  0xb369105e, 0xc46e20c8, 0x72080df5, 0x050f3d63, 0x9c066cd9, 0xeb015c4f,
  0x7565c9ec, 0x0262f97a, 0x9b6ba8c0, 0xec6c9856, 0x7cd385c7, 0x0bd4b551,
  0x92dde4eb, 0xe5dad47d, 0x7bbe41de, 0x0cb97148, 0x95b020f2, 0xe2b71064,
  0x6fbf1d91, 0x18b82d07, 0x81b17cbd, 0xf6b64c2b, 0x68d2d988, 0x1fd5e91e,
  0x86dcb8a4, 0xf1db8832, 0x616495a3, 0x1663a535, 0x8f6af48f, 0xf86dc419,
  0x660951ba, 0x110e612c, 0x88073096, 0xff000000
};

__attribute__((section("bltext"),noinline,unused))
static uint32_t
crc32(uint32_t crc, const void *data, size_t n_bytes)
{
  for(size_t i = 0; i < n_bytes; ++i)
    crc = crc32table[(uint8_t)crc ^ ((uint8_t*)data)[i]] ^ crc >> 8;

  return crc;
}


__attribute__((section("bltext"),noinline))
static void
print_char(char c)
{
#ifdef GPIO_UART_TX
  reg_wr(UART_TXD, c);
  reg_wr(UART_TX_TASK, 1);

  while(!reg_rd(UART_TX_RDY)) {
  }
  reg_wr(UART_TX_RDY, 0);
  reg_wr(UART_TX_TASK, 0);
#endif
}

__attribute__((section("bltext"),noinline))
static void
print_nibble(uint8_t n)
{
  print_char(n < 10 ? n + '0' : n + 'a' - 10);
}

__attribute__((section("bltext"),noinline,unused))
static void
print_u32(uint32_t n)
{
  for(int i = 28; i >= 0; i -= 4) {
    print_nibble((n >> i) & 0xf);
  }
}

__attribute__((section("bltext"),noinline,unused))
static void
print_u8(uint8_t n)
{
  for(int i = 4; i >= 0; i -= 4) {
    print_nibble((n >> i) & 0xf);
  }
}

__attribute__((always_inline,unused))
static inline void
gpio_set_output(int port, int hi)
{
  uint32_t bit = 1 << port;
  reg_wr(hi ? GPIO_OUTSET : GPIO_OUTCLR, bit);
}

#ifdef GPIO_SPIFLASH_CS

__attribute__((section("bltext"),noinline))
static void
delay(void)
{
    for(int i = 0; i < 1000; i++) {
      asm volatile("nop");
    }
}

__attribute__((section("bltext"),noinline))
static uint8_t
spi_txrx(uint8_t out)
{
  reg_wr(SPI_TXD, out);
  while(reg_rd(SPI_EVENTS_READY) == 0) {}
  reg_wr(SPI_EVENTS_READY, 0);
  return reg_rd(SPI_RXD);
}

__attribute__((section("bltext"),noinline))
static uint8_t
spiflash_wakeup(void)
{
  delay();

  gpio_set_output(GPIO_SPIFLASH_CS, 0);
  uint8_t id;
  for(int i = 0; i < 5; i++) {
    id = spi_txrx(0xab);
  }
  gpio_set_output(GPIO_SPIFLASH_CS, 1);
  return id;
}

__attribute__((section("bltext"),noinline))
static void
spiflash_read(void *ptr, uint32_t addr, size_t len)
{
  delay();

  uint8_t *u8 = ptr;
  gpio_set_output(GPIO_SPIFLASH_CS, 0);
  spi_txrx(0x3);
  spi_txrx(addr >> 16);
  spi_txrx(addr >> 8);
  spi_txrx(addr);
  for(size_t i = 0; i < len; i++) {
    u8[i] = spi_txrx(0);
  }
  gpio_set_output(GPIO_SPIFLASH_CS, 1);
}

__attribute__((section("bltext"),noinline))
static void
spiflash_wait_ready(void)
{
  while(1) {
    delay();
    gpio_set_output(GPIO_SPIFLASH_CS, 0);
    spi_txrx(5);
    uint8_t status = spi_txrx(0);
    gpio_set_output(GPIO_SPIFLASH_CS, 1);
    if((status & 1) == 0)
      break;
  }
}


__attribute__((section("bltext"),noinline))
static void
spiflash_erase(uint32_t block)
{
  spiflash_wait_ready();

  gpio_set_output(GPIO_SPIFLASH_CS, 0);
  spi_txrx(0x6); // Write-enable
  gpio_set_output(GPIO_SPIFLASH_CS, 1);

  delay();

  uint32_t addr = block * 4096;

  gpio_set_output(GPIO_SPIFLASH_CS, 0);
  spi_txrx(0x20);
  spi_txrx(addr >> 16);
  spi_txrx(addr >> 8);
  spi_txrx(addr);
  gpio_set_output(GPIO_SPIFLASH_CS, 1);

  spiflash_wait_ready();
}

__attribute__((always_inline))
static inline void
flash_wait_ready(void)
{
  while(!reg_rd(NVMC_READY)) {}
}

__attribute__((section("bltext"),noinline))
static void
flash_erase_sector(int sector)
{
  reg_wr(NVMC_CONFIG, 2);
  reg_wr(NVMC_ERASEPAGE, sector * 4096);
  flash_wait_ready();
  reg_wr(NVMC_CONFIG, 0);
}
#endif

extern uint32_t vectors[];

typedef struct {
  uint32_t magic;
  uint32_t size;
  uint32_t image_crc;
  uint32_t header_crc;
} otahdr_t;


#define BLOCKSHIFT 5
#define BLOCKSIZE (1 << BLOCKSHIFT)

void __attribute__((section("bltext"),noinline,noreturn)) bl_start(void)
{
  reg_wr(WDT_CRV, 20 * 32768); // in seconds
  reg_wr(WDT_CONFIG, 1);       // Keep watchdog running while CPU is asleep
  reg_wr(WDT_RREN, 1);         // RR[0] enable
  reg_wr(WDT_TASKS_START, 1);

#ifdef GPIO_UART_TX
  reg_wr(UART_ENABLE, 4);
  reg_wr(UART_PSELTXD, GPIO_UART_TX);
  reg_wr(UART_BAUDRATE, 0x1d60000); // 115200
#endif

#ifdef GPIO_SPIFLASH_CS

  reg_wr(GPIO_PIN_CNF(GPIO_SPIFLASH_CS), 1); // OUTPUT
  gpio_set_output(GPIO_SPIFLASH_CS, 1);

  reg_wr(SPI_ENABLE, 1);
  reg_wr(SPI_PSEL_SCK, GPIO_SPI_SCLK);
  reg_wr(SPI_PSEL_MOSI, GPIO_SPI_MOSI);
  reg_wr(SPI_PSEL_MISO, GPIO_SPI_MISO);
  reg_wr(SPI_FREQUENCY, 0x80000000);

  spiflash_wakeup();

  otahdr_t hdr;
  spiflash_read(&hdr, 0, 16);
  uint32_t crc = ~crc32(0, &hdr, sizeof(hdr));

  if(!crc && hdr.magic == 0x3141544f) {
    // Upgrade header is valid
    print_char('!');

    uint32_t block[BLOCKSIZE / sizeof(uint32_t)];

    crc = 0;
    for(size_t i = 0; i < hdr.size; i += BLOCKSIZE) {
      size_t chunk = MIN(BLOCKSIZE, hdr.size - i);
      spiflash_read(block, 4096 + i, chunk);
      crc = crc32(crc, block, chunk);
    }
    crc = ~crc;
    if(crc == hdr.image_crc) {
      // Image is valid
      print_char('S');
      print_u32(hdr.size);
      print_char(':');
      print_u32(crc);

      while(1) {

        for(size_t i = 0; i < hdr.size; i += BLOCKSIZE) {
          spiflash_read(block, 4096 + i, BLOCKSIZE);

          int sector = (i >> 12) + 1;
          if((i & 4095) == 0) {
            print_char('.');
            print_u8(sector);
            flash_erase_sector(sector);
          }

          volatile uint32_t *dst = (void *)(intptr_t)(4096 + i);
          reg_wr(NVMC_CONFIG, 1);
          for(size_t j = 0; j < BLOCKSIZE / sizeof(uint32_t); j++) {
            *dst++ = block[j];
          }
          reg_wr(NVMC_CONFIG, 0);
        }
        crc = ~crc32(0, (void *)(intptr_t)4096, hdr.size);
        if(crc == hdr.image_crc)
          break;
      }
      spiflash_erase(0);
    }
  }
  reg_wr(SPI_ENABLE, 0);
#endif

#ifdef GPIO_UART_TX
  reg_wr(UART_ENABLE, 0);
#endif

  void (*init)(void) __attribute__((noreturn))  = (void *)vectors[1];
  init();
}

void __attribute__((section("bltext"),noinline))
bl_nmi(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_hard_fault(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_mm_fault(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_bus_fault(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_usage_fault(void)
{
  while(1) {}
}
