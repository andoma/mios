#include "t234ccplex_bpmp.h"
#include "t234ccplex_clk.h"

#include <mios/mios.h>
#include <mios/cli.h>
#include <mios/pci.h>
#include <mios/driver.h>
#include <mios/eventlog.h>
#include <mios/task.h>

#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <malloc.h>

#include "reg.h"
#include "irq.h"

#define APPL_PINMUX                           0x000
#define APPL_CTRL                             0x004
#define APPL_INTR_EN_L0_0                     0x008
#define APPL_INTR_EN_L1_8                     0x044
#define APPL_INTR_STATUS_L1_8                 0x04c
#define APPL_LINK_STATUS		      0x0cc
#define APPL_DEBUG                            0x0d0
#define APPL_DM_TYPE                          0x100
#define APPL_CFG_BASE_ADDR                    0x104
#define APPL_CFG_IATU_DMA_BASE_ADDR	      0x108
#define APPL_CFG_MISC                         0x110
#define APPL_CFG_SLCG_OVERRIDE		      0x114
#define APPL_PCIE_MISC                        0x15c

#define PCIE_CAP_LINK_CONTROL_LINK_STATUS_REG   0x80
#define PCIE_CAP_LINK_CONTROL2_LINK_STATUS2_REG	0xa0
#define PORT_LOGIC_AUX_CLK_FREQ                 0xb40
#define PORT_LOGIC_GEN2_CTRL	                0x80c
#define PORT_LOGIC_PORT_LINK_CTRL		0x710

#define ATU_CTRL1                0x00
#define ATU_CTRL2                0x04
#define ATU_LOWER_BASE           0x08
#define ATU_UPPER_BASE           0x0c
#define ATU_LOWER_LIMIT          0x10
#define ATU_LOWER_TARGET         0x14
#define ATU_UPPER_TARGET         0x18
#define ATU_UPPER_LIMIT          0x20

#define PCIE_ATU_TYPE_CFG0       4

#define P2U_CONTROL_GEN1 0x78
#define P2U_PERIODIC_EQ_CTRL_GEN3 0xc0
#define P2U_PERIODIC_EQ_CTRL_GEN4 0xc4
#define P2U_RX_DEBOUNCE_TIME 0xa4
#define P2U_DIR_SEARCH_CTRL 0xd4


typedef struct ctrl_conf {
  // Tegra specific

  uint32_t appl_base;
  uint32_t rp_base;
  uint32_t atu_dma_base;
  uint8_t pcie_controller;
  uint8_t powergate;
  uint8_t core_reset;
  uint8_t apb_reset;
  uint16_t clock;
  uint16_t sys0_irq;
  uint16_t intx_irq_base;

  uint8_t num_phys;
  uint32_t phys[4];

  const char *name;

} ctrl_conf_t;

__attribute__((unused))
static const ctrl_conf_t pcie_c4  = {
  .appl_base = 0x14160000,
  .rp_base   = 0x36080000,
  .atu_dma_base  = 0x36040000,
  .pcie_controller = 4,
  .powergate = 8,
  .core_reset = 120,
  .apb_reset = 125,
  .clock = 224,
  .sys0_irq = 32 + 51,
  .intx_irq_base = 32 + 528,
  .num_phys = 4,
  .phys = {
    0x3e40000,
    0x3e50000,
    0x3e60000,
    0x3e70000,
  },
  .name = "pcie_c4",
};

__attribute__((unused))
static const ctrl_conf_t pcie_c8  = {
  .appl_base = 0x140a0000,
  .rp_base = 0x2a000000,
  .atu_dma_base = 0x2a040000,
  .pcie_controller = 8,
  .powergate = 13,
  .core_reset = 25,
  .apb_reset = 26,
  .clock = 172,
  .sys0_irq = 32 + 356,
  .intx_irq_base = 32 + 428,
  .num_phys = 2,
  .phys = {
    0x3f40000, // p2u_gbe_2
    0x3f50000, // p2u_gbe_3
  },
  .name = "pcie_c8",
};


typedef struct t234_pci_ctrl_t {
  struct device tpc_dev;
  const ctrl_conf_t *tpc_conf;
} t234_pci_ctrl_t;



typedef struct t234_pci_dev {
  pci_dev_t tpd_dev;
  long tpd_cfgbase;
  t234_pci_ctrl_t *tpd_ctrl;
  char tpd_name[32];
} t234_pci_dev_t;


static void
atu_prog(const ctrl_conf_t *cfg, int index, uint32_t type,
         uint64_t base, uint64_t size, uint64_t target)
{
  const uint32_t regbase = cfg->atu_dma_base + index * 0x200;

  reg_wr(regbase + ATU_LOWER_BASE, base);
  reg_wr(regbase + ATU_UPPER_BASE, base >> 32);

  uint64_t limit = base + size - 1;
  reg_wr(regbase + ATU_LOWER_LIMIT, limit);
  reg_wr(regbase + ATU_UPPER_LIMIT, limit >> 32);

  reg_wr(regbase + ATU_LOWER_TARGET, target);
  reg_wr(regbase + ATU_UPPER_TARGET, target >> 32);

  reg_wr(regbase + ATU_CTRL1, type | (1 << 13) /* increase region size */);
  reg_wr(regbase + ATU_CTRL2, (1 << 31)); // Enable
}

static uint8_t cfg_rd8(struct pci_dev *pd, uint32_t reg)
{
  t234_pci_dev_t *tpd = (t234_pci_dev_t *)pd;
  return reg_rd8(tpd->tpd_cfgbase + reg);
}

static uint16_t cfg_rd16(struct pci_dev *pd, uint32_t reg)
{
  t234_pci_dev_t *tpd = (t234_pci_dev_t *)pd;
  return reg_rd16(tpd->tpd_cfgbase + reg);
}

static uint32_t cfg_rd32(struct pci_dev *pd, uint32_t reg)
{
  t234_pci_dev_t *tpd = (t234_pci_dev_t *)pd;
  return reg_rd(tpd->tpd_cfgbase + reg);
}

static void cfg_wr8(struct pci_dev *pd, uint32_t reg, uint8_t val)
{
  t234_pci_dev_t *tpd = (t234_pci_dev_t *)pd;
  reg_wr8(tpd->tpd_cfgbase + reg, val);
}

static void cfg_wr16(struct pci_dev *pd, uint32_t reg, uint16_t val)
{
  t234_pci_dev_t *tpd = (t234_pci_dev_t *)pd;
  reg_wr16(tpd->tpd_cfgbase + reg, val);
}

static void cfg_wr32(struct pci_dev *pd, uint32_t reg, uint32_t val)
{
  t234_pci_dev_t *tpd = (t234_pci_dev_t *)pd;
  reg_wr(tpd->tpd_cfgbase + reg, val);
}

static int irq_attach_intx(struct pci_dev *pd, int x, int level,
                            void (*fn)(void *arg), void *arg)
{
  t234_pci_dev_t *tpd = (t234_pci_dev_t *)pd;
  int irq = tpd->tpd_ctrl->tpc_conf->intx_irq_base + x - PCI_INTA;
  irq_enable_fn_arg(irq, level, fn, arg);
  return irq;
}



static const struct pci_dev_vtable t234ccplex_pci_dev_vtable = {

  .cfg_rd8 = cfg_rd8,
  .cfg_rd16 = cfg_rd16,
  .cfg_rd32 = cfg_rd32,

  .cfg_wr8 = cfg_wr8,
  .cfg_wr16 = cfg_wr16,
  .cfg_wr32 = cfg_wr32,

  .irq_attach_intx = irq_attach_intx,
};


// TODO: Move to common PCI code
// TODO: Use pmem extent allocation instead of dumb mem_base
static error_t
pci_device_probe(pci_dev_t *pd, uint64_t mem_base)
{
  pd->pd_vid = pci_cfg_rd16(pd, 0);
  pd->pd_pid = pci_cfg_rd16(pd, 2);

  if(pd->pd_vid == 0xffff || pd->pd_vid == 0)
    return ERR_NOT_FOUND;

  evlog(LOG_INFO, "%s: Found device %04x:%04x",
        pd->pd_dev.d_name, pd->pd_vid, pd->pd_pid);

  for(int b = 0; b < 6; b++) {
    pci_cfg_wr32(pd, PCI_CFG_BAR(b), 0xffffffff);
  }

  for(int b = 0; b < 6; b++) {
    uint32_t reg = pci_cfg_rd32(pd, PCI_CFG_BAR(b));

    if(reg == 0)
      continue;
    if(reg & 1)
      continue; // IOMEM not supported

    if(reg & 4) {
      // 64bit

      uint64_t hi = pci_cfg_rd32(pd, PCI_CFG_BAR(b + 1));
      uint64_t mask = (reg & ~7) | (hi << 32);
      uint64_t size = (~mask) + 1;

      mem_base = (mem_base + size - 1) & ~(size - 1);

      evlog(LOG_DEBUG, "%s: BAR%d [64bit 0x%lx bytes] mapped at 0x%lx",
            pd->pd_dev.d_name, b, size, mem_base);

      pd->pd_bar[b] = mem_base;
      pci_cfg_wr32(pd, PCI_CFG_BAR(b), mem_base);
      pci_cfg_wr32(pd, PCI_CFG_BAR(b + 1), mem_base >> 32);
      mem_base += size;

      b++;
    } else {
      panic("32bit BAR not implemented");
    }
  }

  uint32_t reg = pci_cfg_rd32(pd, 4);
  reg |= 1 << 1; // Memory Space Enable
  reg |= 1 << 2; // Bus master Enable
  pci_cfg_wr32(pd, 4, reg);

  return driver_attach(&pd->pd_dev);
}


// Move to common PCI code
static void
t234_pci_dev_print(struct device *dev, struct stream *st)
{
  t234_pci_dev_t *tpd = (t234_pci_dev_t *)dev;
  pci_dev_t *pd = &tpd->tpd_dev;

  stprintf(st, "\tVendor:0x%04x Product:0x%04x\n",
           pd->pd_vid, pd->pd_pid);
  for(int i = 0; i < 6; i++) {
    if(pd->pd_bar[i])
      stprintf(st, "\tBAR[%d] = 0x%lx\n", i, pd->pd_bar[i]);
  }
}


static const device_class_t t234_pci_dev_class = {
  .dc_print_info = t234_pci_dev_print,
  .dc_shutdown = device_shutdown,
};

static error_t
probe_ctrl(t234_pci_ctrl_t *tpc)
{
  t234_pci_dev_t *tpd;

  tpd = xalloc(sizeof(t234_pci_dev_t), 0, MEM_MAY_FAIL | MEM_CLEAR);
  if(tpd == NULL)
    return ERR_NO_MEMORY;

  tpd->tpd_dev.pd_vtable = &t234ccplex_pci_dev_vtable;
  tpd->tpd_cfgbase = tpc->tpc_conf->rp_base + 0x20000;
  tpd->tpd_ctrl = tpc;

  int bus = 1;
  int dev = 0;

  atu_prog(tpc->tpc_conf, 0, PCIE_ATU_TYPE_CFG0, tpd->tpd_cfgbase, 0x20000,
           (bus << 24) | (dev << 16));

  device_t *d = &tpd->tpd_dev.pd_dev;

  snprintf(tpd->tpd_name, sizeof(tpd->tpd_name), "%sd%d",
           tpc->tpc_conf->name, dev);
  d->d_name = tpd->tpd_name;
  d->d_type = DEVICE_TYPE_PCI;
  d->d_class = &t234_pci_dev_class;
  d->d_parent = &tpc->tpc_dev;
  device_retain(d->d_parent);

  reg_wr(tpc->tpc_conf->rp_base + 0x108, 0);
  reg_wr(tpc->tpc_conf->rp_base + 0x114, 0);

  device_register(d);
  return pci_device_probe(&tpd->tpd_dev, tpc->tpc_conf->rp_base + 0x200000);
}


static void
p2u_on(uint32_t base)
{
  reg_set_bits(base + P2U_CONTROL_GEN1, 2, 2, 0b01);
  reg_set_bits(base + P2U_PERIODIC_EQ_CTRL_GEN3, 0, 2, 0b10);
  reg_set_bits(base + P2U_PERIODIC_EQ_CTRL_GEN4, 1, 1, 0b1);
  reg_set_bits(base + P2U_RX_DEBOUNCE_TIME, 0, 16, 160);
  reg_clr_bit(base + P2U_DIR_SEARCH_CTRL, 18);
}

static error_t
pcie_stop(const ctrl_conf_t *cfg)
{
  error_t err;

  if((err = bpmp_rst_set(cfg->core_reset, 1)) != 0)
    return err;

  if((err = bpmp_rst_set(cfg->apb_reset, 1)) != 0)
    return err;

  if((err = bpmp_pcie_set(cfg->pcie_controller, 0)) != 0)
    return err;

  if((err = clk_disable(cfg->clock)) != 0)
    return err;

  return bpmp_powergate_set(cfg->powergate, 0);
}


static error_t
t234pcie_shutdown(device_t *d)
{
  t234_pci_ctrl_t *tpc = (t234_pci_ctrl_t *)d;
  device_shutdown(d);

  const ctrl_conf_t *cfg = tpc->tpc_conf;
  // Assert PEX_RESET
  reg_clr_bit(cfg->appl_base + APPL_PINMUX, 0);
  usleep(10);
  return pcie_stop(cfg);
}

static void
t234_pci_rp_print(struct device *dev, struct stream *st)
{
  t234_pci_ctrl_t *tpc = (t234_pci_ctrl_t *)dev;
  uint32_t rp_base = tpc->tpc_conf->rp_base;

  stprintf(st, "\tVendor:0x%04x Product:0x%04x @ 0x%x\n",
           reg_rd16(rp_base), reg_rd16(rp_base + 2), rp_base);


  // Print AER. For Tegra we know it starts at 0x100...
  // Once moved to common PCI code, this needs to be enumerated
  stprintf(st, "\tUncorrectable errors:0x%08x\n",
           reg_rd(rp_base + 0x104));
  stprintf(st, "\tCorrectable errors:  0x%08x\n",
           reg_rd(rp_base + 0x110));
  stprintf(st, "\tRoot port errors:    0x%08x\n",
           reg_rd(rp_base + 0x130));
  stprintf(st, "\tCapture Ctrl:        0x%08x\n",
           reg_rd(rp_base + 0x118));
  stprintf(st, "\tCaptured TLP:        0x%08x:0x%08x:0x%08x:0x%08x\n",
           reg_rd(rp_base + 0x11c),
           reg_rd(rp_base + 0x120),
           reg_rd(rp_base + 0x124),
           reg_rd(rp_base + 0x128));
  stprintf(st, "\tError source:        0x%08x\n",
           reg_rd(rp_base + 0x134));
}


static const device_class_t t234pcie_class = {
  .dc_shutdown = t234pcie_shutdown,
  .dc_print_info = t234_pci_rp_print,
};

static error_t
pcie_start(const ctrl_conf_t *cfg)
{
  error_t err;

  evlog(LOG_INFO, "%s: Initializing", cfg->name);

  if((err = bpmp_powergate_set(cfg->powergate, 1)) != 0)
    return err;

  if((err = bpmp_rst_set(cfg->core_reset, 1)) != 0)
    return err;

  if((err = bpmp_rst_set(cfg->apb_reset, 1)) != 0)
    return err;

  if((err = bpmp_pcie_set(cfg->pcie_controller, 1)) != 0)
    return err;

  if((err = clk_enable(cfg->clock)) != 0)
    return err;

  if((err = bpmp_rst_set(cfg->apb_reset, 0)) != 0)
    return err;

  uint32_t appl_base = cfg->appl_base;
  uint32_t dbi_base = cfg->rp_base;

  reg_wr(appl_base + APPL_CFG_BASE_ADDR, cfg->rp_base);
  reg_wr(appl_base + APPL_CFG_IATU_DMA_BASE_ADDR, cfg->atu_dma_base);
  reg_wr(appl_base + APPL_DM_TYPE, 4); // Configure as root-port

  reg_set_bit(appl_base + APPL_CTRL, 6); // Card detected

  reg_wr(appl_base + APPL_CFG_SLCG_OVERRIDE, 0);

  reg_set_bits(appl_base + APPL_CFG_MISC, 10, 4, 3); // AXI Cache attribute

  reg_set_bits(appl_base + APPL_PINMUX, 2, 2, 0b01); // CLKREQ override

  // Enable PHYs
  for(int i = 0; i < cfg->num_phys; i++) {
    p2u_on(cfg->phys[i]);
  }

  // Release PCIE core
  if((err = bpmp_rst_set(cfg->core_reset, 0)) != 0)
    return err;

  reg_wr(dbi_base + PORT_LOGIC_AUX_CLK_FREQ, 19);

  uint32_t link_speed = 1;
  reg_set_bits(dbi_base + PCIE_CAP_LINK_CONTROL2_LINK_STATUS2_REG, 0, 4,
               link_speed);

  // Lanes
  reg_set_bits(dbi_base + PORT_LOGIC_GEN2_CTRL, 8, 5, 7);

  // Link-cap
  reg_set_bits(dbi_base + PORT_LOGIC_PORT_LINK_CTRL, 16, 6, 7);


  reg_wr(appl_base + APPL_INTR_EN_L1_8,
         (1 << 11) |  // Enable INTx
         0);

  reg_set_bit(appl_base + APPL_INTR_EN_L0_0, 8); // Enable INTx

  reg_wr(appl_base + APPL_PCIE_MISC,
         (1 << 1) | /* Segregated interrupt mode
                     * Route INTA,B,C,D to separate irq vectors */
         0);

  t234_pci_ctrl_t *tpc = xalloc(sizeof(t234_pci_ctrl_t), 0, MEM_CLEAR);
  tpc->tpc_conf = cfg;

  tpc->tpc_dev.d_name = cfg->name;
  tpc->tpc_dev.d_class = &t234pcie_class;
  device_register(&tpc->tpc_dev);

  // Toggle PEX reset
  reg_clr_bit(appl_base + APPL_PINMUX, 0);
  usleep(100);
  // Enable LTSSM
  reg_set_bit(appl_base + APPL_CTRL, 7);

  reg_set_bit(appl_base + APPL_PINMUX, 0);

  int64_t deadline = clock_get() + 2000000;

  evlog(LOG_DEBUG, "%s: Waiting for link up", cfg->name);

  // TODO: Use interrupts here instead of polling
  while(clock_get() < deadline) {
    uint32_t link_status = reg_rd(appl_base + APPL_LINK_STATUS);
    if(link_status & 1) {

      uint32_t rp_link_status =
        reg_rd(dbi_base + PCIE_CAP_LINK_CONTROL_LINK_STATUS_REG);

      //      uint32_t link_speed = (rp_link_status >> 16) & 0xf;
      uint32_t lanes = (rp_link_status >> 20) & 0x3f;
      evlog(LOG_INFO, "%s: Link up, %d lanes", cfg->name, lanes);
      return probe_ctrl(tpc);
    }
    usleep(1000);
  }
  evlog(LOG_ERR, "%s: Link not up", cfg->name);
  pcie_stop(cfg);
  return ERR_NOT_READY;
}

static void *
pcie_init_thread(void *arg)
{
  pcie_start(arg);
  return NULL;
}

static void
pcie_init(const ctrl_conf_t *cfg)
{
  thread_create(pcie_init_thread, (void *)cfg, 0, cfg->name, TASK_DETACHED, 3);
}


static void __attribute__((constructor(1200)))
t234ccplex_pcie_init(void)
{
  pcie_init(&pcie_c8);
  pcie_init(&pcie_c4);
}
