#pragma once

// Boot status in BSEC scratch register 0 (survives reset, clears on POR)
#define BSEC_SCRATCH0  0x56009E00

#define BOOTSTATUS_FSBL_RAN     (1 << 0)  // FSBL executed
#define BOOTSTATUS_FSBL_SLOT_B  (1 << 1)  // FSBL was slot B (0=A)
#define BOOTSTATUS_APP_A_DIRTY  (1 << 2)  // App A attempted, not confirmed
#define BOOTSTATUS_APP_B_DIRTY  (1 << 3)  // App B attempted, not confirmed
#define BOOTSTATUS_BOOTED_B     (1 << 4)  // Currently booted from slot B (0=A)
