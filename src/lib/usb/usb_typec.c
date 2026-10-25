// USB Type-C connection state machine (Type-C spec 4.5.2), without the
// Try.SRC / Try.SNK and accessory states.
//
// A dual role port toggles between Unattached.SNK and Unattached.SRC
// until the CC lines show a partner. VBUS is polled if the board can
// measure it.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <mios/device.h>
#include <mios/stream.h>
#include <mios/task.h>

#include <usb/typec.h>

#include "irq.h"

#define T_CC_DEBOUNCE  150000  // tCCDebounce, 100-200 ms
#define T_PD_DEBOUNCE   15000  // tPDDebounce, 10-20 ms
#define T_DRP_SNK       40000  // tDRP is 50-100 ms with the source part
#define T_DRP_SRC       35000  // (dcSRC.DRP) between 30 and 70 %
#define T_VBUS_POLL     50000  // There is no VBUS interrupt

#define VSAFE0V_MV        800
#define VBUS_PRESENT_MV  3670  // vSinkDisconnect

typedef enum {
  ST_UNATTACHED_SNK,
  ST_ATTACHWAIT_SNK,
  ST_ATTACHED_SNK,
  ST_UNATTACHED_SRC,
  ST_ATTACHWAIT_SRC,
  ST_ATTACHED_SRC,
} typec_state_t;

static const char *state_names[] = {
  [ST_UNATTACHED_SNK] = "Unattached.SNK",
  [ST_ATTACHWAIT_SNK] = "AttachWait.SNK",
  [ST_ATTACHED_SNK]   = "Attached.SNK",
  [ST_UNATTACHED_SRC] = "Unattached.SRC",
  [ST_ATTACHWAIT_SRC] = "AttachWait.SRC",
  [ST_ATTACHED_SRC]   = "Attached.SRC",
};

static const char *cc_names[] = {
  [TYPEC_CC_OPEN]   = "open",
  [TYPEC_CC_RA]     = "Ra",
  [TYPEC_CC_RD]     = "Rd",
  [TYPEC_CC_RP_DEF] = "Rp (default)",
  [TYPEC_CC_RP_1A5] = "Rp (1.5A)",
  [TYPEC_CC_RP_3A0] = "Rp (3.0A)",
};

struct typec_port {
  device_t p_dev;
  typec_phy_t *p_phy;
  typec_port_config_t p_cfg;

  task_waitable_t p_wait;
  uint8_t p_cc_event;     // Protected by IRQ_LEVEL_IO

  typec_state_t p_state;
  int64_t p_deadline;     // State timer, 0 if not running
  typec_cc_t p_cc[2];     // Previous sample
  int p_vbus_mv;          // Last measurement, -1 if not measured
  uint8_t p_polarity;
  uint8_t p_debouncing;   // Detach debounce running in an attached state
  uint32_t p_attaches;
};


static void
typec_cc_changed(void *opaque)
{
  typec_port_t *p = opaque;
  p->p_cc_event = 1;
  task_wakeup(&p->p_wait, 0);
}


static int
is_rp(typec_cc_t cc)
{
  return cc >= TYPEC_CC_RP_DEF;
}


static void
enter_state(typec_port_t *p, typec_state_t state, int timeout)
{
  typec_phy_t *tp = p->p_phy;

  if(state == ST_UNATTACHED_SNK)
    tp->tp_vtable->set_pull(tp, TYPEC_PULL_RD);
  else if(state == ST_UNATTACHED_SRC)
    tp->tp_vtable->set_pull(tp, p->p_cfg.rp);

  p->p_state = state;
  p->p_deadline = timeout ? clock_get() + timeout : 0;
  p->p_debouncing = 0;
}


static int64_t
vbus_poll_deadline(typec_port_t *p)
{
  return p->p_cfg.vbus_mv ? clock_get() + T_VBUS_POLL : 0;
}


// Where to go when there is no partner (anymore)
static void
enter_unattached(typec_port_t *p)
{
  switch(p->p_cfg.type) {
  case TYPEC_PORT_SRC:
    enter_state(p, ST_UNATTACHED_SRC, 0);
    break;
  case TYPEC_PORT_SNK:
    enter_state(p, ST_UNATTACHED_SNK, 0);
    break;
  case TYPEC_PORT_DRP:
    enter_state(p, ST_UNATTACHED_SNK, T_DRP_SNK);
    break;
  }
}


static void
set_role(typec_port_t *p, typec_role_t role)
{
  if(role != TYPEC_ROLE_NONE) {
    p->p_attaches++;
    p->p_phy->tp_vtable->set_polarity(p->p_phy, p->p_polarity);
    printf("%s: Attached as %s (CC%d)\n", p->p_dev.d_name,
           role == TYPEC_ROLE_SOURCE ? "source" : "sink", p->p_polarity + 1);
  } else {
    printf("%s: Detached\n", p->p_dev.d_name);
  }
  p->p_cfg.role_changed(p->p_cfg.opaque, role);
}


// Advance the state machine given the current CC state
static void
typec_step(typec_port_t *p, const typec_cc_t cc[2], int expired)
{
  const int changed = cc[0] != p->p_cc[0] || cc[1] != p->p_cc[1];
  const int rds = (cc[0] == TYPEC_CC_RD) + (cc[1] == TYPEC_CC_RD);
  const int vbus = p->p_vbus_mv;
  const int vbus_present = vbus < 0 || vbus >= VBUS_PRESENT_MV;
  const int vbus_safe0v = vbus < VSAFE0V_MV; // Also when not measured

  switch(p->p_state) {
  case ST_UNATTACHED_SNK:
    if(is_rp(cc[0]) || is_rp(cc[1]))
      enter_state(p, ST_ATTACHWAIT_SNK, T_CC_DEBOUNCE);
    else if(expired)
      enter_state(p, ST_UNATTACHED_SRC, T_DRP_SRC);
    break;

  case ST_ATTACHWAIT_SNK:
    if(!is_rp(cc[0]) && !is_rp(cc[1])) {
      enter_unattached(p);
    } else if(changed) {
      p->p_deadline = clock_get() + T_CC_DEBOUNCE;
    } else if(expired && !vbus_present) {
      // The source turns VBUS on after its own debounce
      p->p_deadline = vbus_poll_deadline(p);
    } else if(expired) {
      p->p_polarity = is_rp(cc[1]);
      enter_state(p, ST_ATTACHED_SNK, 0);
      set_role(p, TYPEC_ROLE_SINK);
      p->p_deadline = vbus_poll_deadline(p);
    }
    break;

  case ST_ATTACHED_SNK:
    if(p->p_cfg.vbus_mv) {
      // With VBUS measurement the spec says to detach when VBUS goes
      if(!vbus_present) {
        set_role(p, TYPEC_ROLE_NONE);
        enter_unattached(p);
      } else {
        p->p_deadline = vbus_poll_deadline(p);
      }
    } else if(is_rp(cc[p->p_polarity])) {
      p->p_debouncing = 0;
      p->p_deadline = 0;
    } else if(p->p_debouncing && expired) {
      set_role(p, TYPEC_ROLE_NONE);
      enter_unattached(p);
    } else if(!p->p_debouncing) {
      p->p_debouncing = 1;
      p->p_deadline = clock_get() + T_PD_DEBOUNCE;
    }
    break;

  case ST_UNATTACHED_SRC:
    if(rds == 1)
      enter_state(p, ST_ATTACHWAIT_SRC, T_CC_DEBOUNCE);
    else if(expired)
      enter_state(p, ST_UNATTACHED_SNK, T_DRP_SNK);
    break;

  case ST_ATTACHWAIT_SRC:
    if(rds != 1) {
      enter_unattached(p);
    } else if(changed) {
      p->p_deadline = clock_get() + T_CC_DEBOUNCE;
    } else if(expired && !vbus_safe0v) {
      // Don't drive VBUS while something else does (or it's discharging)
      p->p_deadline = vbus_poll_deadline(p);
    } else if(expired) {
      p->p_polarity = cc[1] == TYPEC_CC_RD;
      enter_state(p, ST_ATTACHED_SRC, 0);
      set_role(p, TYPEC_ROLE_SOURCE);
    }
    break;

  case ST_ATTACHED_SRC:
    if(cc[p->p_polarity] == TYPEC_CC_RD) {
      p->p_debouncing = 0;
      p->p_deadline = 0;
    } else if(p->p_debouncing && expired) {
      set_role(p, TYPEC_ROLE_NONE);
      enter_unattached(p);
    } else if(!p->p_debouncing) {
      p->p_debouncing = 1;
      p->p_deadline = clock_get() + T_PD_DEBOUNCE;
    }
    break;
  }

  p->p_cc[0] = cc[0];
  p->p_cc[1] = cc[1];
}


__attribute__((noreturn))
static void *
typec_thread(void *arg)
{
  typec_port_t *p = arg;
  typec_phy_t *tp = p->p_phy;

  enter_unattached(p);

  while(1) {
    typec_cc_t cc[2];
    tp->tp_vtable->get_cc(tp, cc);
    p->p_vbus_mv = p->p_cfg.vbus_mv ? p->p_cfg.vbus_mv(p->p_cfg.opaque) : -1;
    const int expired = p->p_deadline && clock_get() >= p->p_deadline;
    typec_step(p, cc, expired);

    int q = irq_forbid(IRQ_LEVEL_IO);
    while(!p->p_cc_event) {
      if(p->p_deadline) {
        if(task_sleep_deadline(&p->p_wait, p->p_deadline))
          break;
      } else {
        task_sleep(&p->p_wait);
      }
    }
    p->p_cc_event = 0;
    irq_permit(q);
  }
}


static void
typec_print_info(struct device *d, struct stream *st)
{
  typec_port_t *p = (typec_port_t *)d;
  typec_cc_t cc[2];
  p->p_phy->tp_vtable->get_cc(p->p_phy, cc);

  stprintf(st, "State: %s\n", state_names[p->p_state]);
  stprintf(st, "CC1: %s  CC2: %s\n", cc_names[cc[0]], cc_names[cc[1]]);
  if(p->p_cfg.vbus_mv) {
    const int mv = p->p_cfg.vbus_mv(p->p_cfg.opaque);
    stprintf(st, "VBUS: %d.%02d V\n", mv / 1000, (mv % 1000) / 10);
  }
  if(p->p_state == ST_ATTACHED_SNK || p->p_state == ST_ATTACHED_SRC)
    stprintf(st, "Active CC: CC%d\n", p->p_polarity + 1);
  stprintf(st, "Attaches: %d\n", p->p_attaches);
}


static const device_class_t typec_class = {
  .dc_class_name = "usb-typec",
  .dc_print_info = typec_print_info,
};


typec_port_t *
typec_port_create(typec_phy_t *tp, const typec_port_config_t *cfg,
                  const char *name)
{
  typec_port_t *p = calloc(1, sizeof(typec_port_t));
  p->p_phy = tp;
  p->p_cfg = *cfg;

  p->p_dev.d_name = name;
  p->p_dev.d_class = &typec_class;
  device_register(&p->p_dev);

  tp->tp_opaque = p;
  tp->tp_cc_changed = typec_cc_changed;

  thread_create(typec_thread, p, 2048, name, 0, 5);
  return p;
}
