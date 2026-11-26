#include <mios/ethphy.h>
#include <mios/driver.h>
#include <mios/eventlog.h>
#include <net/ether.h>
#include <stdio.h>
#include <unistd.h>

// Datasheet: https://www.ti.com/lit/ds/symlink/dp83826e.pdf (SNLS647I)
//
// The PHY is always configured by register, whatever its straps latched.
// That matters on a board whose strap network was drawn for ENHANCED mode
// but which runs in BASIC mode (ModeSelect tied to GND), where the same
// straps mean MII (not RMII), MII isolate on, and 10M half duplex. A board
// with ModeSelect in the bootstrap dead band can latch either mode, so the
// writes below must be valid in both.
//
// A software reset reloads the latched straps (SNLS647I section 8.4.1),
// and so does a hard reset. dp83826_init() is therefore written to be
// called again after every PHY reset: it reapplies the configuration
// every time and creates the device only once.

#define REG_BMCR    0x0000
#define REG_BMSR    0x0001
#define REG_PHYIDR1 0x0002
#define REG_PHYIDR2 0x0003
#define REG_ANAR    0x0004
#define REG_PHYSTS  0x0010
#define REG_RCSR    0x0017
#define REG_LEDCR   0x0018
#define REG_PHYCR   0x0019
#define REG_PHYRCR  0x001f
#define REG_SOR1    0x0467

// PHYRCR bit 15: software hard reset, same effect as the reset pin. The
// SMI is ready 2 ms after a reset, so wait 10 ms and then poll the ID.
#define PHYRCR_HARD_RESET (1 << 15)
#define RESET_SETTLE_US   10000

#define MODEL_BASIC    0x11 // PHYIDR2[9:4]: this is the strap mode,
#define MODEL_ENHANCED 0x13 // not the E/I part variant

// BMCR: 100M, full duplex, auto-negotiation enabled and restarted.
// Writing the whole register also clears isolate (bit 10), power-down
// (bit 11) and loopback (bit 14). Speed and duplex bits only matter if
// auto-negotiation is later disabled.
#define BMCR_CONFIG 0x3300
#define BMCR_ISOLATE (1 << 10)
#define BMCR_ANEG_EN (1 << 12)

// ANAR: advertise 100BASE-TX full and half duplex, selector 802.3.
// Use 0x01e1 to also allow a 10BASE-T fallback.
#define ANAR_CONFIG 0x0181

// RCSR: bit 6 is reserved and resets to 1, bits 1:0 = 01 is the default
// elasticity buffer. Bit 7 = 0 selects a 25 MHz crystal reference, which
// makes the PHY the RMII leader (it drives the 50 MHz REF_CLK). A board
// that feeds the PHY a 50 MHz clock instead needs bit 7 set.
#define RCSR_BASE      0x0041
#define RCSR_RMII_MODE (1 << 5)

// PHYCR: bit 15 = auto-MDIX on, bit 14 = force MDI-X (must be 0).
#define PHYCR_AUTO_MDIX  (1 << 15)
#define PHYCR_FORCE_MDIX (1 << 14)

// LEDCR bit 7: link LED (LED0) polarity, 1 = active high. BASIC mode
// defaults to active low, but a board that wires the LED from the pin to
// GND needs active high to light on link.
#define LEDCR_LINK_ACTIVE_HIGH (1 << 7)


device_t *dp83826_init(struct ether_netif *eni, ethphy_mode_t mode, unsigned int flags);



static void
dp83826_print_info(struct device *dev, struct stream *s)
{
  struct ether_netif *eni = (struct ether_netif *)dev->d_parent;

  uint16_t id2 = ethphy_mii_read(eni, REG_PHYIDR2);
  uint16_t model = (id2 >> 4) & 0x3f;

  uint16_t rcsr = ethphy_mii_read(eni, REG_RCSR);
  uint16_t physts = ethphy_mii_read(eni, REG_PHYSTS);
  // PHYSTS[14]: 0 = transmit on TD pair (MDI), 1 = pairs swapped (MDI-X).
  // Auto-MDIX lands differently after every PHY restart, so a fault on
  // one pair looks like an intermittent link. Show which one we are on.
  stprintf(s, "DP83826 rev %d  Mode: %s  Interface: %sMII  Pairs: %s\n",
           id2 & 0xf,
           model == MODEL_ENHANCED ? "ENHANCED" : "BASIC",
           (rcsr & RCSR_RMII_MODE) ? "R" : "",
           (physts & 0x4000) ? "MDI-X (TX on RD)" : "MDI (TX on TD)");
  // PHYSTS reports what the PHY actually resolved, which stays right when
  // the link partner does not auto-negotiate (parallel detection).
  // Bit 0 = link, bit 1 = 10 Mbps (0 = 100 Mbps), bit 2 = full duplex.
  if(physts & 0x0001) {
    stprintf(s, "PHY resolved: %s Mbps %s duplex\n",
             (physts & 0x0002) ? "10" : "100",
             (physts & 0x0004) ? "full" : "half");
  }
  ethphy_print_status(eni, s);
}

static const ethphy_device_class_t ethphy_dp83826 = {
  .dc = {
    .dc_class_name = "DP83826 PHY",
    .dc_print_info = dp83826_print_info,
  },
};


// Overwrite every strap-controlled setting we care about, then read back.
// Returns the number of registers that did not read back as written.
static int
dp83826_configure(struct ether_netif *eni, ethphy_mode_t mode)
{
  const uint16_t rcsr = RCSR_BASE |
    (mode == ETHPHY_MODE_RMII ? RCSR_RMII_MODE : 0);
  ethphy_mii_write(eni, REG_RCSR, rcsr);

  ethphy_mii_write(eni, REG_ANAR, ANAR_CONFIG);

  // PHYCR[4:0] holds the latched PHY address (read only), so modify
  // rather than overwrite.
  uint16_t phycr = ethphy_mii_read(eni, REG_PHYCR);
  phycr = (phycr | PHYCR_AUTO_MDIX) & ~PHYCR_FORCE_MDIX;
  ethphy_mii_write(eni, REG_PHYCR, phycr);

  // The rest of LEDCR (blink rate, drive-LED test bits) keeps its value.
  const uint16_t ledcr =
    ethphy_mii_read(eni, REG_LEDCR) | LEDCR_LINK_ACTIVE_HIGH;
  ethphy_mii_write(eni, REG_LEDCR, ledcr);

  // Last, so auto-negotiation restarts with the new advertisement.
  ethphy_mii_write(eni, REG_BMCR, BMCR_CONFIG);

  const uint16_t rcsr_rb  = ethphy_mii_read(eni, REG_RCSR);
  const uint16_t anar_rb  = ethphy_mii_read(eni, REG_ANAR);
  const uint16_t phycr_rb = ethphy_mii_read(eni, REG_PHYCR);
  const uint16_t ledcr_rb = ethphy_mii_read(eni, REG_LEDCR);
  const uint16_t bmcr_rb  = ethphy_mii_read(eni, REG_BMCR);

  int bad = 0;
  // RCSR bits 3:2 are read-to-clear status, so compare only the
  // configuration bits.
  if((rcsr_rb & 0x01f3) != (rcsr & 0x01f3))
    bad++;
  if(anar_rb != ANAR_CONFIG)
    bad++;
  if((phycr_rb & (PHYCR_AUTO_MDIX | PHYCR_FORCE_MDIX)) != PHYCR_AUTO_MDIX)
    bad++;
  if(!(ledcr_rb & LEDCR_LINK_ACTIVE_HIGH))
    bad++;
  // BMCR bit 9 (restart) self-clears, so check enable and isolate only.
  if((bmcr_rb & (BMCR_ANEG_EN | BMCR_ISOLATE)) != BMCR_ANEG_EN)
    bad++;

  evlog(bad ? LOG_ERR : LOG_DEBUG,
        "dp83826: Configured%s RCSR:%04x ANAR:%04x PHYCR:%04x "
        "LEDCR:%04x BMCR:%04x",
        bad ? " with read-back mismatch," : ",",
        rcsr_rb, anar_rb, phycr_rb, ledcr_rb, bmcr_rb);
  return bad;
}


// Called by the MAC driver after every PHY reset, not only the first.
// Reconfigures the PHY each time; creates the device on the first call
// and returns the existing one after that.
device_t *
dp83826_init(struct ether_netif *eni, ethphy_mode_t mode, unsigned int flags)
{
  uint16_t id1 = ethphy_mii_read(eni, REG_PHYIDR1);
  uint16_t id2 = ethphy_mii_read(eni, REG_PHYIDR2);
  if(id1 != 0x2000 || (id2 & 0xfc00) != 0xa000) {
    evlog(LOG_ERR, "dp83826: Not answering (ID %04x:%04x)", id1, id2);
    return NULL;
  }

  // Second reset. Timing requirements footnote (2) of SNLS647I: if the
  // crystal clock is late relative to the supply ramp, an additional reset
  // is needed once power-on reset has completed and the clock is stable.
  // The PHY answering MDIO here means that is the case, so reset again
  // through PHYRCR bit 15, which acts as the reset pin. This reloads the
  // straps (the older boards can latch a different mode) and clears the
  // RMII bit in RCSR, so the ID is read again and the configuration below
  // is applied afterwards.
  ethphy_mii_write(eni, REG_PHYRCR, PHYRCR_HARD_RESET);
  usleep(RESET_SETTLE_US);
  for(int i = 0;; i++) {
    id1 = ethphy_mii_read(eni, REG_PHYIDR1);
    id2 = ethphy_mii_read(eni, REG_PHYIDR2);
    if(id1 == 0x2000 && (id2 & 0xfc00) == 0xa000)
      break;
    if(i == 100) {
      evlog(LOG_ERR, "dp83826: Not answering after reset (ID %04x:%04x)",
            id1, id2);
      return NULL;
    }
    usleep(1000);
  }
  const uint16_t model = (id2 >> 4) & 0x3f;

  if(eni->eni_phy == NULL) {
    const uint16_t sor = ethphy_mii_read(eni, REG_SOR1);
    // Printed Strap0 first, so character n is Strapn (SOR1 bit n).
    char strapbits[12];
    for(int i = 0; i < 11; i++) {
      strapbits[i] = (sor >> i) & 1 ? '1' : '0';
    }
    strapbits[11] = 0;

    evlog(LOG_DEBUG, "dp83826: Revision %d in %s mode (Strap0-10:%s)",
          id2 & 0xf, model == MODEL_ENHANCED ? "ENHANCED" : "BASIC",
          strapbits);
  }

  // The board is meant to run in BASIC mode. ENHANCED means ModeSelect
  // (pin 1) is not at GND, for example a pull-down that leaves the pin in
  // the bootstrap dead band. The registers below still work, but the SMI
  // address and pin 21/31 functions differ.
  if(model == MODEL_ENHANCED) {
    evlog(LOG_WARNING, "dp83826: Booted in ENHANCED mode; "
          "ModeSelect (pin 1) should be tied to GND");
  }

  dp83826_configure(eni, mode);

  evlog(LOG_DEBUG, "dp83826: Running in %sMII mode",
        mode == ETHPHY_MODE_RMII ? "R":"");

  if(eni->eni_phy != NULL)
    return eni->eni_phy;

  return ethphy_create((device_t *)eni, &ethphy_dp83826, sizeof(device_t));
}

static void *
dp83826_probe(driver_type_t type, device_t *parent)
{
  if(type != DRIVER_TYPE_ETHPHY)
    return NULL;

  // For DRIVER_TYPE_ETHPHY, the parent is a ether_netif_t device
  struct ether_netif *eni = (struct ether_netif *)parent;

  const uint16_t id1 = ethphy_mii_read(eni, REG_PHYIDR1);
  const uint16_t id2 = ethphy_mii_read(eni, REG_PHYIDR2);

  if(id1 != 0x2000 || (id2 & 0xfc00) != 0xa000) {
    return NULL;
  }
  const uint16_t model = (id2 >> 4) & 0x3f;
  if(model != MODEL_ENHANCED && model != MODEL_BASIC) {
    return NULL;
  }

  return &dp83826_init;
}

DRIVER(dp83826_probe, 5);
