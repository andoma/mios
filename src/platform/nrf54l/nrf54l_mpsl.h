#pragma once

// Bring up Nordic's Multiprotocol Service Layer (binary blob). low_prio is
// invoked from the MPSL low-priority interrupt (IRQ_LEVEL_NET); every
// SDC/MPSL "low priority" API call must run at that same execution priority
// (i.e. inside low_prio, or with IRQ_LEVEL_NET blocked).
void nrf54l_mpsl_init(void (*low_prio)(void));

// Pend the MPSL low-priority interrupt (runs low_prio soon, at NET level).
void nrf54l_mpsl_kick(void);
