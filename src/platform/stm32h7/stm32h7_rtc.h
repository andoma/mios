#pragma once

// PC13, PC14 and PC15 belong to the backup domain. The RTC can force any
// of them to a fixed push-pull level (RTC_TAFCR PCxMODE/PCxVALUE, RM0468
// section 51.7.16) "whatever the GPIO configuration", and the setting
// lives in a register that a system reset does not clear: it survives
// NRST, software and watchdog resets and the time spent in the ROM
// bootloader. Only a backup domain reset or losing VDD and VBAT undoes
// it. Meant for a pin that holds the board's own power on.
//
// pin is 13, 14 or 15. The pin must not have another RTC function
// enabled (tamper, timestamp, alarm or calibration output).
void stm32h7_rtc_force_pin(int pin, int value);
