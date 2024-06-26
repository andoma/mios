#pragma once

#define TIM1_BASE  0x40012c00
#define TIM2_BASE  0x40000000
#define TIM3_BASE  0x40000400
#define TIM4_BASE  0x40000800
#define TIM5_BASE  0x40000c00
#define TIM6_BASE  0x40001000
#define TIM7_BASE  0x40001400
#define TIM8_BASE  0x40013400
#define TIM15_BASE 0x40014000
#define TIM16_BASE 0x40014400
#define TIM17_BASE 0x40014800
#define TIM20_BASE 0x40015000


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
#define TIMx_CCR1  0x34
#define TIMx_CCR2  0x38
#define TIMx_CCR3  0x3c
#define TIMx_CCR4  0x40
#define TIMx_BDTR  0x44
#define TIMx_CCR5  0x48
#define TIMx_CCR6  0x4c
#define TIMx_CCMR3 0x50

#define TIMx_CCRx(x)  (0x30 + (x) * 4)
