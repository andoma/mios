#pragma once

#include "platform/stm32/stm32_dma.h"

#define STM32F4_DMA1(ca, sa) \
  (0xffff0000 | (ca << 8) | (sa))

#define STM32F4_DMA2(ca, sa, cb, sb)                    \
  (((ca) << 24) | ((sa) << 16) | (cb << 8) | (sb))

#define STM32F4_DMA_SPI1_TX  STM32F4_DMA2(3, 11, 3, 13)
#define STM32F4_DMA_SPI1_RX  STM32F4_DMA2(3, 8,  3, 10)

#define STM32F4_DMA_SPI2_TX  STM32F4_DMA1(0, 4)
#define STM32F4_DMA_SPI2_RX  STM32F4_DMA1(0, 3)

#define STM32F4_DMA_SPI3_TX  STM32F4_DMA2(0, 7, 0, 5)
#define STM32F4_DMA_SPI3_RX  STM32F4_DMA2(0, 2, 0, 0)

#define STM32F4_DMA_USART1_TX STM32F4_DMA1(4, 15)
#define STM32F4_DMA_USART2_TX STM32F4_DMA1(4, 7)
#define STM32F4_DMA_USART3_TX STM32F4_DMA1(4, 3)
#define STM32F4_DMA_UART4_TX  STM32F4_DMA1(4, 4)
#define STM32F4_DMA_UART5_TX  STM32F4_DMA1(4, 7)
#define STM32F4_DMA_USART6_TX STM32F4_DMA2(5, 14, 5, 15)

