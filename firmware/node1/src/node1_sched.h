/**
 * @file    node1_sched.h
 * @brief 节点一调度器：**与 RTOS 无关**的周期任务驱动逻辑。
 *
 * 为什么要把调度做成"纯逻辑"：本项目的任务划分（100ms 传感器 / 1s 心跳 /
 *  1s 遥测）决定了"最坏情况下每个任务延后多久执行"。这段逻辑如果写在
 *  FreeRTOS 任务体里，就只能在真机上看表现；做成 `while (!due) {}` 的
 *  纯判断后，可以在 PC 上直接测：给定 t=0..5000ms 的时间序列，断言
 *  传感器任务恰好跑了 50 次、遥测恰好 5 次。
 *
 * 真实固件有两种接法：
 *   -裸机 super-loop（node1_main_bare.c）：直接循环调 node1_sched_tick()
 *   - FreeRTOS（node1_main_freertos.c）：各任务体只调自己的 node1_sched_tick()
 *
 * 两者共用本文件的判定逻辑，保证"裸机验证过的时序"与"RTOS 上的时序"一致。
 */
#ifndef NODE1_SCHED_H
#define NODE1_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#include "node1_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单个周期任务的到期状态。 */
typedef struct {
    uint32_t period_ms;
    uint32_t last_run_ms;
    uint32_t run_count;   /**< 累计执行次数（诊断用） */
    bool     fired;       /**< 本次 tick 是否到期 */
} node1_task_slot_t;

typedef struct {
    node1_task_slot_t sensor;
    node1_task_slot_t heartbeat;
    node1_task_slot_t telemetry;
} node1_sched_t;

/** 初始化：全部任务的 last_run 置为 now（即第一次 tick 恰好在 now+period 到期）。
 *  @note 若置 0，则上电后第一个周期会立即触发一批任务，
 *        对 ADC 与总线都是不必要的冲击。 */
void node1_sched_init(node1_sched_t *s, uint32_t now_ms);

/**
 * 判断各任务是否到期，并更新计数。**只判定，不执行** ——
 * 调用方按 fired 标志决定是否调对应任务函数。
 *
 * 判据：`elapsed = now - last_run`（**无符号减法天然处理回绕**，
 * uint32_t 溢出 49.7 天也不会出错；写成 `now >= last + period` 在
 * 回绕时会永久失效），`elapsed >= period` 即到期。
 */
void node1_sched_poll(node1_sched_t *s, uint32_t now_ms);

/** 驱动一步：poll + 执行到期任务 + 处理收到的 CAN 帧 + 喂狗。
 *  @param app 应用上下文
 *  @param sched 调度状态
 *  @param now_ms 当前时间
 *  @param feed_watchdog 喂狗回调；为 NULL 时跳过
 *
 *  单次调用应保持**短**（<1ms）：CAN 收发与传感器采样都在这里，
 *  一旦被某个业务逻辑阻塞，总线就会开始丢帧。
 */
void node1_sched_step(node1_app_t *app, node1_sched_t *sched, uint32_t now_ms,
                      void (*feed_watchdog)(void));

#ifdef __cplusplus
}
#endif

#endif /* NODE1_SCHED_H */
