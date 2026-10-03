/**
 * @file  test_node_sched.c
 * @brief 调度器时序的 PC 单元测试。
 *
 * 为什么必须测调度器：任务周期写错（比如把 100ms 写成 1000ms）时，
 * 固件**完全正常运行**，只是行为不对 —— 主站看到的数据看不出异常，
 * 而过温报警会晚 10 倍触发。这类缺陷只能靠断言周期计数来抓。
 *
 * 特别覆盖了两条在真机上极难复现的路径：
 *   1. uint32_t 时基**回绕**（49.7 天）—— 写成加法比较会永久失效
 *   2. **长间隔调用**（例如调试时在断点里停了 5 秒）—— 周期任务不该
 *      "补跑"5 次，而应只跑 1 次（补跑会向总线灌入大量陈旧遥测）
 */
#define UTEST_MAIN
#define UTEST_SUITE_NAME "node_sched"

#include "utest.h"

#include <string.h>

#include "node1_app.h"
#include "node1_config.h"
#include "node1_sched.h"
#include "proto_codec.h"

/* ========================================================================== */
/* Mock HAL                                                                    */
/* ========================================================================== */

#define MOCK_CAN_CAP 8

static struct {
    proto_can_frame_t can_tx[MOCK_CAN_CAP];
    int can_tx_count;
    int watchdog_count;
    int sensor_read_count;
    uint16_t therm_raw;
} g;

static uint32_t mock_millis(void) { return 0u; }
static void mock_delay_ms(uint32_t ms) { (void)ms; }

static bool mock_sensor_read(node1_sensor_raw_t *out)
{
    g.sensor_read_count++;
    out->therm_raw = g.therm_raw;
    out->therm_do = false;
    return true;
}
static void mock_motor_set(node1_motor_id_t id, node1_motor_action_t a,
                           uint8_t d) { (void)id; (void)a; (void)d; }
static void mock_motor_enable(bool on) { (void)on; }
static void mock_buzzer_set(bool on) { (void)on; }

static bool mock_can_send(const proto_can_frame_t *f)
{
    if (g.can_tx_count < MOCK_CAN_CAP) {
        g.can_tx[g.can_tx_count] = *f;
        g.can_tx_count++;
    }
    return true;
}
static bool mock_can_recv(proto_can_frame_t *out) { (void)out; return false; }

static const node1_hal_t g_hal = {
    mock_millis, mock_delay_ms, mock_sensor_read, mock_motor_set,
    mock_motor_enable, mock_buzzer_set, mock_can_send, mock_can_recv
};

static void feed_watchdog(void) { g.watchdog_count++; }

/** 初始化 mock + app + sched，返回 sched 供断言。 */
static node1_sched_t setup(node1_app_t *app)
{
    memset(&g, 0, sizeof(g));
    g.therm_raw = node1_temp_to_raw(250);
    node1_app_init(app, &g_hal, 0u);
    node1_sched_t s;
    node1_sched_init(&s, 0u);
    return s;
}

/** 从 t=0 跑到 t_end（含），每 1ms 一步，统计各任务执行次数。 */
static void run_until(node1_app_t *app, node1_sched_t *s, uint32_t t_end,
                      uint32_t step_ms)
{
    for (uint32_t t = 0u; t <= t_end; t += step_ms) {
        node1_sched_step(app, s, t, feed_watchdog);
    }
}

/** 从 t=from 到 t=to（含），每 step_ms 一步。 */
static void run_range(node1_app_t *app, node1_sched_t *s, uint32_t from,
                      uint32_t to, uint32_t step_ms)
{
    for (uint32_t t = from; t <= to; t += step_ms) {
        node1_sched_step(app, s, t, feed_watchdog);
    }
}

static uint32_t count_tx(uint16_t id)
{
    uint32_t n = 0u;
    for (int i = 0; i < g.can_tx_count; ++i) {
        if (g.can_tx[i].id == id) {
            ++n;
        }
    }
    return n;
}

/* ========================================================================== */
/* 用例                                                                        */
/* ========================================================================== */

UTEST_CASE(sensor_runs_at_100ms)
{
    static node1_app_t app;
    node1_sched_t s = setup(&app);

    run_until(&app, &s, 1000u, 1u);

    /* t=100,200,...,1000 → 恰好 10 次。t=0 不应执行（见 node1_sched_init） */
    UTEST_EQ_INT(s.sensor.run_count, 10u);
    UTEST_EQ_INT(g.sensor_read_count, 10);
}

UTEST_CASE(telemetry_and_heartbeat_run_at_1hz)
{
    static node1_app_t app;
    node1_sched_t s = setup(&app);

    run_until(&app, &s, 3000u, 1u);

    UTEST_EQ_INT(s.telemetry.run_count, 3u);
    UTEST_EQ_INT(s.heartbeat.run_count, 3u);
    /* 遥测帧应真的发出去了（1Hz × 3s） */
    UTEST_EQ_INT(count_tx(PROTO_ID_TELEMETRY_NODE1), 3u);
}

UTEST_CASE(no_task_fires_immediately_after_init)
{
    static node1_app_t app;
    node1_sched_t s = setup(&app);

    /* 只跑到 t=50ms（远小于任何周期）→ 一个周期任务都不该触发。
     * 若last_run 初始化为 0，上电瞬间会同时触发三个任务，
     * 对 ADC 与总线都是不必要的冲击。 */
    run_until(&app, &s, 50u, 1u);

    UTEST_EQ_INT(s.sensor.run_count, 0u);
    UTEST_EQ_INT(s.heartbeat.run_count, 0u);
    UTEST_EQ_INT(s.telemetry.run_count, 0u);
    UTEST_EQ_INT(g.sensor_read_count, 0);
}

UTEST_CASE(long_gap_does_not_backfill_missed_runs)
{
    static node1_app_t app;
    node1_sched_t s = setup(&app);

    node1_sched_step(&app, &s, 0u, feed_watchdog);
    /* 模拟调试断点停了 5 秒：此时 100ms 周期"错过了" 50 次 */
    node1_sched_step(&app, &s, 5000u, feed_watchdog);

    /* 只应执行 1 次，而不是补跑 50 次。
     * 补跑会向总线灌入 50 帧陈旧遥测，挤占真正的新数据。 */
    UTEST_EQ_INT(s.sensor.run_count, 1u);
    UTEST_EQ_INT(s.telemetry.run_count, 1u);
    UTEST_EQ_INT(g.sensor_read_count, 1);
}

UTEST_CASE(watchdog_is_fed_every_step)
{
    static node1_app_t app;
    node1_sched_t s = setup(&app);

    run_until(&app, &s, 100u, 10u); /* 11 步 */

    /* IWDG 2s 超时，100ms 一次足够。但漏喂是"跑一段时间后莫名重启"
     * 的常见原因，必须有断言盯着。 */
    UTEST_EQ_INT(g.watchdog_count, 11);
}

UTEST_CASE(sched_survives_null_arguments)
{
    /* 防御性：调度器在启动早期或异常路径可能被喂 NULL。
     * 崩在这里等于整个固件挂死，比不做事严重得多。 */
    node1_sched_step(NULL, NULL, 0u, feed_watchdog);
    node1_sched_init(NULL, 0u);
    node1_sched_poll(NULL, 0u);
    UTEST_CHECK(1); /* 没崩即通过 */
}

UTEST_CASE(timer_wraparound_still_fires)
{
    static node1_app_t app;
    node1_sched_t s = setup(&app);

    /* 逼近 uint32_t 回绕：2^32 ≈ 42.9e9 ms ≈ 49.7 天 */
    const uint32_t near_max = 0xFFFFFF00u;
    node1_sched_step(&app, &s, near_max, feed_watchdog);
    const uint32_t before = s.telemetry.run_count;

    /* 跨过回绕点：near_max 之后需要再过 1 个遥测周期（1000ms）才该触发。
     * near_max = 0xFFFFFF00，加 1000 后回绕到 0x000002E8（744）。
     * ⚠️ 这里必须算准 —— 早先误用 0x100（256），那距上次才 512ms，
     * 本来就不该触发，是**测试数据算错**而非代码有问题。 */
    node1_sched_step(&app, &s, 0x000002E8u, feed_watchdog);

    /* 写成 `now >= last + period` 的实现在这里会永久失效
     *（last + period 溢出成小数，被误判为"还没到"）——
     * 现象是设备跑满 49.7 天后周期任务集体停摆。 */
    UTEST_EQ_INT(s.telemetry.run_count, before + 1u);
}

UTEST_CASE(sensor_task_catches_overtemp_in_expected_time)
{
    static node1_app_t app;
    node1_sched_t s = setup(&app);

    /* 正常温度跑 2 秒，确认不误报。
     * ⚠️ 早先写成两次 run_until(0→2000) 与 run_until(0→3500)，
     * 时间被"倒回"又重跑一遍，等价于 2000~3500 段跑了两遍，
     * 于是事件数翻倍。**时间必须单向推进** —— 这是写时序测试的通用陷阱。 */
    run_range(&app, &s, 0u, 2000u, 10u);
    UTEST_EQ_INT(count_tx(PROTO_ID_NODE1_EVENT), 0u);

    /* 突然升到 80℃ */
    g.therm_raw = node1_temp_to_raw(800);
    /* 传感器每 100ms 采一次，最多延迟 100ms 就该发现；
     * 再加心跳 1s 内上报，总计不超过 1.2s。 */
    run_range(&app, &s, 2010u, 3500u, 10u);

    /* 持续过温窗口 2010~3500ms 恰好只发 **1** 帧，推导如下：
     *   - 2100ms 传感器任务发现 80℃（>60℃ 报警阈值），置 overtemp_active
     *   - 3000ms 心跳任务首次检查到该状态 → 抑制闸门首次放行 → 发 1 帧
     *   - 下一次心跳在 4000ms，超出本用例范围
     * 所以是 1 帧。这同时说明一件事：**心跳周期（1s）远大于风暴
     * 抑制窗口（200ms）时，抑制窗口对"持续型事件"几乎不起作用**——
     * 真正的限流来自"只在状态跳变时置位"，而非窗口本身。
     * 该结论已记入 docs 的待协调项（事件上报策略需按事件类型区分）。 */
    UTEST_EQ_INT(count_tx(PROTO_ID_NODE1_EVENT), 1u);
}

UTEST_CASE(soft_reset_request_is_deferred_until_after_ack)
{
    static node1_app_t app;
    node1_sched_t s = setup(&app);

    /* 构造 RESET 命令，通过 mock 的 can_recv 注入不可行（它总是返回 false），
     * 因此直接调业务层置标志，再验证调度器会消费它。 */
    proto_command_msg_t m;
    memset(&m, 0, sizeof(m));
    m.seq = 1u;
    m.cmd = PROTO_CMD_RESET;
    m.device = PROTO_DEV_SYSTEM;
    m.param = 0;
    proto_can_frame_t f;
    UTEST_EQ_INT(proto_encode_command(NODE1_NODE_ID, &m, &f), PROTO_OK);

    UTEST_CHECK(node1_on_can_frame(&app, &f, 10u));
    UTEST_CHECK(app.pending_soft_reset);
    UTEST_EQ_INT(g.can_tx_count, 1); /* ACK 已发出 */

    /* 调度器消费标志 */
    node1_sched_step(&app, &s, 20u, feed_watchdog);
    UTEST_CHECK(!app.pending_soft_reset);
    /* 单测环境下 perform_reset 是空实现（弱符号），不会真的复位 */
    UTEST_EQ_INT(g.can_tx_count, 1);
}
