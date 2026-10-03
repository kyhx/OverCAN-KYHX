/**
 * @file    node1_bsp.h
 * @brief 节点一 STM32F103 BSP 的对外接口（**唯一 include HAL 的地方**）。
 *
 * 为什么要单独一层：node1_app.c 之所以能在 PC 上跑单测，就是因为它只认
 * node1_hal_t vtable，不认任何 STM32 的头。这个头文件是那道"防火墙"的外壳 ——
 * 换 MCU 时只需重写本目录，BSP 内部的寄存器操作不会泄漏进业务层。
 *
 * 目标：STM32F103C8T6（Cortex-M3，72MHz，20KB SRAM）
 * 依赖：STM32 HAL + CMSIS（由 CubeMX / 手工引入的 stm32f1xx_hal 包提供）
 */
#ifndef NODE1_BSP_H
#define NODE1_BSP_H

#include "node1_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化全部外设：GPIO / ADC / TIM4 PWM / USART1 / bxCAN / IWDG。
 *
 * 顺序有讲究：
 *   1. 先 HAL_Init + 时钟（72MHz）—— 其余外设的时钟源依赖它
 *   2. **STBY 先拉低**：驱动芯片进待机，避免 IN 引脚浮空时电机抽搐
 *   3. 再配 PWM（但占空比保持 0）
 *   4. 最后开 CAN —— 避免"还没准备好就开始收帧"
 *
 * ⚠️ 本函数**不启动调度器**，只准备硬件。启动顺序由 main 决定。
 */
void node1_bsp_init(const node1_hal_t **out_hal);

/** 喂独立看门狗。IWDG 一旦启动**软件关不掉**，所有长循环都必须调用。 */
void node1_bsp_feed_watchdog(void);

/** 软复位（响应主站 RESET 命令）。内部会先做最小清理再复位。 */
void node1_bsp_soft_reset(void);

/**
 * 打印栈高水位（走 RTT/串口，不含浮点）。
 * @note 移植到 FreeRTOS 后应由调度器在启动后调用一次
 */
void node1_bsp_print_stack_watermarks(void);

#ifdef __cplusplus
}
#endif

#endif /* NODE1_BSP_H */
