#include <mios/ethphy.h>
#include <mios/eventlog.h>



static error_t
dp83826_init(ethphy_mode_t mode,
             const ethphy_reg_io_t *regio,
             void *arg)
{
  uint16_t rcsr = regio->read(arg, 0x17);
  evlog(LOG_DEBUG, "RCSR:%x", rcsr);

  if(mode == ETHPHY_MODE_RMII) {
    rcsr |= 0x20;
  } else {
    rcsr &= ~0x20;
  }

  regio->write(arg, 0x17, rcsr);
  return 0;
}


const ethphy_driver_t ethphy_dp83826 = {
  .init = dp83826_init,
};
