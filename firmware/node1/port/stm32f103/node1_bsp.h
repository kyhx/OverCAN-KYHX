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
 * 初始化系统时钟（**兜底**，通常不需要显式调用）。
 *
 * 行为：若 SystemCoreClock 已达 72MHz（说明 CubeMX 的 main() 已配好时钟）则
 * 立即返回；否则用 HSE 8MHz × PLL9 配到 72MHz。
 * 做成"按实测值判断"而不是"两个入口各配一次"，是为了避免时钟被重复配置——
 * 重复配置在 F1 上会短暂失去 SYSCLK，表现为随机死机。
 */
void node1_bsp_init_system(void);

/**
 * 推进毫秒时基（**在 SysTick 中断里调用**）。
 *
 * ⚠️ 本模块**不定义 SysTick_Handler**：CubeMX 生成的 Core/Src/stm32f1xx_it.c
 * 已有一个（内部调 HAL_IncTick）。请在它的 `USER CODE BEGIN SysTick_IRQn 1`
 * 段落里调用本函数，否则 node1_hal_t::millis() 永远返回 0，调度器一个任务都不会跑。
 */
void node1_bsp_tick_ms(void);

/**
 * 初始化全部外设：GPIO / ADC / TIM4 PWM / USART1 / bxCAN / IWDG。
 *
 * 顺序有讲究：
 *   1. **STBY 先拉低**：驱动芯片进待机，避免 IN 引脚浮空时电机抽搐
 *   2. 再配 PWM（但占空比保持 0）
 *   3. 最后开 CAN —— 避免"还没准备好就开始收帧"
 *
 * ⚠️ 本函数**不启动调度器**，只准备硬件；也不重复配置系统时钟
 * （见 node1_bsp_init_system 的兜底逻辑）。
 */
void node1_bsp_init(const node1_hal_t **out_hal);

/** 喂独立看门狗。IWDG 一旦启动**软件关不掉**，所有长循环都必须调用。 */
void node1_bsp_feed_watchdog(void);

/** 软复位（响应主站 RESET 命令）。内部会先做最小清理再复位。 */
void node1_bsp_soft_reset(void);

/**
 * 固件主体入口：初始化 BSP + 业务层 + 调度层，然后**常驻主循环，不返回**。
 *
 * 定义在 node1_main.c。之所以单独抽一个"不返回的入口"，是为了让 CubeMX 生成的
 * `main()` 只需一行调用——用户代码段不会被重新生成覆盖，也就不存在"生成器把
 * 我们的初始化逻辑冲掉"的风险。
 */
void node1_firmware_run(void);

/**
 * 打印栈高水位（走 RTT/串口，不含浮点）。
 * @note 移植到 FreeRTOS 后应由调度器在启动后调用一次
 */
void node1_bsp_print_stack_watermarks(void);

#ifdef __cplusplus
}
#endif

#endif /* NODE1_BSP_H */
