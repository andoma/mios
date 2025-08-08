#include <mios/ethphy.h>
#include <mios/eventlog.h>

#include <stdio.h>

// Datasheet: https://www.ti.com/lit/ds/symlink/dp83826e.pdf

#define REG_PHYIDR1 0x0002
#define REG_PHYIDR2 0x0003
#define REG_REGCR   0x000d
#define REG_ADDAR   0x000e
#define REG_RCSR    0x0017
#define REG_SOR1    0x0467

static uint16_t
reg_read(const ethphy_reg_io_t *regio, void *arg, uint16_t reg)
{
  if(reg < 0x20) {
    return regio->read(arg, reg);
  }

  // Extended register access
  regio->write(arg, REG_REGCR, 0x1f);
  regio->write(arg, REG_ADDAR, reg);
  regio->write(arg, REG_REGCR, 0x401f);
  return regio->read(arg, REG_ADDAR);
}

static void
reg_write(const ethphy_reg_io_t *regio, void *arg, uint16_t reg, uint16_t val)
{
  if(reg < 0x20) {
    return regio->write(arg, reg, val);
  }

  // Extended register access
  regio->write(arg, REG_REGCR, 0x1f);
  regio->write(arg, REG_ADDAR, reg);
  regio->write(arg, REG_REGCR, 0x401f);
  return regio->write(arg, REG_ADDAR, val);
}


static error_t
dp83826_init(ethphy_mode_t mode,
             const ethphy_reg_io_t *regio,
             void *arg)
{
  uint16_t id1 = reg_read(regio, arg, REG_PHYIDR1);
  uint16_t id2 = reg_read(regio, arg, REG_PHYIDR2);

  if(id1 != 0x2000 || (id2 & 0xfc00) != 0xa000) {
    evlog(LOG_ERR, "dp83826: Invalid PHY ID 0x%04x:0x%04x", id1, id2);
    return ERR_NO_DEVICE;
  }
  uint16_t model = (id2 >> 4) & 0x3f;
  if(model != 0x13 && model != 0x11) {
    evlog(LOG_ERR, "dp83826: Invalid model 0x%04x:0x%04x", id1, id2);
    return ERR_NO_DEVICE;
  }

  uint16_t sor = reg_read(regio, arg, REG_SOR1);
  char strapbits[12];
  for(int i = 0; i < 11; i++) {
    strapbits[i] = (sor >> i) & 1 ? '1' : '0';
  }
  strapbits[11] = 0;

  evlog(LOG_DEBUG, "dp83826: Revision %d in %s mode (Strap:%s)",
        id2 & 0xf, model == 0x13 ? "Enhanced" : "Basic", strapbits);

  uint16_t rcsr = reg_read(regio, arg, REG_RCSR);
  printf("RCSR:%x\n", rcsr);
  if(mode == ETHPHY_MODE_RMII) {
    rcsr |= 0x20;
  } else {
    rcsr &= ~0x20;
  }
  evlog(LOG_DEBUG, "dp83826: Running in %sMII mode",
        mode == ETHPHY_MODE_RMII ? "R":"");
  printf("RCSR:%x\n", rcsr);
  reg_write(regio, arg, REG_RCSR, rcsr);

  rcsr = reg_read(regio, arg, REG_RCSR);
  printf("RCSR:%x\n", rcsr);
  return 0;
}


const ethphy_driver_t ethphy_dp83826 = {
  .init = dp83826_init,
};
