/**
 * @file    node1_main.c
 * @brief 节点一固件入口（裸机 super-loop 版）。
 *
 * 为什么先做裸机版而不是直接上 FreeRTOS：
 *   1. **能立刻烧板验证**。裸机版不依赖 RTOS 移植与 FreeRTOSConfig，
 *      拿到板子当天就能看到 CAN 收发与 PWM 波形，闭环最短。
 *   2. **把"业务对不对"与"调度够不够好"分开验证**。若一上来就上 RTOS，
 *      出现异常时难以区分是业务逻辑、任务划分还是栈配置的问题。
 *   3. 调度逻辑（node1_sched.c）本身**与 RTOS 无关且已单测覆盖**，
 *      换成 FreeRTOS 只是把"谁在什么时刻调它"换个方式，时序结论不变。
 *
 * 阶段 2 迁到 FreeRTOS 时，node1_sched_step() 的调用点从"主循环"变成
 * "各任务体"，其余代码不动 —— 这就是当初把它单独抽出来的收益。
 *
 * 接线提醒（详见 node1_bsp.c 头注释）：
 *   热敏 AO→PA0 / DO→PA1；TB6612 PWMA=PA8 PWMB=PA9 IN1/2=PB0,PB1,PB10,PB11
 *   STBY=PB12；蜂鸣器 PB13（低电平有效）；CAN1 TX=PA12 RX=PA11
 *   **PA13/PA14 是 SWD，绝不可占用** —— 它们是唯一的烧写与调试通道。
 */
#include "node1_app.h"
#include "node1_config.h"
#include "node1_sched.h"
#include "node1_bsp.h"

/** 应用与调度上下文：常驻 .bss，避免占用栈（20KB SRAM 很紧张）。 */
static node1_app_t g_app;
static node1_sched_t g_sched;

/** 用强符号覆盖 node1_sched.c 里的弱符号空实现。 */
void node1_sched_perform_reset(void)
{
    node1_bsp_soft_reset(); /* 不返回 */
}

int main(void)
{
    const node1_hal_t *hal = NULL;

    /* --- 1. 硬件初始化 ------------------------------------------- */
    node1_bsp_init(&hal);

    /* --- 2. 应用与调度初始化-------------------------------------- *
     * ⚠️ 两者都传入 HAL 的毫秒时钟作为"现在"。上电即把 last_run 置为当前，
     * 避免第一周期立刻触发一批任务冲击 ADC 与总线。 */
    node1_app_init(&g_app, hal, hal->millis());
    node1_sched_init(&g_sched, hal->millis());

    /* --- 3. 主循环 ----------------------------------------------- *
     * 单次循环必须**短**。每轮只做两件事：喂狗 + 推进调度。
     *
     * 固定 1ms 延时是有意的取舍，不是偷懒：
     *   - 500kbps 下最坏帧长约 250μs，1ms 的轮询粒度足够；
     *   - 节点侧只有 1kHz 遥测与 1Hz 心跳，1ms 轮询绰绰有余；
     *   - 真正的命令处理靠 CAN RX 中断入队，**不靠轮询**，故不会丢命令。
     * 若将来加入需要 ms 级响应的业务（如闭环控制），应改用 FreeRTOS
     * 的高精度定时器，而不是缩短这里的延时 —— 那只会白烧 CPU。 */
    for (;;) {
        hal->delay_ms(1u);
        node1_sched_step(&g_app, &g_sched, hal->millis(),
                         node1_bsp_feed_watchdog);
    }
}
