#pragma once

// USB Type-C connection management.
//
// A Type-C PHY (such as the STM32 UCPD) controls the pull resistors on the
// CC lines and reports what it sees. The Type-C layer runs the attach /
// detach state machine on top of it and tells board code which role to
// take. USB Power Delivery (usb_pd.h) uses the same PHY.

#include <stddef.h>
#include <stdint.h>

struct stream;

// CC line state as seen by the port
typedef enum {
  TYPEC_CC_OPEN,    // Nothing attached
  TYPEC_CC_RA,      // Powered cable or VCONN load (seen as source)
  TYPEC_CC_RD,      // Sink attached (seen as source)
  TYPEC_CC_RP_DEF,  // Source attached, default USB current (seen as sink)
  TYPEC_CC_RP_1A5,  // Source attached, 1.5 A
  TYPEC_CC_RP_3A0,  // Source attached, 3.0 A
} typec_cc_t;

// What the port presents on its CC lines
typedef enum {
  TYPEC_PULL_RD,
  TYPEC_PULL_RP_DEF,
  TYPEC_PULL_RP_1A5,
  TYPEC_PULL_RP_3A0,
} typec_pull_t;


typedef struct typec_phy typec_phy_t;

typedef struct typec_phy_vtable {
  void (*set_pull)(typec_phy_t *tp, typec_pull_t pull);
  void (*get_cc)(typec_phy_t *tp, typec_cc_t cc[2]);
  // Select the CC line used for USB PD (0 = CC1, 1 = CC2)
  void (*set_polarity)(typec_phy_t *tp, int cc);

  // USB Power Delivery transport, NULL if the PHY can't do PD

  // Enable or disable the PD receiver on the selected CC line
  void (*pd_rx_enable)(typec_phy_t *tp, int on);

  // Start transmission of a SOP packet: header and data objects, the PHY
  // adds the CRC. buf is copied. Completion is reported through
  // tp_pd_tx_done. Callable from interrupt context
  void (*pd_transmit)(typec_phy_t *tp, const uint8_t *buf, size_t len);

  // Send Hard Reset signaling, completion via tp_pd_hard_reset
  void (*pd_hard_reset)(typec_phy_t *tp);

  // Optional, PHY statistics for the device info
  void (*print_info)(typec_phy_t *tp, struct stream *st);
} typec_phy_vtable_t;

struct typec_phy {
  const typec_phy_vtable_t *tp_vtable;

  // Installed by the Type-C layer. Called by the PHY driver from
  // interrupt context (at most IRQ_LEVEL_IO) when the CC state changes
  void (*tp_cc_changed)(void *opaque);
  void *tp_opaque;

  // Installed by the PD layer. Called by the PHY driver from interrupt
  // context (at most IRQ_LEVEL_IO)

  // A SOP packet with valid CRC was received
  void (*tp_pd_rx)(void *opaque, const uint8_t *buf, size_t len);

  // Transmission done: 0 if sent, an error if discarded or aborted
  void (*tp_pd_tx_done)(void *opaque, int status);

  // Hard Reset received (received = 1) or our Hard Reset sent (0)
  void (*tp_pd_hard_reset)(void *opaque, int received);

  void *tp_pd_opaque;
};


struct usb_pd;


typedef enum {
  TYPEC_PORT_SNK,
  TYPEC_PORT_SRC,
  TYPEC_PORT_DRP,  // Dual role, toggles between source and sink
} typec_port_type_t;

typedef enum {
  TYPEC_ROLE_NONE,    // Detached
  TYPEC_ROLE_SINK,    // Attached as sink, USB device
  TYPEC_ROLE_SOURCE,  // Attached as source, USB host
} typec_role_t;

typedef struct typec_port typec_port_t;

typedef struct typec_port_config {
  typec_port_type_t type;
  typec_pull_t rp;  // Current to advertise as source

  // Called from the Type-C thread. On SOURCE the board turns on VBUS and
  // starts USB host mode, on SINK it starts USB device mode, on NONE it
  // turns VBUS off (if it was on) and stops USB. May block
  void (*role_changed)(void *opaque, typec_role_t role);

  // Optional, returns VBUS in mV. With it a sink attaches only once VBUS
  // is present and detaches when it goes away, and a source waits for
  // VBUS to be below vSafe0V before turning it on. Without it a sink
  // detaches when Rp goes away
  int (*vbus_mv)(void *opaque);

  // Optional USB Power Delivery, started when attached as sink
  struct usb_pd *pd;

  void *opaque;
} typec_port_config_t;


typec_port_t *typec_port_create(typec_phy_t *tp,
                                const typec_port_config_t *cfg,
                                const char *name);
