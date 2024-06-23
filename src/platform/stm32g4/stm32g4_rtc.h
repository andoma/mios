#pragma once

#define RTC_BASE 0x40002800

#define RTC_ICSR (RTC_BASE + 0x0c)
#define RTC_WUTR (RTC_BASE + 0x14)
#define RTC_CR   (RTC_BASE + 0x18)
#define RTC_WPR  (RTC_BASE + 0x24)
#define RTC_SR   (RTC_BASE + 0x50)
#define RTC_SCR  (RTC_BASE + 0x5c)
