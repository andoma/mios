#pragma once

#define TIM2_BASE  0x40000000
#define TIM3_BASE  0x40000400
#define TIM4_BASE  0x40000800
#define TIM5_BASE  0x40000c00
#define TIM6_BASE  0x40001000
#define TIM7_BASE  0x40001400
#define TIM8_BASE  0x40010400

#define TIM15_BASE 0x40014000
#define TIM16_BASE 0x40014400
#define TIM17_BASE 0x40014800

#define TIMx_CR1    0x0
#define TIMx_CR2    0x4
#define TIMx_SMCR   0x8
#define TIMx_DIER   0xc
#define TIMx_SR     0x10
#define TIMx_CCMR1  0x18
#define TIMx_CCMR2  0x1c
#define TIMx_CCER   0x20
#define TIMx_CNT    0x24
#define TIMx_PSC    0x28
#define TIMx_ARR    0x2c

#define TIMx_CCR1   0x34
#define TIMx_CCR2   0x38
#define TIMx_CCR3   0x3c
#define TIMx_CCR4   0x40

#define TIMx_BDTR   0x44

#define TIMx_CCRx(x)  (0x30 + (x) * 4)
