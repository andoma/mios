#pragma once

#include <stdint.h>

#include "device.h"

struct pci_dev;

#define PCI_CFG_BAR(x) (0x10 + (x) * 4)

#define PCI_INTA 1
#define PCI_INTB 2
#define PCI_INTC 3
#define PCI_INTD 4

typedef struct pci_dev_vtable {

  uint8_t (*cfg_rd8)(struct pci_dev *pd, uint32_t reg);
  uint16_t (*cfg_rd16)(struct pci_dev *pd, uint32_t reg);
  uint32_t (*cfg_rd32)(struct pci_dev *pd, uint32_t reg);

  void (*cfg_wr8)(struct pci_dev *pd, uint32_t reg, uint8_t val);
  void (*cfg_wr16)(struct pci_dev *pd, uint32_t reg, uint16_t val);
  void (*cfg_wr32)(struct pci_dev *pd, uint32_t reg, uint32_t val);

  int (*irq_attach_intx)(struct pci_dev *pd, int x, int level,
                         void (*fn)(void *arg), void *arg);

} pci_dev_vtable_t;

typedef struct pci_dev {

  struct device pd_dev;

  const pci_dev_vtable_t *pd_vtable;

  uint16_t pd_vid;
  uint16_t pd_pid;

  uint32_t pd_classcode;

  long pd_bar[6];

} pci_dev_t;


static inline uint8_t pci_cfg_rd8(struct pci_dev *pd, uint32_t reg)
{
  return pd->pd_vtable->cfg_rd8(pd, reg);
}

static inline uint16_t pci_cfg_rd16(struct pci_dev *pd, uint32_t reg)
{
  return pd->pd_vtable->cfg_rd16(pd, reg);
}

static inline uint32_t pci_cfg_rd32(struct pci_dev *pd, uint32_t reg)
{
  return pd->pd_vtable->cfg_rd32(pd, reg);
}

static inline void pci_cfg_wr8(struct pci_dev *pd, uint32_t reg, uint8_t val)
{
  pd->pd_vtable->cfg_wr8(pd, reg, val);
}

static inline void pci_cfg_wr16(struct pci_dev *pd, uint32_t reg, uint16_t val)
{
  pd->pd_vtable->cfg_wr16(pd, reg, val);
}

static inline void pci_cfg_wr32(struct pci_dev *pd, uint32_t reg, uint32_t val)
{
  pd->pd_vtable->cfg_wr32(pd, reg, val);
}

static inline int pci_irq_attach_intx(struct pci_dev *pd, int x, int level,
                                       void (*fn)(void *arg), void *arg)
{
  return pd->pd_vtable->irq_attach_intx(pd, x, level, fn, arg);
}
