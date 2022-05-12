#pragma once

#define TIM16_BASE 0x40014400

#define TIMx_CR1   0x00
#define TIMx_CR2   0x04
#define TIMx_SMCR  0x08
#define TIMx_DIER  0x0c
#define TIMx_SR    0x10
#define TIMx_EGR   0x14
#define TIMx_CCMR1 0x18
#define TIMx_CCMR2 0x1c
#define TIMx_CCER  0x20

#define TIMx_CNT   0x24
#define TIMx_PSC   0x28
#define TIMx_ARR   0x2c

#define TIMx_CCRx(x) (0x30 + (x) * 4)

#define TIMx_CCR1  TIMx_CCRx(1)
#define TIMx_CCR2  TIMx_CCRx(2)
#define TIMx_CCR3  TIMx_CCRx(3)
#define TIMx_CCR4  TIMx_CCRx(4)


#define TIMx_BDTR  0x44
#define TIMx_CCMR3 0x54
#define TIMx_CCR5  0x58
#define TIMx_CCR6  0x5c

