#pragma once

#define TIM1_BASE   0x40012c00
#define TIM3_BASE   0x40000400
#define TIM4_BASE   0x40000800
#define TIM6_BASE   0x40001000
#define TIM7_BASE   0x40001400
#define TIM14_BASE  0x40002000

#define TIM15_BASE  0x40014000
#define TIM16_BASE  0x40014400
#define TIM17_BASE  0x40014800


#define TIMx_CR1   0x00
#define TIMx_CR2   0x04
#define TIMx_SMCR  0x08
#define TIMx_DIER  0x0c
#define TIMx_SR    0x10
#define TIMx_EGR   0x14

#define TIMx_CNT   0x24
#define TIMx_PSC   0x28
#define TIMx_ARR   0x2c
