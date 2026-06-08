#include <stdint.h>
#include <stddef.h>

#include "stm32n6_reg.h"
#include "stm32n6_clk.h"

// =====================================================================
// Section attributes
// =====================================================================

#define BL __attribute__((section("bltext"), noinline))
#define BL_DATA __attribute__((section("bldata")))

#define BL_STR(name, val) \
  static const char name[] BL_DATA = val

// =====================================================================
// GPIO (same API as MIOS but self-contained in bootloader)
// =====================================================================

#define GPIO_PORT_ADDR(x) (0x56020000 + ((x) * 0x400))
#define GPIO_MODER(x)     (GPIO_PORT_ADDR(x) + 0x00)
#define GPIO_OTYPER(x)    (GPIO_PORT_ADDR(x) + 0x04)
#define GPIO_OSPEEDR(x)   (GPIO_PORT_ADDR(x) + 0x08)
#define GPIO_PUPDR(x)     (GPIO_PORT_ADDR(x) + 0x0c)
#define GPIO_BSRR(x)      (GPIO_PORT_ADDR(x) + 0x18)
#define GPIO_AFRL(x)      (GPIO_PORT_ADDR(x) + 0x20)
#define GPIO_AFRH(x)      (GPIO_PORT_ADDR(x) + 0x24)

#define GPIO(PORT, BIT) (((PORT) << 4) | (BIT))

#define GPIO_PA(x)  GPIO(0, x)
#define GPIO_PB(x)  GPIO(1, x)
#define GPIO_PC(x)  GPIO(2, x)
#define GPIO_PD(x)  GPIO(3, x)
#define GPIO_PE(x)  GPIO(4, x)
#define GPIO_PF(x)  GPIO(5, x)
#define GPIO_PG(x)  GPIO(6, x)
#define GPIO_PH(x)  GPIO(7, x)
#define GPIO_PN(x)  GPIO(13, x)

typedef enum {
  GPIO_PULL_NONE = 0,
  GPIO_PULL_UP = 1,
  GPIO_PULL_DOWN = 2,
} gpio_pull_t;

typedef enum {
  GPIO_PUSH_PULL = 0,
  GPIO_OPEN_DRAIN = 1,
} gpio_output_type_t;

typedef enum {
  GPIO_SPEED_LOW       = 0,
  GPIO_SPEED_MID       = 1,
  GPIO_SPEED_HIGH      = 2,
  GPIO_SPEED_VERY_HIGH = 3,
} gpio_output_speed_t;

typedef uint8_t gpio_t;


static void __attribute__((unused)) BL
gpio_conf_output(gpio_t gpio, gpio_output_type_t type,
                 gpio_output_speed_t speed, gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  clk_enable(CLK_GPIO(port));
  reg_set_bits(GPIO_OTYPER(port),  bit, 1, type);
  reg_set_bits(GPIO_OSPEEDR(port), bit * 2, 2, speed);
  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 1);
}


static void BL
gpio_conf_af(gpio_t gpio, int af, gpio_output_type_t type,
             gpio_output_speed_t speed, gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  clk_enable(CLK_GPIO(port));
  reg_set_bits(GPIO_OTYPER(port), bit, 1, type);
  reg_set_bits(GPIO_OSPEEDR(port), bit * 2, 2, speed);

  if(bit < 8)
    reg_set_bits(GPIO_AFRL(port), bit * 4, 4, af);
  else
    reg_set_bits(GPIO_AFRH(port), (bit - 8) * 4, 4, af);

  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 2);
}


static void __attribute__((unused)) BL
gpio_set_output(gpio_t gpio, int on)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;
  reg_wr(GPIO_BSRR(port), 1 << (bit + !on * 16));
}

// =====================================================================
// USART
// Boot ROM leaves clocks in nominal scenario (default fuses):
//   CPU=400MHz, AHB=150MHz, APB=150MHz
// =====================================================================

#define USART1_BASE  0x52001000
#define USART_CR1    0x00
#define USART_BRR    0x0c
#define USART_SR     0x1c
#define USART_TDR    0x28

// RCC_CCIPR13.USART1SEL[2:0] kernel-clock mux: 000 = pclk2, 110 = hsi_div_ck.
#define RCC_CCIPR13     (RCC_BASE + 0x174)
#define RCC_HSICFGR     (RCC_BASE + 0x48)
#define USART1SEL_PCLK2  0b000
#define USART1SEL_HSI    0b110

static void __attribute__((unused)) BL
bl_uart_init(void)
{
  reg_set_bit(RCC_APB2ENR, 4); // USART1 bus clock (register access)

  // Clock the USART1 *kernel* (baud generator) from hsi_div_ck, not the APB
  // bus. HSI is a fixed 64 MHz RC, so the baud is independent of the bus
  // clock — which the boot ROM leaves in an unpredictable state in USB
  // serial boot (a PLL is locked for the USB PHY). HSIDIV (RCC_HSICFGR[8:7])
  // divides HSI; it is 0 (=64 MHz) after reset.
  reg_set_bits(RCC_CCIPR13, 0, 3, USART1SEL_HSI);
  uint32_t hsi = 64000000u >> ((reg_rd(RCC_HSICFGR) >> 7) & 3);

  gpio_conf_af(GPIO_PE(5), 7, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PE(6), 7, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  reg_wr(USART1_BASE + USART_CR1, 0);
  reg_wr(USART1_BASE + USART_BRR, hsi / 115200);
  reg_wr(USART1_BASE + USART_CR1, (1 << 0) | (1 << 2) | (1 << 3));
}

// Restore the USART1 kernel clock to pclk2 before handing off to mios,
// whose UART driver assumes pclk2.
static void __attribute__((unused)) BL
bl_uart_deinit(void)
{
  reg_set_bits(RCC_CCIPR13, 0, 3, USART1SEL_PCLK2);
}

static void BL
bl_putchar(char c)
{
  while(!(reg_rd(USART1_BASE + USART_SR) & (1 << 7))) {}
  reg_wr(USART1_BASE + USART_TDR, c);
}

static void BL
bl_puts(const char *s)
{
  while(*s) {
    if(*s == '\n')
      bl_putchar('\r');
    bl_putchar(*s++);
  }
}

static void __attribute__((unused)) BL
bl_puthex(uint32_t v)
{
  for(int i = 28; i >= 0; i -= 4) {
    int n = (v >> i) & 0xf;
    bl_putchar(n < 10 ? '0' + n : 'a' + n - 10);
  }
}

// =====================================================================
// Fault handling
// =====================================================================

#define SCB_VTOR    0xE000ED08
#define SCB_SHCSR   0xE000ED24
#define SCB_CFSR    0xE000ED28
#define SCB_HFSR    0xE000ED2C
#define SCB_MMFAR   0xE000ED34
#define SCB_BFAR    0xE000ED38

// Minimal vector table for FSBL (placed in bldata section)
typedef void (*vector_fn)(void);

static void BL bl_hard_fault(void);
static void BL bl_mem_fault(void);
static void BL bl_bus_fault(void);
static void BL bl_usage_fault(void);

// Vector table - must be 128-byte aligned for VTOR
static vector_fn __attribute__((section("blvtbl"), aligned(128), used))
bl_vectors[48] = {
  [0] = (vector_fn)0x34180400,  // MSP (not used, VTOR set after boot)
  [1] = (vector_fn)0,           // Reset (not used)
  [2] = (vector_fn)0,           // NMI
  [3] = bl_hard_fault,
  [4] = bl_mem_fault,
  [5] = bl_bus_fault,
  [6] = bl_usage_fault,
};

static void BL
bl_print_fault(const char *name)
{
  BL_STR(msg_nl, "\n");
  BL_STR(msg_cfsr, "  CFSR: ");
  BL_STR(msg_hfsr, "  HFSR: ");
  BL_STR(msg_mmfar, "  MMFAR: ");
  BL_STR(msg_bfar, "  BFAR: ");

  bl_puts(name);
  bl_puts(msg_nl);

  bl_puts(msg_cfsr);
  bl_puthex(reg_rd(SCB_CFSR));
  bl_puts(msg_nl);

  bl_puts(msg_hfsr);
  bl_puthex(reg_rd(SCB_HFSR));
  bl_puts(msg_nl);

  bl_puts(msg_mmfar);
  bl_puthex(reg_rd(SCB_MMFAR));
  bl_puts(msg_nl);

  bl_puts(msg_bfar);
  bl_puthex(reg_rd(SCB_BFAR));
  bl_puts(msg_nl);

  while(1) {}
}

static void BL bl_hard_fault(void)
{
  BL_STR(msg, "FSBL HARD FAULT");
  bl_print_fault(msg);
}

static void BL bl_mem_fault(void)
{
  BL_STR(msg, "FSBL MEM FAULT");
  bl_print_fault(msg);
}

static void BL bl_bus_fault(void)
{
  BL_STR(msg, "FSBL BUS FAULT");
  bl_print_fault(msg);
}

static void BL bl_usage_fault(void)
{
  BL_STR(msg, "FSBL USAGE FAULT");
  bl_print_fault(msg);
}

static void __attribute__((unused)) BL
bl_setup_vtor(void)
{
  // Enable fault handlers (MemManage, BusFault, UsageFault)
  reg_wr(SCB_SHCSR, reg_rd(SCB_SHCSR) | (1 << 16) | (1 << 17) | (1 << 18));

  // Set VTOR to our vector table
  reg_wr(SCB_VTOR, (uint32_t)bl_vectors);
}

// =====================================================================
// Simple delay
// =====================================================================

static void __attribute__((unused)) BL
bl_delay(int count)
{
  for(volatile int d = 0; d < count; d++) {}
}

// =====================================================================
// =====================================================================
// Watchdog
// =====================================================================

#include "stm32n6_wdog.h"

#define WDOG_TIMEOUT_SEC 3
#define WDOG_HZ 128

static void BL
bl_wdog_init(void)
{
  reg_wr(IWDG_KR, 0x5555);
  reg_wr(IWDG_RLR, WDOG_HZ * WDOG_TIMEOUT_SEC);
  reg_wr(IWDG_PR, 6);  // Prescaler (/256) for 32768 -> WDOG_HZ
  reg_wr(IWDG_KR, 0xAAAA);
  reg_wr(IWDG_KR, 0xCCCC);
}

static void BL
bl_wdog_kick(void)
{
  reg_wr(IWDG_KR, 0xAAAA);
}

// =====================================================================
// XSPI1 memory-mapped mode
// Boot ROM already configured XSPI1→XSPIM_P2 (NOR flash) in indirect
// mode. We switch to memory-mapped mode so flash appears at 0x91000000
// (secure alias for XSPIM_P2).
// =====================================================================

#define XSPI1_BASE   0x58025000
#define XSPI2_BASE   0x5802A000
#define XSPIM_BASE   0x5802B400

#define XSPI_CR   0x000
#define XSPI_DCR1 0x008
#define XSPI_DCR2 0x00c
#define XSPI_SR   0x020
#define XSPI_FCR  0x024
#define XSPI_DLR  0x040
#define XSPI_AR   0x048
#define XSPI_DR   0x050
#define XSPI_TCR  0x108
#define XSPI_CCR  0x100
#define XSPI_IR   0x110

static void BL
bl_xspi_wait_busy(uint32_t base)
{
  while(reg_rd(base + XSPI_SR) & (1 << 5)) {}
}

// Test flash access with indirect read (same method as MIOS driver)
static uint32_t __attribute__((unused)) BL
bl_xspi_read_word(uint32_t base, uint32_t addr)
{
  reg_wr(base + XSPI_CR,
         (0b01 << 28) | 1); // FMODE=READ, Enable

  reg_wr(base + XSPI_DLR, 3); // Read 4 bytes

  reg_wr(base + XSPI_CCR,
         (0b001 << 24) |    // Data on single line
         (0b10 << 12) |     // 24-bit address
         (0b001 << 8) |     // Address on single line
         (0b001 << 0));     // Instruction on single line

  reg_wr(base + XSPI_IR, 0x03); // Basic read

  reg_wr(base + XSPI_AR, addr); // Triggers the read

  bl_xspi_wait_busy(base);
  uint32_t val = reg_rd(base + XSPI_DR);
  reg_wr(base + XSPI_FCR, (1 << 1)); // Clear TCF
  return val;
}

#define PWR_BASE     0x56024800
#define PWR_SVMCR3   (PWR_BASE + 0x3c)

static void BL
bl_xspi_init(void)
{
  // Enable VDDIO3 (Port N power supply for NOR flash, 1.8V)
  // This is the key requirement — PLL is NOT needed for XSPI
  reg_set_bit(PWR_SVMCR3, 26); // VDDIO3VRSEL = 1.8V
  reg_set_bit(PWR_SVMCR3, 9);  // VDDIO3SV
  reg_set_bit(PWR_SVMCR3, 1);  // VDDIO3VMEN

  // Boot ROM used XSPI1 (MODE=1) to load us. Use it to reset the flash
  // before switching to XSPI2, in case CubeProgrammer left it in a
  // non-standard mode that persists across NRST.
  clk_enable(CLK_XSPI1);
  clk_enable(CLK_XSPIM);

  // Flash software reset via XSPI1 (boot ROM's controller, known working)
  reg_wr(XSPI1_BASE + XSPI_FCR, 0x1F);  // Clear all flags
  reg_wr(XSPI1_BASE + XSPI_TCR, 0);     // No dummy cycles

  static const uint8_t reset_cmds[] BL_DATA = { 0x66, 0x99 };
  for(int c = 0; c < 2; c++) {
    reg_wr(XSPI1_BASE + XSPI_CR, (0b00 << 28) | 1); // FMODE=WRITE, EN
    reg_wr(XSPI1_BASE + XSPI_DLR, 0);
    reg_wr(XSPI1_BASE + XSPI_CCR, (0b001 << 0));     // Instruction only
    reg_wr(XSPI1_BASE + XSPI_IR, reset_cmds[c]);
    while(!(reg_rd(XSPI1_BASE + XSPI_SR) & (1 << 1))) {} // Wait TCF
    reg_wr(XSPI1_BASE + XSPI_FCR, (1 << 1));
  }
  for(volatile int i = 0; i < 10000; i++) {} // Wait for flash reset

  // Keep using XSPI1 (MODE=1, boot ROM's config) — don't switch XSPIM.
  // The MODE=1→0 switch doesn't work reliably after CubeProgrammer + NRST.
  // Configure XSPI1 for our use.
  reg_wr(XSPI1_BASE + XSPI_CR, 0);    // Disable
  reg_wr(XSPI1_BASE + XSPI_DCR1,
         (23 << 16) | (0b010 << 24));  // DEVSIZE=23, standard mode
  reg_wr(XSPI1_BASE + XSPI_DCR2, 4);  // Prescaler=4 → ~30MHz
  reg_wr(XSPI1_BASE + XSPI_TCR, 0);   // No dummy cycles
}

// XSPI1 memory-mapped base (datasheet: XSPI1 bank at 0x90000000)
// FSBL uses XSPI1 with XSPIM MODE=1 (boot ROM's config, XSPI1→P2)
#define FLASH_MMAP_BASE  0x90000000

static void BL
bl_xspi_mmap_enable(uint32_t base)
{
  reg_wr(base + XSPI_CR, 0); // Disable

  reg_wr(base + XSPI_CCR,
         (0b001 << 24) |    // Data on single line
         (0b10 << 12) |     // 24-bit address
         (0b001 << 8) |     // Address on single line
         (0b001 << 0));     // Instruction on single line

  reg_wr(base + XSPI_IR, 0x03);

  reg_wr(base + XSPI_CR,
         (0b11 << 28) | 1); // FMODE=memory-mapped, Enable
}

// =====================================================================
// XSPI1 indirect NOR write (1-bit SPI), used to provision NOR over DFU.
// All FSBL/app/selector offsets are < 16 MB, so 24-bit addressing is fine.
// =====================================================================

#define XSPI_SR_TCF    (1 << 1)   // transfer complete
#define XSPI_SR_FTF    (1 << 2)   // FIFO threshold (room to write)

#define NOR_CMD_WREN   0x06
#define NOR_CMD_RDSR   0x05
#define NOR_CMD_PP     0x02       // page program, 24-bit address
#define NOR_CMD_SE     0x20       // 4 KB sector erase, 24-bit address
#define NOR_PAGE_SIZE  256
#define NOR_SECTOR     4096

// Single instruction, no address or data (e.g. write-enable).
static void BL
bl_nor_cmd(uint8_t cmd)
{
  reg_wr(XSPI1_BASE + XSPI_CR, (0b00 << 28) | 1); // FMODE=WRITE, EN
  reg_wr(XSPI1_BASE + XSPI_DLR, 0);
  reg_wr(XSPI1_BASE + XSPI_CCR, (0b001 << 0));    // instruction, 1 line
  reg_wr(XSPI1_BASE + XSPI_IR, cmd);
  while(!(reg_rd(XSPI1_BASE + XSPI_SR) & XSPI_SR_TCF)) {}
  reg_wr(XSPI1_BASE + XSPI_FCR, XSPI_SR_TCF);
}

static uint8_t BL
bl_nor_status(void)
{
  reg_wr(XSPI1_BASE + XSPI_CR, (0b01 << 28) | 1); // FMODE=READ, EN
  reg_wr(XSPI1_BASE + XSPI_DLR, 0);               // one byte
  reg_wr(XSPI1_BASE + XSPI_CCR, (0b001 << 24) | (0b001 << 0));
  reg_wr(XSPI1_BASE + XSPI_IR, NOR_CMD_RDSR);
  while(!(reg_rd(XSPI1_BASE + XSPI_SR) & XSPI_SR_TCF)) {}
  uint8_t st = reg_rd8(XSPI1_BASE + XSPI_DR);
  reg_wr(XSPI1_BASE + XSPI_FCR, XSPI_SR_TCF);
  return st;
}

// Read the 3-byte JEDEC ID (diagnostic): nonzero/non-0xff => NOR responds.
static uint32_t BL
bl_nor_rdid(void)
{
  reg_wr(XSPI1_BASE + XSPI_CR, (0b01 << 28) | 1); // FMODE=READ, EN
  reg_wr(XSPI1_BASE + XSPI_DLR, 2);               // 3 bytes
  reg_wr(XSPI1_BASE + XSPI_CCR, (0b001 << 24) | (0b001 << 0));
  reg_wr(XSPI1_BASE + XSPI_IR, 0x9F);             // RDID
  while(!(reg_rd(XSPI1_BASE + XSPI_SR) & XSPI_SR_TCF)) {}
  uint32_t id = reg_rd(XSPI1_BASE + XSPI_DR);
  reg_wr(XSPI1_BASE + XSPI_FCR, XSPI_SR_TCF);
  return id;
}

// Read 4 bytes at addr (1-bit fast-read 0x03), TCF-based (diagnostic).
static uint32_t BL
bl_nor_read32(uint32_t addr)
{
  reg_wr(XSPI1_BASE + XSPI_CR, (0b01 << 28) | 1); // FMODE=READ, EN
  reg_wr(XSPI1_BASE + XSPI_DLR, 3);               // 4 bytes
  reg_wr(XSPI1_BASE + XSPI_CCR,
         (0b001 << 24) | (0b10 << 12) | (0b001 << 8) | (0b001 << 0));
  reg_wr(XSPI1_BASE + XSPI_IR, 0x03);
  reg_wr(XSPI1_BASE + XSPI_AR, addr);
  while(!(reg_rd(XSPI1_BASE + XSPI_SR) & XSPI_SR_TCF)) {}
  uint32_t v = reg_rd(XSPI1_BASE + XSPI_DR);
  reg_wr(XSPI1_BASE + XSPI_FCR, XSPI_SR_TCF);
  return v;
}

// Spin until the write-in-progress bit clears, kicking the watchdog.
static void BL
bl_nor_wait_wip(void)
{
  while(bl_nor_status() & 1)
    bl_wdog_kick();
}

static void BL
bl_nor_erase_sector(uint32_t addr)
{
  bl_nor_cmd(NOR_CMD_WREN);
  reg_wr(XSPI1_BASE + XSPI_CR, (0b00 << 28) | 1);
  reg_wr(XSPI1_BASE + XSPI_DLR, 0);
  reg_wr(XSPI1_BASE + XSPI_CCR,
         (0b000 << 24) |   // no data
         (0b10 << 12) |    // 24-bit address
         (0b001 << 8) |    // address, 1 line
         (0b001 << 0));    // instruction, 1 line
  reg_wr(XSPI1_BASE + XSPI_IR, NOR_CMD_SE);
  reg_wr(XSPI1_BASE + XSPI_AR, addr);
  while(!(reg_rd(XSPI1_BASE + XSPI_SR) & XSPI_SR_TCF)) {}
  reg_wr(XSPI1_BASE + XSPI_FCR, XSPI_SR_TCF);
  bl_nor_wait_wip();
}

// Program up to one page (<= 256 bytes, not crossing a page boundary).
static void BL
bl_nor_program_page(uint32_t addr, const uint8_t *src, uint32_t len)
{
  bl_nor_cmd(NOR_CMD_WREN);
  reg_wr(XSPI1_BASE + XSPI_CR, (0b00 << 28) | 1); // FMODE=WRITE, EN
  reg_wr(XSPI1_BASE + XSPI_DLR, len - 1);
  reg_wr(XSPI1_BASE + XSPI_CCR,
         (0b001 << 24) |   // data, 1 line
         (0b10 << 12) |    // 24-bit address
         (0b001 << 8) |    // address, 1 line
         (0b001 << 0));    // instruction, 1 line
  reg_wr(XSPI1_BASE + XSPI_IR, NOR_CMD_PP);
  reg_wr(XSPI1_BASE + XSPI_AR, addr);
  for(uint32_t i = 0; i < len; i++) {
    while(!(reg_rd(XSPI1_BASE + XSPI_SR) & XSPI_SR_FTF)) {}
    reg_wr8(XSPI1_BASE + XSPI_DR, src[i]);
  }
  while(!(reg_rd(XSPI1_BASE + XSPI_SR) & XSPI_SR_TCF)) {}
  reg_wr(XSPI1_BASE + XSPI_FCR, XSPI_SR_TCF);
  bl_nor_wait_wip();
}

// Erase the sectors spanning [addr, addr+len) and program src over them.
static void BL
bl_nor_write(uint32_t addr, const uint8_t *src, uint32_t len)
{
  for(uint32_t a = addr & ~(NOR_SECTOR - 1); a < addr + len; a += NOR_SECTOR)
    bl_nor_erase_sector(a);

  while(len) {
    uint32_t chunk = NOR_PAGE_SIZE - (addr & (NOR_PAGE_SIZE - 1));
    if(chunk > len)
      chunk = len;
    bl_nor_program_page(addr, src, chunk);
    addr += chunk;
    src += chunk;
    len -= chunk;
  }
}

// =====================================================================
// ELF loader
// =====================================================================

#define EI_MAG0    0
#define EI_CLASS   4
#define ELFCLASS32 1
#define EM_ARM     40
#define PT_LOAD    1

typedef struct {
  uint8_t  e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint32_t e_entry;
  uint32_t e_phoff;
  uint32_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_offset;
  uint32_t p_vaddr;
  uint32_t p_paddr;
  uint32_t p_filesz;
  uint32_t p_memsz;
  uint32_t p_flags;
  uint32_t p_align;
} Elf32_Phdr;

// Flash partition offsets (must match stm32n6_flash.c)
#define FSBL1_OFFSET        0x000000
#define FSBL2_OFFSET        0x040000
#define BOOTSELECTOR_OFFSET 0x080000
#define APP_A_OFFSET        0x100000
#define APP_B_OFFSET        0x300000

static void BL
bl_memcpy(void *dst, const void *src, uint32_t len)
{
  uint8_t *d = dst;
  const uint8_t *s = src;
  while(len--)
    *d++ = *s++;
}

static void BL
bl_memset(void *dst, int val, uint32_t len)
{
  uint8_t *d = dst;
  while(len--)
    *d++ = val;
}

// Standard CRC-32 matching mios crc32(0, ...) output
static uint32_t BL
bl_crc32(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFF;
  for(uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    for(int j = 0; j < 8; j++) {
      uint32_t mask = -(crc & 1);
      crc = (crc >> 1) ^ (0xEDB88320 & mask);
    }
  }
  return crc ^ 0xFFFFFFFF;
}

#define MIOS_APP_MAGIC    0x5041496d  // "mIAP" little-endian

static int BL
bl_load_elf(const uint8_t *base)
{
  // Check for partition header
  uint32_t magic = *(const uint32_t *)(base + 0);
  uint32_t length = *(const uint32_t *)(base + 4);

  if(magic != MIOS_APP_MAGIC) {
    BL_STR(msg, "  No mIAP header\n");
    bl_puts(msg);
    return -1;
  }

  // Verify CRC trailer: last 12 bytes = [~crc32(4)]["mI0sIMG1"(8)]
  // ~crc32(0, image_data + crc_field, length - 8) == 0
  if(length < 12) {
    BL_STR(msg, "  Image too small\n");
    bl_puts(msg);
    return -1;
  }

  bl_wdog_kick();
  uint32_t crc = bl_crc32(base + 8, length - 8);
  if(~crc != 0) {
    BL_STR(msg, "  CRC mismatch\n");
    bl_puts(msg);
    return -1;
  }

  base += 8;  // Skip partition header, point to ELF

  const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)base;

  // Validate ELF magic
  if(ehdr->e_ident[0] != 0x7f ||
     ehdr->e_ident[1] != 'E' ||
     ehdr->e_ident[2] != 'L' ||
     ehdr->e_ident[3] != 'F') {
    BL_STR(msg, "  Not an ELF\n");
    bl_puts(msg);
    return -1;
  }

  if(ehdr->e_ident[EI_CLASS] != ELFCLASS32 || ehdr->e_machine != EM_ARM) {
    BL_STR(msg, "  Not ARM32 ELF\n");
    bl_puts(msg);
    return -1;
  }

  const Elf32_Phdr *phdr = (const Elf32_Phdr *)(base + ehdr->e_phoff);

  // Pre-scrub ITCM with 32-bit zero stores so its ECC is valid before any
  // segment is loaded there. bl_memcpy() below is byte-by-byte and would
  // trip ECC read-modify-write faults on uninitialised words otherwise.
  for(volatile uint32_t *p = (volatile uint32_t *)0x00000000;
      p < (volatile uint32_t *)0x00010000; p++) {
    *p = 0;
  }

  for(int i = 0; i < ehdr->e_phnum; i++) {
    if(phdr[i].p_type != PT_LOAD)
      continue;

    // Skip segments targeting the bootloader's own memory (download buffer)
    if(phdr[i].p_paddr >= 0x34180000 && phdr[i].p_paddr < 0x34200000)
      continue;

    BL_STR(msg_load, "  LOAD ");
    BL_STR(msg_arrow, " -> ");
    BL_STR(msg_sz, " (");
    BL_STR(msg_bytes, " bytes)\n");

    bl_puts(msg_load);
    bl_puthex(phdr[i].p_offset);
    bl_puts(msg_arrow);
    bl_puthex(phdr[i].p_paddr);
    bl_puts(msg_sz);
    bl_puthex(phdr[i].p_filesz);
    bl_puts(msg_bytes);

    // Copy segment from memory-mapped flash to RAM
    bl_wdog_kick();
    bl_memcpy((void *)phdr[i].p_paddr,
              base + phdr[i].p_offset,
              phdr[i].p_filesz);

    // Zero-fill BSS (memsz > filesz)
    if(phdr[i].p_memsz > phdr[i].p_filesz) {
      bl_memset((void *)(phdr[i].p_paddr + phdr[i].p_filesz),
                0,
                phdr[i].p_memsz - phdr[i].p_filesz);
    }
  }

  return ehdr->e_entry;
}

// =====================================================================
// Boot ROM context (UM3234 Table 23)
// =====================================================================

struct boot_context {
  uint32_t boot_partition;     // 0=none, 1=FSBL1, 2=FSBL2
  uint32_t sd_err[6];         // SD error counters
  uint32_t emmc_status[3];    // eMMC status
  uint16_t boot_interface;    // 4=sNOR XSPI, 5=UART, 6=USB, 8=HyperFlash
  uint16_t boot_instance;
  uint32_t hse_clock_hz;
  uint32_t reserved;
  uint32_t auth_status;       // 0=none, 1=failed, 2=success
} __attribute__((packed));

// End of the .boot section in the loaded image (linker symbol). The host
// parks the mIAP-wrapped mios ELF immediately after .boot, 32-byte aligned.
extern char _boot_end[];

#include "stm32n6_bootstatus.h"

#define DL_BUFFER_BASE  0x34180000u   // ROM download buffer (secure alias)
#define BOOT_BASE       0x34180400u   // .boot lands here (= _boot_start)

// Erase+program a NOR region, then read back and verify the leading magic
// word. Verbose so a one-shot provisioning is legible on the console.
static int BL
bl_nor_region(const char *name, uint32_t off, const uint8_t *src,
              uint32_t len, uint32_t magic)
{
  BL_STR(s_at, " @0x");
  BL_STR(s_len, " len 0x");
  BL_STR(s_wr, " writing...");
  BL_STR(s_ok, " OK\n");
  BL_STR(s_bad, " VERIFY FAILED, got 0x");
  BL_STR(s_nl, "\n");

  bl_puts(name);
  bl_puts(s_at);  bl_puthex(off);
  bl_puts(s_len); bl_puthex(len);
  bl_puts(s_wr);

  bl_nor_write(off, src, len);

  uint32_t got = bl_nor_read32(off);
  if(got == magic) {
    bl_puts(s_ok);
    return 0;
  }
  bl_puts(s_bad);
  bl_puthex(got);
  bl_puts(s_nl);
  return -1;
}

// Provision NOR from the just-downloaded image in SRAM. The download buffer
// at 0x34180000 holds [STM2 header 0x400][.boot @ 0x400][parked mIAP app].
// Write the FSBL (header + .boot) to both FSBL slots and the parked app to
// both app slots, set the boot selector, and verify each region. The FSBL
// header is the v2.3 image we booted from, re-scoped so image_length and
// checksum cover .boot only (measured from the 0x240 header+ext end, so it
// includes the zero align gap before .boot).
static void BL
bl_provision_nor(void)
{
  BL_STR(m_start, "Provisioning NOR via XSPI1 (1-bit SPI)\n");
  bl_puts(m_start);

  // Route XSPI1 -> Port 2 (the NOR): XSPIM swapped mode, and configure the
  // Port-2 pins (CS/CLK/IO0/IO1). The boot ROM does both for cold boot, but
  // not for USB serial boot.
  clk_enable(CLK_XSPIM);
  reg_wr(XSPIM_BASE, 0x02);  // MUXEN=0, MODE=1 (swapped): XSPI1 <-> Port 2
  gpio_conf_af(GPIO_PN(1), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(2), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(3), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(6), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  bl_xspi_init();

  BL_STR(m_id, "  JEDEC ID 0x");
  BL_STR(m_nl, "\n");
  bl_puts(m_id);
  bl_puthex(bl_nor_rdid());
  bl_puts(m_nl);

  // Re-scope the in-RAM STM2 header for the FSBL slot (.boot only).
  uint32_t boot_padded = (((uint32_t)_boot_end - BOOT_BASE) + 31) & ~31u;
  uint8_t *hdr = (uint8_t *)DL_BUFFER_BASE;
  *(volatile uint32_t *)(hdr + 0x6c) = (0x400 - 0x240) + boot_padded;
  uint32_t sum = 0;
  const uint8_t *boot = (const uint8_t *)BOOT_BASE;
  for(uint32_t i = 0; i < boot_padded; i++)
    sum += boot[i];
  *(volatile uint32_t *)(hdr + 0x64) = sum;
  uint32_t fsbl_len = 0x400 + boot_padded;

  // Parked mIAP app right after .boot; byte length = mIAP length field
  // (elf_len + 12) + the 8-byte magic+length prefix.
  const uint8_t *parked = (const uint8_t *)(((uint32_t)_boot_end + 31) & ~31u);
  uint32_t app_len = *(const uint32_t *)(parked + 4) + 8;

  BL_STR(n_fsbl1, "  FSBL1");
  BL_STR(n_fsbl2, "  FSBL2");
  BL_STR(n_appa, "  App A");
  BL_STR(n_appb, "  App B");
  int bad = 0;
  bad |= bl_nor_region(n_fsbl1, FSBL1_OFFSET, hdr, fsbl_len, 0x324d5453);
  bad |= bl_nor_region(n_fsbl2, FSBL2_OFFSET, hdr, fsbl_len, 0x324d5453);
  bad |= bl_nor_region(n_appa, APP_A_OFFSET, parked, app_len, MIOS_APP_MAGIC);
  bad |= bl_nor_region(n_appb, APP_B_OFFSET, parked, app_len, MIOS_APP_MAGIC);

  // Boot selector: 'A'.
  uint8_t sel = 'A';
  bl_nor_write(BOOTSELECTOR_OFFSET, &sel, 1);
  BL_STR(m_sel, "  Selector 'A' @0x080000\n");
  bl_puts(m_sel);

  // Clean boot status so the cold boot starts fresh.
  reg_wr(BSEC_SCRATCH0, 0);

  BL_STR(m_ok, "NOR provisioning complete\n");
  BL_STR(m_fail, "NOR provisioning FAILED verification\n");
  bl_puts(bad ? m_fail : m_ok);
}

// =====================================================================
// Entry point
// =====================================================================

void __attribute__((noreturn)) BL
bl_main(void *ctx_arg)
{
  // Start the watchdog: resets the chip if FSBL or app startup hangs.
  bl_wdog_init();

  // Bring up the UART for the boot log (clocked from HSI, see bl_uart_init).
  bl_uart_init();

  bl_setup_vtor();

  BL_STR(msg_banner, "MIOS FSBL v1\n");
  bl_puts(msg_banner);

  BL_STR(msg_nl2, "\n");

  // Read and update boot status (BSEC scratch reg, survives reset)
#include "stm32n6_bootstatus.h"

  uint32_t bootstatus = reg_rd(BSEC_SCRATCH0);
  bootstatus |= BOOTSTATUS_FSBL_RAN;

  // Record which FSBL slot ran
  uint32_t ctx_addr = (uint32_t)ctx_arg;
  uint16_t boot_if = 0;
  if(ctx_addr == 0x24100000 || ctx_addr == 0x34100000) {
    const struct boot_context *ctx = ctx_arg;
    if(ctx->boot_partition == 2)
      bootstatus |= BOOTSTATUS_FSBL_SLOT_B;
    boot_if = ctx->boot_interface;
  }

  // Serial boot (boot_interface 5=UART, 6=USB): the ROM downloaded our
  // bootstrap image into SRAM. Provision NOR from it, then drop straight
  // into the freshly-downloaded mios from RAM. NOR is now provisioned, so
  // the next cold boot (BOOT pins -> NOR) runs from flash and OTA takes over.
  // Resetting here would only re-enter the DFU ROM (BOOT0 is still held for
  // DFU), so jump to RAM instead — leaving a running system behind.
  if(boot_if == 5 || boot_if == 6) {
    BL_STR(msg_serial, "Serial boot: provisioning NOR from RAM\n");
    bl_puts(msg_serial);
    bl_provision_nor();

    const uint8_t *parked =
      (const uint8_t *)(((uint32_t)_boot_end + 31) & ~31u);
    int entry = bl_load_elf(parked);
    if(entry > 0) {
      // Reset XSPI and switch to MODE=0 for MIOS (uses XSPI2)
      reg_wr(XSPI1_BASE + XSPI_CR, 0);
      rst_assert(CLK_XSPI1);
      rst_assert(CLK_XSPI2);
      rst_assert(CLK_XSPIM);
      for(volatile int i = 0; i < 100; i++) {}
      rst_deassert(CLK_XSPI1);
      rst_deassert(CLK_XSPI2);
      rst_deassert(CLK_XSPIM);
      reg_wr(XSPIM_BASE, 0x00); // MODE=0: XSPI2->P2

      BL_STR(msg_run, "Booting mios from RAM\n");
      bl_puts(msg_run);
      bl_uart_deinit();
      ((void (*)(void))entry)();
    }

    BL_STR(msg_bad, "Parked image load failed, halting.\n");
    bl_puts(msg_bad);
    while(1) {}
  }

  // Initialize XSPI and switch to memory-mapped mode
  bl_xspi_init();
  bl_xspi_mmap_enable(XSPI1_BASE);

  const uint8_t *flash = (const uint8_t *)FLASH_MMAP_BASE;

  // Read boot selector: 'B' = slot B, anything else = slot A (default)
  uint8_t selector = flash[BOOTSELECTOR_OFFSET];
  int primary_is_b = (selector == 'B');

  // Determine which slots to try, respecting dirty bits
  uint32_t primary_dirty = primary_is_b ? BOOTSTATUS_APP_B_DIRTY
                                        : BOOTSTATUS_APP_A_DIRTY;
  uint32_t fallback_dirty = primary_is_b ? BOOTSTATUS_APP_A_DIRTY
                                         : BOOTSTATUS_APP_B_DIRTY;

  uint32_t primary_offset  = primary_is_b ? APP_B_OFFSET : APP_A_OFFSET;
  uint32_t fallback_offset = primary_is_b ? APP_A_OFFSET : APP_B_OFFSET;

  int entry = -1;
  int booted_b = primary_is_b;

  if(!(bootstatus & primary_dirty)) {
    // Primary slot is clean — try it
    BL_STR(msg_try, "Boot ");
    bl_puts(msg_try);
    bl_putchar(primary_is_b ? 'B' : 'A');
    bl_puts(msg_nl2);

    bootstatus |= primary_dirty;
    if(primary_is_b)
      bootstatus |= BOOTSTATUS_BOOTED_B;
    else
      bootstatus &= ~BOOTSTATUS_BOOTED_B;
    reg_wr(BSEC_SCRATCH0, bootstatus);

    entry = bl_load_elf(flash + primary_offset);
  } else {
    BL_STR(msg_dirty, "Slot ");
    bl_puts(msg_dirty);
    bl_putchar(primary_is_b ? 'B' : 'A');
    BL_STR(msg_d2, " dirty, skip\n");
    bl_puts(msg_d2);
  }

  if(entry < 0 && !(bootstatus & fallback_dirty)) {
    // Fallback slot is clean — try it
    booted_b = !primary_is_b;
    BL_STR(msg_fb, "Fallback ");
    bl_puts(msg_fb);
    bl_putchar(booted_b ? 'B' : 'A');
    bl_puts(msg_nl2);

    bootstatus |= fallback_dirty;
    if(booted_b)
      bootstatus |= BOOTSTATUS_BOOTED_B;
    else
      bootstatus &= ~BOOTSTATUS_BOOTED_B;
    reg_wr(BSEC_SCRATCH0, bootstatus);

    entry = bl_load_elf(flash + fallback_offset);
  }

  if(entry < 0) {
    BL_STR(msg_halt, "No bootable application, halting.\n");
    bl_puts(msg_halt);
    while(1) {}
  }

  // Jump to application
  BL_STR(msg_jump, "Jumping to ");
  bl_puts(msg_jump);
  bl_puthex(entry);
  bl_puts(msg_nl2);

  // Set VTOR to application vector table (entry should point to reset handler,
  // VTOR is typically at the start of the first PT_LOAD segment)
  // The application's startup code will set VTOR itself.

  // Reset XSPI and switch to MODE=0 for MIOS (uses XSPI2)
  reg_wr(XSPI1_BASE + XSPI_CR, 0);
  rst_assert(CLK_XSPI1);
  rst_assert(CLK_XSPI2);
  rst_assert(CLK_XSPIM);
  for(volatile int i = 0; i < 100; i++) {}
  rst_deassert(CLK_XSPI1);
  rst_deassert(CLK_XSPI2);
  rst_deassert(CLK_XSPIM);
  reg_wr(XSPIM_BASE, 0x00); // MODE=0: XSPI2→P2

  // Jump to application
  bl_uart_deinit();
  typedef void (*app_entry_t)(void);
  app_entry_t app = (app_entry_t)entry;
  app();

  __builtin_unreachable();
}

// bl_fault - called from isr.S if needed before VTOR is set up
void __attribute__((noreturn)) BL
bl_fault(void)
{
  // Try to print if UART was already initialized
  BL_STR(msg, "FSBL EARLY FAULT\n");
  bl_puts(msg);
  while(1) {}
}
