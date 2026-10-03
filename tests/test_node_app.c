/**
 * @file  test_node_app.c
 * @brief 节点一业务逻辑的 PC 单元测试（注入 mock HAL，不碰任何硬件）。
 *
 * 为什么必须测这一层：本项目的执行器/传感器行为"看起来对"极易出错——
 *   - 重复命令是否真的**没有**再次操作电机（幂等铁律①的落点）
 *   - 参数越界是否**拒绝**而不是截断（protocol.md §4.4 的设计原则）
 *   - 主站掉线是否真进安全态（总线断了电机还在转 = 执行器失控）
 *   - 温标换算是否单调可逆（0.1℃ int16 有符号处理）
 * 这些没有硬件就看不出问题，而本项目当前**还没有硬件台架**，
 * 因此这一层测试是"能不能在没有板子的情况下说逻辑对"的唯一凭据。
 *
 * mock HAL 的手法：vtable 里全是函数指针，替换成记录型实现即可断言
 * "电机被设置成什么""CAN 发出去了哪几帧"。
 */
#define UTEST_MAIN
#define UTEST_SUITE_NAME "node_app"

#include "utest.h"

#include <string.h>

#include "node1_app.h"
#include "node1_config.h"
#include "proto_codec.h"
#include "proto_node.h"

/* ========================================================================== */
/* Mock HAL                                                                    */
/* ========================================================================== */

#define MOCK_CAN_CAP   16
#define MOCK_MOTOR_CAP 8

typedef struct {
    /* 记录 motor_set 调用序列 */
    struct {
        node1_motor_action_t action;
        uint8_t duty;
    } motor_log[MOCK_MOTOR_CAP];
    int motor_log_count;
    int motor_enable_count;
    bool last_motor_enabled;

    int buzzer_count;
    bool last_buzzer;

    /* 记录发出的 CAN 帧 */
    proto_can_frame_t can_tx[MOCK_CAN_CAP];
    int can_tx_count;
    bool can_send_ok;      /**< 模拟邮箱满：false 时 can_send 全部失败 */

    /* 传感器输入 */
    node1_sensor_raw_t sensor_in;
    bool sensor_read_ok;
    int sensor_read_count;
} mock_t;

static mock_t g_mock;

static uint32_t mock_millis(void) { return 0u; }
static void mock_delay_ms(uint32_t ms) { (void)ms; }

static bool mock_sensor_read(node1_sensor_raw_t *out)
{
    g_mock.sensor_read_count++;
    if (!g_mock.sensor_read_ok) {
        return false;
    }
    *out = g_mock.sensor_in;
    return true;
}

static void mock_motor_set(node1_motor_id_t id, node1_motor_action_t action,
                           uint8_t duty_pct)
{
    (void)id;
    if (g_mock.motor_log_count < MOCK_MOTOR_CAP) {
        g_mock.motor_log[g_mock.motor_log_count].action = action;
        g_mock.motor_log[g_mock.motor_log_count].duty = duty_pct;
        g_mock.motor_log_count++;
    }
}

static void mock_motor_enable(bool on)
{
    g_mock.motor_enable_count++;
    g_mock.last_motor_enabled = on;
}

static void mock_buzzer_set(bool on)
{
    g_mock.buzzer_count++;
    g_mock.last_buzzer = on;
}

static bool mock_can_send(const proto_can_frame_t *f)
{
    if (!g_mock.can_send_ok) {
        return false;
    }
    if (g_mock.can_tx_count < MOCK_CAN_CAP) {
        g_mock.can_tx[g_mock.can_tx_count] = *f;
        g_mock.can_tx_count++;
    }
    return true;
}

static bool mock_can_recv(proto_can_frame_t *out)
{
    (void)out;
    return false; /* 测试中由用例直接调 node1_on_can_frame() 注入 */
}

static const node1_hal_t g_mock_hal = {
    mock_millis, mock_delay_ms, mock_sensor_read, mock_motor_set,
    mock_motor_enable, mock_buzzer_set, mock_can_send, mock_can_recv
};

/** 复位 mock 并初始化一个已连 mock HAL 的 app。 */
static node1_app_t *fresh_app(uint32_t now_ms)
{
    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.can_send_ok = true;
    g_mock.sensor_read_ok = true;
    g_mock.sensor_in.therm_raw = node1_temp_to_raw(250); /* 25.0℃ */
    g_mock.sensor_in.therm_do = false;

    static node1_app_t app;
    node1_app_init(&app, &g_mock_hal, now_ms);
    return &app;
}

/* ========================================================================== */
/* 用例                                                                        */
/* ========================================================================== */

UTEST_CASE(temp_scale_is_monotonic_and_reversible)
{
    /* 温度升高 → NTC 阻值下降 → 分压输出电压上升 → ADC 原始值上升。
     * 这个方向搞反是最常见的接线错误，必须钉死。 */
    const int16_t cold = node1_temp_from_raw(node1_temp_to_raw(0));   /*  0.0℃ */
    const int16_t warm = node1_temp_from_raw(node1_temp_to_raw(500));  /* 50.0℃ */
    const int16_t hot = node1_temp_from_raw(node1_temp_to_raw(900));   /* 90.0℃ */

    UTEST_CHECK(cold != NODE1_ADC_FAULT_TEMP_C10);
    UTEST_CHECK(warm != NODE1_ADC_FAULT_TEMP_C10);
    UTEST_CHECK(hot != NODE1_ADC_FAULT_TEMP_C10);
    UTEST_CHECK(cold < warm);
    UTEST_CHECK(warm < hot);

    /* 可逆性：四舍五入修好后应能**精确**往返。
     * ⚠️ 单位是 0.1℃（协议 §4.3），不是℃！这里曾写成 node1_temp_to_raw(50)
     * 想表达 50℃ —— 实际只表示 5.0℃，断言"结果在 495~505"就永远失败。
     * 单位错误 + 数值巧合（50℃ 恰好也接近某个 raw）是这类 bug 难发现的原因，
     * 故下面用显式注释锁死单位。 */
    UTEST_EQ_INT(warm, 500); /* 50.0℃ */
    UTEST_EQ_INT(hot, 900);  /* 90.0℃ */
}

UTEST_CASE(temp_negative_is_signed)
{
    /* -10℃ 必须能正确表示（int16 补码），这是协议明确要求的。
     * 曾经用 uint16 存温度的写法会让负温变成 65000+，是真实踩过的坑。 */
    const uint16_t raw = node1_temp_to_raw(-100);
    const int16_t t = node1_temp_from_raw(raw);
    UTEST_CHECK(t != NODE1_ADC_FAULT_TEMP_C10);
    UTEST_CHECK(t > -150 && t < -50); /* -10.0℃ ± 5.0℃ */
}

UTEST_CASE(temp_out_of_range_is_fault)
{
    /* ADC 读数越界 → 判传感器故障，而不是返回一个假温度。
     * 返回假温度会让主站"看起来一切正常"，这是最坏的一类故障。 */
    UTEST_EQ_INT(node1_temp_from_raw(0u), NODE1_ADC_FAULT_TEMP_C10);
    UTEST_EQ_INT(node1_temp_from_raw(4095u), NODE1_ADC_FAULT_TEMP_C10);
}

/** 构造一条命令帧（用协议库编码，保证与真实主站发的一致）。 */
static proto_can_frame_t make_cmd(uint8_t node, uint8_t seq, proto_cmd_t cmd,
                                  proto_device_t dev, uint8_t channel,
                                  int16_t param)
{
    proto_command_msg_t m;
    memset(&m, 0, sizeof(m));
    m.seq = seq;
    m.cmd = (uint8_t)cmd;
    m.device = (uint8_t)dev;
    m.channel = channel;
    m.param = param;
    proto_can_frame_t f;
    memset(&f, 0, sizeof(f));
    (void)proto_encode_command(node, &m, &f);
    return f;
}

/**
 * 构造一条**协议库无法编码**的命令帧（参数越界 / 非法枚举）。
 *
 * 为什么必须手搓字节：proto_encode_command 自带范围校验，越界参数根本
 * 编码不出来。而真实场景恰恰是"主站发了错的东西"—— 可能是主站 bug、
 * 可能是协议版本不一致、也可能是恶意构造。要测节点的防御能力，
 * 就必须绕开编码器直接摆好字节。
 */
static proto_can_frame_t make_raw_cmd(uint8_t node, uint8_t seq, uint8_t cmd,
                                      uint8_t dev, uint8_t channel,
                                      int16_t param)
{
    proto_can_frame_t f;
    memset(&f, 0, sizeof(f));
    f.id = proto_make_id(PROTO_CAT_COMMAND, node, PROTO_TYPE_NORMAL);
    f.dlc = PROTO_DLC;
    f.data[0] = seq;
    f.data[1] = cmd;
    f.data[2] = dev;
    f.data[3] = channel;
    f.data[4] = (uint8_t)((uint16_t)param & 0xFFu);        /* 小端 */
    f.data[5] = (uint8_t)(((uint16_t)param >> 8) & 0xFFu);
    f.data[6] = 0u;
    f.data[7] = 0u;
    return f;
}

/** 找到 mock 记录中最后一条指定 ID 的帧。 */
static bool find_last_tx(uint16_t id, proto_can_frame_t *out)
{
    for (int i = g_mock.can_tx_count - 1; i >= 0; --i) {
        if (g_mock.can_tx[i].id == id) {
            *out = g_mock.can_tx[i];
            return true;
        }
    }
    return false;
}

UTEST_CASE(cmd_set_motor_executes_and_acks)
{
    node1_app_t *app = fresh_app(0u);
    const int before = g_mock.motor_log_count;

    /* 主站 → 节点一：设电机占空比 50.0% (param=500) */
    const proto_can_frame_t f =
        make_cmd(NODE1_NODE_ID, 1u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 500);
    const bool executed = node1_on_can_frame(app, &f, 100u);

    UTEST_CHECK(executed);
    UTEST_CHECK(g_mock.motor_log_count == before + 1);
    UTEST_EQ_INT(g_mock.motor_log[before].action, NODE1_MOTOR_ACTION_FORWARD);
    UTEST_EQ_INT(g_mock.motor_log[before].duty, 50);
    UTEST_EQ_INT(app->motor_duty, 50);

    /* 必须回 ACK，且 echo_seq 等于命令 seq —— 否则主站会重传到超时 */
    proto_can_frame_t ack;
    UTEST_CHECK(find_last_tx(PROTO_ID_ACK_NODE1, &ack));
    UTEST_EQ_INT(ack.data[0], 1);            /* echo_seq */
    UTEST_EQ_INT(ack.data[1], PROTO_OK);     /* result */
    UTEST_EQ_INT(app->stat_cmd_exec, 1u);
}

UTEST_CASE(cmd_duplicate_does_not_re_touch_motor)
{
    node1_app_t *app = fresh_app(0u);
    const proto_can_frame_t f =
        make_cmd(NODE1_NODE_ID, 7u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 700);

    UTEST_CHECK(node1_on_can_frame(app, &f, 100u));
    const int after_first = g_mock.motor_log_count;

    /* 主站超时重传**同一 seq**（50ms 后） */
    const bool executed_again = node1_on_can_frame(app, &f, 150u);

    UTEST_CHECK(!executed_again);                  /* 绝不重复执行 */
    UTEST_EQ_INT(g_mock.motor_log_count, after_first); /* 电机没被再碰 */
    UTEST_EQ_INT(app->stat_cmd_dup, 1u);

    /* 但**仍须回 ACK**，否则主站会一直重传到判失败 */
    proto_can_frame_t ack;
    UTEST_CHECK(find_last_tx(PROTO_ID_ACK_NODE1, &ack));
    UTEST_EQ_INT(ack.data[0], 7);
    UTEST_EQ_INT(ack.data[1], PROTO_OK);
}

UTEST_CASE(cmd_out_of_range_is_rejected_not_truncated)
{
    node1_app_t *app = fresh_app(0u);
    const int before = g_mock.motor_log_count;

    /* param = 5000 超界（电机合法范围 0~1000）。**必须手搓字节**——
     * 协议库的编码器会先拒掉，根本编不出越界帧，而真实场景恰恰是
     * 主站发了错东西（bug / 版本不一致 / 恶意构造）。 */
    const proto_can_frame_t f =
        make_raw_cmd(NODE1_NODE_ID, 3u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 5000);

    const bool executed = node1_on_can_frame(app, &f, 100u);

    UTEST_CHECK(!executed);
    UTEST_EQ_INT(g_mock.motor_log_count, before); /* 电机完全没动 */
    UTEST_EQ_INT(app->stat_cmd_reject, 1u);

    /* ACK 必须带错误码，主站据此知道是"参数错"而非"没收到" */
    proto_can_frame_t ack;
    UTEST_CHECK(find_last_tx(PROTO_ID_ACK_NODE1, &ack));
    UTEST_EQ_INT(ack.data[1], PROTO_ERR_RANGE);

    /* 关键：被拒的 seq **不应**被记为已执行 —— 协议要求 REJECT 不占用 seq。
     * 否则主站修正参数重发同一 seq 时会被误判为重复而丢弃。 */
    const proto_can_frame_t retry =
        make_cmd(NODE1_NODE_ID, 3u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 400);
    UTEST_CHECK(node1_on_can_frame(app, &retry, 200u));
    UTEST_EQ_INT(app->motor_duty, 40);
}

UTEST_CASE(cmd_broadcast_and_unicast_seq_are_independent)
{
    node1_app_t *app = fresh_app(0u);

    /* 广播通道 seq=5，然后单播通道 seq=5 —— 铁律①：
     * 两者必须各自计数，若共用则单播会被误判重复而静默丢弃。 */
    const proto_can_frame_t bc =
        make_cmd(0u, 5u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 300);
    const proto_can_frame_t uc =
        make_cmd(NODE1_NODE_ID, 5u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 800);

    UTEST_CHECK(node1_on_can_frame(app, &bc, 100u));
    UTEST_EQ_INT(app->motor_duty, 30);

    /* 关键断言：同一 seq 值走另一通道，仍须执行 */
    UTEST_CHECK(node1_on_can_frame(app, &uc, 110u));
    UTEST_EQ_INT(app->motor_duty, 80);
    UTEST_EQ_INT(app->stat_cmd_dup, 0u);
}

UTEST_CASE(first_command_after_boot_executes_regardless_of_seq_zero)
{
    node1_app_t *app = fresh_app(0u);

    /* 铁律②：上电后 seq_valid=false，第一个命令无条件执行。
     * 这里是"主站恰好发 seq=0"的真实场景 —— 若未置标志会被误判重复。 */
    const proto_can_frame_t f =
        make_cmd(NODE1_NODE_ID, 0u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 600);

    UTEST_CHECK(node1_on_can_frame(app, &f, 100u));
    UTEST_EQ_INT(app->motor_duty, 60);
    UTEST_EQ_INT(app->stat_cmd_dup, 0u);
}

UTEST_CASE(soft_reset_requires_first_command_to_run_again)
{
    node1_app_t *app = fresh_app(0u);
    const proto_can_frame_t f =
        make_cmd(NODE1_NODE_ID, 9u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 500);
    UTEST_CHECK(node1_on_can_frame(app, &f, 100u));

    node1_app_soft_reset(app, 200u);

    /* 软复位后主站重发同一 seq，必须重新执行（不能被判重复丢弃） */
    UTEST_CHECK(node1_on_can_frame(app, &f, 300u));
    UTEST_EQ_INT(app->stat_cmd_dup, 0u);
    UTEST_EQ_INT(app->stat_cmd_exec, 2u);
}

UTEST_CASE(telemetry_reports_measured_temperature)
{
    node1_app_t *app = fresh_app(0u);

    g_mock.sensor_in.therm_raw = node1_temp_to_raw(375); /* 37.5℃ */
    g_mock.sensor_in.therm_do = true;
    node1_task_sensor(app, 0u);
    UTEST_CHECK(app->temperature_c10 >= 374 && app->temperature_c10 <= 376);
    UTEST_EQ_INT(app->do_state, 1);

    node1_task_telemetry(app, 1000u);

    proto_can_frame_t f;
    UTEST_CHECK(find_last_tx(PROTO_ID_TELEMETRY_NODE1, &f));

    proto_telemetry_node1_t t;
    UTEST_EQ_INT(proto_decode_telemetry_node1(&f, &t), PROTO_OK);
    /* 量化到 0.1℃ 后允许 ±1 个 LSB 的往返误差 */
    UTEST_CHECK(t.temperature >= 374 && t.temperature <= 376);
    UTEST_EQ_INT(t.do_state, 1);
    /* ⚠️ 无电流采样硬件：bit2 必须恒 0，不得谎报"电流有效" */
    UTEST_EQ_INT(t.valid & (1u << NODE1_VALID_CURRENT_BIT), 0);
    /* ⚠️ 无电流采样硬件：堵转位必须恒 0 */
    UTEST_EQ_INT(t.status & (1u << NODE1_STATUS_STALL_BIT), 0);
    UTEST_CHECK((t.valid & (1u << NODE1_VALID_TEMP_BIT)) != 0);
}

UTEST_CASE(telemetry_seq_increments_for_loss_detection)
{
    node1_app_t *app = fresh_app(0u);
    g_mock.sensor_in.therm_raw = node1_temp_to_raw(250);

    node1_task_sensor(app, 0u);
    node1_task_telemetry(app, 0u);
    node1_task_telemetry(app, 1000u);
    node1_task_telemetry(app, 2000u);

    proto_telemetry_node1_t t;
    proto_can_frame_t f;
    UTEST_CHECK(find_last_tx(PROTO_ID_TELEMETRY_NODE1, &f));
    UTEST_EQ_INT(proto_decode_telemetry_node1(&f, &t), PROTO_OK);
    /* 主站靠这个序号发现丢帧，所以必须严格递增（回绕由 uint8 自然处理） */
    UTEST_EQ_INT(t.seq, 2);
}

UTEST_CASE(overtemp_raises_event_with_hysteresis)
{
    node1_app_t *app = fresh_app(0u);

    /* 升到 65℃（超过 60℃ 报警阈值） */
    g_mock.sensor_in.therm_raw = node1_temp_to_raw(650);
    node1_task_sensor(app, 0u);
    UTEST_CHECK(app->overtemp_active);

    node1_task_heartbeat(app, 1000u);

    proto_can_frame_t f;
    UTEST_CHECK(find_last_tx(PROTO_ID_NODE1_EVENT, &f));
    proto_event_msg_t ev;
    UTEST_EQ_INT(proto_decode_event(&f, &ev), PROTO_OK);
    UTEST_EQ_INT(ev.event_code, PROTO_EV_OVER_TEMP);
    UTEST_EQ_INT(ev.level, PROTO_LVL_CRITICAL);
    UTEST_CHECK(ev.value >= 649 && ev.value <= 651);
    UTEST_EQ_INT(app->stat_event_sent, 1u);

    /* 滞回验证：降到 55℃（低于 60 但高于 50 恢复阈值）→ 仍应保持报警，
     * 否则会在阈值附近抖动产生事件风暴。 */
    g_mock.sensor_in.therm_raw = node1_temp_to_raw(550);
    node1_task_sensor(app, 1100u);
    UTEST_CHECK(app->overtemp_active);

    /* 降到 45℃ → 恢复 */
    g_mock.sensor_in.therm_raw = node1_temp_to_raw(450);
    node1_task_sensor(app, 1200u);
    UTEST_CHECK(!app->overtemp_active);
}

UTEST_CASE(event_storm_is_suppressed_within_window)
{
    node1_app_t *app = fresh_app(0u);
    g_mock.sensor_in.therm_raw = node1_temp_to_raw(700); /* 70℃ */

    node1_task_sensor(app, 0u);
    node1_task_heartbeat(app, 0u);
    UTEST_EQ_INT(app->stat_event_sent, 1u);

    /* 200ms 抑制窗口内（PROTO_EVENT_SUPPRESS_MS）不得再发 */
    node1_task_heartbeat(app, 50u);
    node1_task_heartbeat(app, 100u);
    node1_task_heartbeat(app, 199u);
    UTEST_EQ_INT(app->stat_event_sent, 1u);
    UTEST_CHECK(app->stat_event_suppressed >= 3);

    /* 窗口过后放行 —— 这是修过的真实缺陷：
     * 早期实现放行时未清零 repeat_count，导致计数跨窗口累加。 */
    node1_task_heartbeat(app, 250u);
    UTEST_EQ_INT(app->stat_event_sent, 2u);

    proto_can_frame_t f;
    UTEST_CHECK(find_last_tx(PROTO_ID_NODE1_EVENT, &f));
    proto_event_msg_t ev;
    UTEST_EQ_INT(proto_decode_event(&f, &ev), PROTO_OK);
    UTEST_EQ_INT(ev.repeat_count, 0); /* 本窗口内首次上报，计数归零 */
}

UTEST_CASE(master_offline_enters_safe_state)
{
    node1_app_t *app = fresh_app(0u);

    /* 先收到命令让电机转起来 */
    const proto_can_frame_t f =
        make_cmd(NODE1_NODE_ID, 1u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 900);
    UTEST_CHECK(node1_on_can_frame(app, &f, 100u));
    UTEST_EQ_INT(app->motor_duty, 90);
    UTEST_CHECK(!app->safe_state);

    /* 推进到主站掉线之后（PROTO_OFFLINE_TIMEOUT_MS = 3s），期间无任何帧 */
    g_mock.sensor_in.therm_raw = node1_temp_to_raw(250);
    node1_task_sensor(app, 4000u);
    node1_task_heartbeat(app, 4000u);

    UTEST_CHECK(app->safe_state);              /* 已进安全态 */
    UTEST_EQ_INT(app->motor_duty, 0);          /* 占空比归零 */
    UTEST_EQ_INT(app->motor_action, NODE1_MOTOR_ACTION_STOP);
    UTEST_CHECK(!g_mock.last_motor_enabled);   /* STBY 拉低 → 驱动高阻 */
    UTEST_CHECK(!g_mock.last_buzzer);          /* 蜂鸣器静默 */
}

UTEST_CASE(master_recovery_leaves_safe_state)
{
    node1_app_t *app = fresh_app(0u);
    const proto_can_frame_t cmd =
        make_cmd(NODE1_NODE_ID, 1u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 900);
    (void)node1_on_can_frame(app, &cmd, 100u);

    node1_task_heartbeat(app, 4000u);
    UTEST_CHECK(app->safe_state);

    /* 主站恢复：发心跳帧 */
    proto_heartbeat_msg_t hb;
    memset(&hb, 0, sizeof(hb));
    hb.heartbeat_count = 1;
    hb.online_bitmap = 0x0002u;
    hb.protocol_version = PROTO_PROTOCOL_VERSION;
    proto_can_frame_t hbf;
    UTEST_EQ_INT(proto_encode_heartbeat(&hb, &hbf), PROTO_OK);

    (void)node1_on_can_frame(app, &hbf, 4100u);
    UTEST_CHECK(!app->safe_state);
    UTEST_CHECK(g_mock.last_motor_enabled);   /* 重新使能驱动 */
}

UTEST_CASE(foreign_unicast_command_is_ignored)
{
    node1_app_t *app = fresh_app(0u);
    const int before = g_mock.motor_log_count;

    /* 主站发给节点二的单播，节点一必须忽略且不动作 */
    const proto_can_frame_t f =
        make_cmd(2u, 1u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 500);
    const bool executed = node1_on_can_frame(app, &f, 100u);

    UTEST_CHECK(!executed);
    UTEST_EQ_INT(g_mock.motor_log_count, before);
    /* 也不该给它回 ACK（它不是我的命令，回了会污染主站的 ACK 匹配） */
    for (int i = 0; i < g_mock.can_tx_count; ++i) {
        UTEST_CHECK(g_mock.can_tx[i].id != PROTO_ID_ACK_NODE1);
    }
}

UTEST_CASE(sensor_read_failure_keeps_last_valid_temperature)
{
    node1_app_t *app = fresh_app(0u);

    g_mock.sensor_in.therm_raw = node1_temp_to_raw(300);
    node1_task_sensor(app, 0u);
    UTEST_CHECK(app->temperature_c10 >= 299 && app->temperature_c10 <= 301);

    /* 读取失败：温度 valid 位必须清掉，但**保留上次读数**。
     * 若清零温度，主站会看到"温度骤降到 0℃"而误报低温故障。 */
    g_mock.sensor_read_ok = false;
    node1_task_sensor(app, 100u);

    UTEST_CHECK(app->sensor_fault);
    UTEST_CHECK(app->temperature_c10 >= 299 && app->temperature_c10 <= 301); /* 上次有效值仍在 */
    UTEST_EQ_INT(app->valid_bits & (1u << NODE1_VALID_TEMP_BIT), 0);
    UTEST_CHECK((app->status_bits & (1u << NODE1_STATUS_SENSOR_FAULT_BIT)) != 0);
}

UTEST_CASE(can_mailbox_full_does_not_corrupt_state)
{
    node1_app_t *app = fresh_app(0u);

    /* 模拟邮箱满：can_send 全部返回 false。
     * 命令仍应被"执行"（状态机已推进），但不得崩溃、不得越界写 mock 数组。 */
    g_mock.can_send_ok = false;
    const proto_can_frame_t f =
        make_cmd(NODE1_NODE_ID, 1u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 500);

    const bool executed = node1_on_can_frame(app, &f, 100u);
    UTEST_CHECK(executed);
    UTEST_EQ_INT(app->motor_duty, 50); /* 动作已生效 */
    UTEST_EQ_INT(g_mock.can_tx_count, 0); /* 但一帧都没发出去 */
}

UTEST_CASE(buzzer_command_low_level_active_semantics)
{
    node1_app_t *app = fresh_app(0u);

    proto_can_frame_t on =
        make_cmd(NODE1_NODE_ID, 1u, PROTO_CMD_SET, PROTO_DEV_BUZZER, 0u, 500);
    (void)node1_on_can_frame(app, &on, 100u);
    UTEST_CHECK(g_mock.last_buzzer);

    proto_can_frame_t off =
        make_cmd(NODE1_NODE_ID, 2u, PROTO_CMD_STOP, PROTO_DEV_BUZZER, 0u, 0);
    (void)node1_on_can_frame(app, &off, 200u);
    UTEST_CHECK(!g_mock.last_buzzer);
}

UTEST_CASE(system_reset_command_sets_pending_flag)
{
    node1_app_t *app = fresh_app(0u);

    const proto_can_frame_t f =
        make_cmd(NODE1_NODE_ID, 1u, PROTO_CMD_RESET, PROTO_DEV_SYSTEM, 0u, 0);
    (void)node1_on_can_frame(app, &f, 100u);

    /* 复位必须由调度器执行（保证 ACK 先发出去），业务层只置标志 */
    UTEST_CHECK(app->pending_soft_reset);
}

UTEST_CASE(stop_command_zeroes_duty)
{
    node1_app_t *app = fresh_app(0u);

    proto_can_frame_t run =
        make_cmd(NODE1_NODE_ID, 1u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 750);
    (void)node1_on_can_frame(app, &run, 100u);
    UTEST_EQ_INT(app->motor_duty, 75);

    proto_can_frame_t stop =
        make_cmd(NODE1_NODE_ID, 2u, PROTO_CMD_STOP, PROTO_DEV_MOTOR, 0u, 0);
    (void)node1_on_can_frame(app, &stop, 200u);
    UTEST_EQ_INT(app->motor_duty, 0);
    UTEST_EQ_INT(app->motor_action, NODE1_MOTOR_ACTION_STOP);
}

UTEST_CASE(duty_rounding_does_not_truncate)
{
    node1_app_t *app = fresh_app(0u);

    /* param=499 → 49.9% → 应为 50（+50 四舍五入），若截断会变 49。
     * 主站显示 49.9% 而示波器量到 49%，排查时非常费时。 */
    const proto_can_frame_t f =
        make_cmd(NODE1_NODE_ID, 1u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 0u, 499);
    (void)node1_on_can_frame(app, &f, 100u);
    UTEST_EQ_INT(app->motor_duty, 50);
}

UTEST_CASE(motor_channel_out_of_range_is_rejected)
{
    node1_app_t *app = fresh_app(0u);
    const int before = g_mock.motor_log_count;

    /* channel=1：本节点只有单电机，通道号非法 */
    const proto_can_frame_t f =
        make_cmd(NODE1_NODE_ID, 1u, PROTO_CMD_SET, PROTO_DEV_MOTOR, 1u, 500);
    const bool executed = node1_on_can_frame(app, &f, 100u);

    UTEST_CHECK(!executed);
    UTEST_EQ_INT(g_mock.motor_log_count, before);
}
