#pragma once

struct stream;

// 802.15.4 background-RX client (driven by the radio arbiter).
void nrf54l_154_init(void);

// Called by the arbiter when the radio is handed to / taken from 15.4.
void nrf54l_154_resume(void);
void nrf54l_154_suspend(void);
void nrf54l_154_irq(void);

// Print the 15.4 RX stats (used by the 'radio' device's info in 'dev').
void nrf54l_154_print(struct stream *st);
