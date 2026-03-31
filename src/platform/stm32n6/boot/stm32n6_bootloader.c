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

static void __attribute__((unused)) BL
bl_uart_init(int apb_freq)
{
  reg_set_bit(RCC_APB2ENR, 4); // USART1 clock

  gpio_conf_af(GPIO_PE(5), 7, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PE(6), 7, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  reg_wr(USART1_BASE + USART_CR1, 0);
  reg_wr(USART1_BASE + USART_BRR, apb_freq / 115200);
  reg_wr(USART1_BASE + USART_CR1, (1 << 0) | (1 << 2) | (1 << 3));
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

// XSPI2 memory-mapped base (datasheet: XSPI2 at 0x70000000)
#define FLASH_MMAP_BASE  0x70000000

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

  // Reset XSPI subsystem

  rst_assert(CLK_XSPI1);
  rst_assert(CLK_XSPI2);
  rst_assert(CLK_XSPIM);
  for(volatile int i = 0; i < 100; i++) {}
  rst_deassert(CLK_XSPI1);
  rst_deassert(CLK_XSPI2);
  rst_deassert(CLK_XSPIM);

  // Enable clocks
  clk_enable(CLK_XSPI2);
  clk_enable(CLK_XSPIM);

  // XSPIM MODE=0: XSPI2→P2 (same as MIOS)
  reg_wr(XSPIM_BASE, 0x00);

  // GPIO pins for NOR flash
  gpio_conf_af(GPIO_PN(1), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(2), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(3), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(6), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  // Configure XSPI2
  reg_wr(XSPI2_BASE + XSPI_DCR1,
         (23 << 16) | (0b010 << 24));  // DEVSIZE=23, standard mode
  reg_wr(XSPI2_BASE + XSPI_DCR2, 4);  // Prescaler=4 → ~30MHz SPI clock
  reg_wr(XSPI2_BASE + XSPI_TCR, 0);   // No dummy cycles
}

// XSPI2 memory-mapped base (datasheet: XSPI2 bank at 0x70000000)
#define FLASH_MMAP_BASE  0x70000000

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
#define APP_A_OFFSET  0x100000
#define APP_B_OFFSET  0x300000

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

static int BL
bl_load_elf(const uint8_t *base)
{
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

// =====================================================================
// Entry point
// =====================================================================

void __attribute__((noreturn)) BL
bl_main(void *ctx_arg)
{
  // Blink LED to show we're alive
  gpio_conf_output(GPIO_PG(10), GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  for(int n = 0; n < 6; n++) {
    gpio_set_output(GPIO_PG(10), n & 1);
    bl_delay(200000);
  }

  // Auto-detect clock: if PLL1 is running (nominal), APB2=150MHz, else 32MHz
  int apb2_freq = (reg_rd(RCC_SR) & (1 << 8)) ? 150000000 : 32000000;
  bl_uart_init(apb2_freq);

  bl_setup_vtor();

  BL_STR(msg_banner, "\nMIOS FSBL v1\n");
  bl_puts(msg_banner);

  BL_STR(msg_nl2, "\n");

  // Initialize XSPI2 and switch to memory-mapped mode
  bl_xspi_init();
  bl_xspi_mmap_enable(XSPI2_BASE);

  // Try to load ELF from App A partition
  BL_STR(msg_app_a, "Loading App A:\n");
  bl_puts(msg_app_a);

  const uint8_t *flash = (const uint8_t *)FLASH_MMAP_BASE;
  int entry = bl_load_elf(flash + APP_A_OFFSET);

  if(entry < 0) {
    // Try App B
    BL_STR(msg_app_b, "Loading App B:\n");
    bl_puts(msg_app_b);

    entry = bl_load_elf(flash + APP_B_OFFSET);
  }

  if(entry < 0) {
    BL_STR(msg_halt, "No valid application, halting.\n");
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

  // Reset XSPI subsystem for clean MIOS handoff
  reg_wr(XSPI2_BASE + XSPI_CR, 0);
  rst_assert(CLK_XSPI1);
  rst_assert(CLK_XSPI2);
  rst_assert(CLK_XSPIM);
  for(volatile int i = 0; i < 100; i++) {}
  rst_deassert(CLK_XSPI1);
  rst_deassert(CLK_XSPI2);
  rst_deassert(CLK_XSPIM);

  // Jump to application
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
