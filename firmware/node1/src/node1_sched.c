/**
 * @file    node1_sched.c
 * @brief 节点一调度逻辑实现。
 *
 * 顺序即优先级：**先收 CAN（保护总线时序）→ 再到期任务 → 最后喂狗**。
 * 把 CAN 放在最前，是因为 CAN 帧有硬时限（主站 3s 无帧即判掉线），
 * 而传感器 100ms 的抖动完全可接受。顺序反过来会平白增加丢帧率。
 */
#include "node1_sched.h"

#include <string.h>

#include "node1_config.h"

/**
 * 由 port 层提供的软复位钩子。
 * 声明必须在**文件作用域**（写在函数体内属于文件范围扩展，
 * MSVC 会报 C4210，且并非标准 C）。
 */
extern void node1_sched_perform_reset(void);

static void slot_init(node1_task_slot_t *slot, uint32_t period_ms,
                      uint32_t now_ms)
{
    slot->period_ms = period_ms;
    slot->last_run_ms = now_ms;
    slot->run_count = 0u;
    slot->fired = false;
}

/** @return true = 本次到期 */
static bool slot_poll(node1_task_slot_t *slot, uint32_t now_ms)
{
    /* ⚠️ 必须用无符号减法而非 now >= last + period：
     * uint32_t 在 49.7 天后回绕，加法比较会永久失效（永远判定"没到期"，
     * 表现为"设备跑着跑着就不动了"）。这是嵌入式里的经典长跑 bug。 */
    const uint32_t elapsed = now_ms - slot->last_run_ms;
    if (elapsed >= slot->period_ms) {
        slot->last_run_ms = now_ms;
        slot->run_count++;
        slot->fired = true;
        return true;
    }
    slot->fired = false;
    return false;
}

void node1_sched_init(node1_sched_t *s, uint32_t now_ms)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    slot_init(&s->sensor, NODE1_SENSOR_PERIOD_MS, now_ms);
    slot_init(&s->heartbeat, NODE1_HEARTBEAT_PERIOD_MS, now_ms);
    slot_init(&s->telemetry, NODE1_TELEMETRY_PERIOD_MS, now_ms);
}

void node1_sched_poll(node1_sched_t *s, uint32_t now_ms)
{
    if (s == NULL) {
        return;
    }
    (void)slot_poll(&s->sensor, now_ms);
    (void)slot_poll(&s->heartbeat, now_ms);
    (void)slot_poll(&s->telemetry, now_ms);
}

void node1_sched_step(node1_app_t *app, node1_sched_t *sched, uint32_t now_ms,
                      void (*feed_watchdog)(void))
{
    if (app == NULL || sched == NULL) {
        return;
    }

    /* --- 1. CAN 优先：把队列里的帧全部处理完 ----------------------- */
    /* 不设上限：队列深度只有 16（node1_bsp.c），
     * 且主站可能连发命令，一次处理完最省事。
     * 万一有帧洪水，这里的循环会持续占用 —— 但那本来就是异常场景，
     * 业务层会因幂等拦截而不执行动作，代价可接受。 */
    if (NODE1_HAL_OK(app->hal, can_recv)) {
        proto_can_frame_t f;
        while (app->hal->can_recv(&f)) {
            (void)node1_on_can_frame(app, &f, now_ms);
            if (feed_watchdog != NULL) {
                feed_watchdog(); /* 长队列时防看门狗复位 */
            }
        }
    }

    /* --- 2. 到期任务 --------------------------------------------- */
    node1_sched_poll(sched, now_ms);
    if (sched->sensor.fired) {
        node1_task_sensor(app, now_ms);
    }
    if (sched->heartbeat.fired) {
        node1_task_heartbeat(app, now_ms);
    }
    if (sched->telemetry.fired) {
        node1_task_telemetry(app, now_ms);
    }

    /* --- 3. 软复位请求 ------------------------------------------- */
    /* 放在 ACK 之后由调度器执行，保证主站先收到"命令被接受"的 ACK，
     * 再等节点重启。顺序反了的话主站会因等不到 ACK 而重传，
     * 而节点已经重启，无从判断该不该重传。 */
    if (app->pending_soft_reset) {
        app->pending_soft_reset = false;
        if (feed_watchdog != NULL) {
            feed_watchdog();
        }
        /* 实际复位动作由 port 层提供（裸机=直接复位；单测=空实现） */
        node1_sched_perform_reset();
    }

    /* --- 4. 喂狗 ------------------------------------------------- */
    if (feed_watchdog != NULL) {
        feed_watchdog();
    }
}

/* 弱符号默认实现：无 RTOS 环境（PC 单测）下什么都不做。
 * 真实 port 可用强符号覆盖为 HAL_Delay + NVIC_SystemReset。 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void node1_sched_perform_reset(void)
{
    /* 单测环境：故意留空。若在此处真复位，PC 测试会直接终止。 */
}
