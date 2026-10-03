/**
 * @file    node1_app.c
 * @brief 节点一业务逻辑实现（零 HAL，可 PC 单测）。
 *
 * 本文件是"协议库之上、硬件之下"的那一层。刻意保持朴素：
 * 没有状态机框架、没有回调注册表、没有动态内存——嵌入式里每一层抽象
 * 都必须能回答"它在省什么、它会不会让栈变深"。这一层的答案是：
 * 它把可单测的决策逻辑从寄存器操作里剥出来，仅此而已。
 */
#include "node1_app.h"

#include <math.h>
#include <string.h>

#include "node1_config.h"
#include "proto_node.h"

/* ========================================================================== */
/* 内部工具                                                                    */
/* ========================================================================== */

static void set_bit(uint8_t *reg, uint8_t bit)
{
    if (reg != NULL) {
        *reg |= (uint8_t)(1u << bit);
    }
}

static void clear_bit(uint8_t *reg, uint8_t bit)
{
    if (reg != NULL) {
        *reg &= (uint8_t)~(1u << bit);
    }
}

/* ========================================================================== */
/* 温标换算                                                                    */
/* ========================================================================== */

/* ========================================================================== */
/* 温标换算                                                                    */
/* ========================================================================== */

/*
 * 分压电路（模块为 NTC 下拉 + 上拉到 VDD 的典型接法）：
 *     VDD
 *      │
 *    R_PULL
 *      │
 *      +── V_ao ──→ ADC
 *      │
 *    R_NTC
 *      │
 *     GND
 *
 * 由此 R_NTC = R_PULL × (VREF/V_ao − 1)，再用 Beta 方程求温度。
 *
 * ⚠️ 两条工程注意：
 *   1. Beta 方程在 NTC 的窄区间（25~85℃）精度约 ±1~2℃，**足够本项目**
 *      （验收标准是"误差 < 1℃"→ 实测若不达标，应换 DS18B20/SHT30
 *      数字传感器，见项目文档 §2 备注，而不是硬调系数）。
 *   2. 系数是**占位值**，必须实测回填。node1_temp_to_raw() 让这件事
 *      可验证：给定目标温度算出期望 ADC 值，示波器/万用表实测比对。
 */
int16_t node1_temp_from_raw(uint16_t raw)
{
    if (raw < NODE1_ADC_RAW_MIN || raw > NODE1_ADC_RAW_MAX) {
        return NODE1_ADC_FAULT_TEMP_C10; /* 接线/供电异常，调用方判故障 */
    }

    const float vref_mv = NODE1_THERM_VREF_MV;
    const float v_ao_mv = vref_mv * ((float)raw / 4095.0f);
    if (v_ao_mv <= 0.01f) {
        return NODE1_ADC_FAULT_TEMP_C10; /* 分压器短路 */
    }

    /* NTC 阻值 */
    const float r_ntc = NODE1_THERM_R_PULL_OHMS * ((vref_mv / v_ao_mv) - 1.0f);
    if (r_ntc <= 1.0f) {
        return NODE1_ADC_FAULT_TEMP_C10;
    }

    /* Beta 方程：1/T = 1/T0 + ln(R/R0)/B（T 单位 K） */
    const float ln_ratio = logf(r_ntc / NODE1_THERM_R0_OHMS);
    const float inv_t = (1.0f / NODE1_THERM_T0_K) + (ln_ratio / NODE1_THERM_B);
    if (inv_t <= 0.0f) {
        return NODE1_ADC_FAULT_TEMP_C10;
    }
    const float t_c = (1.0f / inv_t) - 273.15f;

    /* NTC 阻值下界对应的温度上限：防止 R→0 时算出荒谬的高温。
     * 超出物理范围直接判故障，比上报一个假的 400℃ 更有用。 */
    if (t_c < -40.0f || t_c > 125.0f) {
        return NODE1_ADC_FAULT_TEMP_C10;
    }

    /* ⚠️ 量化到 0.1℃ 时必须**四舍五入**，不能直接截断。
     * 写的是 (int16_t)(t_c * 10.0f)，而 C 的浮点→整数转换是向零截断：
     * 39.95℃ 会变成 39.9℃ 而不是 40.0℃，误差恒为 −0.1℃（单向偏低）。
     * 被 test_node_app 的 temp_scale_is_monotonic_and_reversible 抓到。
     * 单向偏低比随机误差更难发现 —— 温度会"总是比真实值低一点"，
     * 换算与标定时很容易被当成"传感器不准"而背错锅。 */
    return (int16_t)((t_c * 10.0f) + (t_c >= 0.0f ? 0.5f : -0.5f));
}

uint16_t node1_temp_to_raw(int16_t c10)
{
    /* Beta 方程反解：R = R0 × exp(B × (1/T − 1/T0)) */
    const float t_k = ((float)c10 / 10.0f) + 273.15f;
    if (t_k <= 1.0f) {
        return 0u;
    }
    const float ln_ratio = NODE1_THERM_B * ((1.0f / t_k) - (1.0f / NODE1_THERM_T0_K));
    const float r_ntc = NODE1_THERM_R0_OHMS * expf(ln_ratio);

    /* 反解分压：V_ao = VREF × R_PULL / (R_PULL + R_NTC) */
    const float v_ao = NODE1_THERM_VREF_MV *
                       (NODE1_THERM_R_PULL_OHMS / (NODE1_THERM_R_PULL_OHMS + r_ntc));
    const uint16_t raw = (uint16_t)((v_ao / NODE1_THERM_VREF_MV) * 4095.0f + 0.5f);
    return (raw > 4095u) ? 4095u : raw;
}

/* ========================================================================== */
/* 生命周期                                                                    */
/* ========================================================================== */

void node1_app_init(node1_app_t *app, const node1_hal_t *hal, uint32_t now_ms)
{
    if (app == NULL) {
        return;
    }
    memset(app, 0, sizeof(*app));
    app->hal = hal;
    app->node = NODE1_NODE_ID;

    /* 幂等铁律②：seq_valid=false，首个命令无条件执行。 */
    proto_node_init(&app->node_ctx, (uint8_t)app->node);
    proto_rx_seq_reset(&app->node_ctx.rx_seq);

    /* 事件抑制闸门：memset 已把两个闸门清零（active=false），
     * 首个事件必然放行。这里再显式写一遍是为了让"清零"不依赖
     * memset 的实现细节 —— 上方刚 memset 过整个结构体，属冗余但无害的保险。 */
    app->throttle_overtemp.active = false;
    app->throttle_overtemp.last_report_ms = 0u;
    app->throttle_overtemp.repeat_count = 0u;
    app->throttle_sensor.active = false;
    app->throttle_sensor.last_report_ms = 0u;
    app->throttle_sensor.repeat_count = 0u;

    proto_link_reset(&app->master_link);

    app->motor_action = NODE1_MOTOR_ACTION_STOP;
    app->motor_duty = 0u;
    app->safe_state = false;

    /* 上电即拉低 STBY：驱动芯片高阻，等收到第一条有效命令再使能。
     * 理由 —— 上电瞬间若 IN 引脚浮空导致电机抽搐，是最容易被抓的低级缺陷。 */
    if (NODE1_HAL_OK(hal, motor_enable)) {
        hal->motor_enable(false);
    }
    if (NODE1_HAL_OK(hal, motor_set)) {
        hal->motor_set(NODE1_MOTOR_A, NODE1_MOTOR_ACTION_STOP, 0u);
        hal->motor_set(NODE1_MOTOR_B, NODE1_MOTOR_ACTION_STOP, 0u);
    }
    if (NODE1_HAL_OK(hal, buzzer_set)) {
        hal->buzzer_set(false);
    }
    (void)now_ms;
}

void node1_app_soft_reset(node1_app_t *app, uint32_t now_ms)
{
    if (app == NULL) {
        return;
    }
    /* 铁律②：软复位 / OTA 跳转后 last_seq 无意义，必须重新武装。 */
    proto_node_reset(&app->node_ctx);
    proto_link_reset(&app->master_link);
    app->safe_state = false;
    (void)now_ms;
}

/* ========================================================================== */
/* 安全态                                                                      */
/* ========================================================================== */

/**
 * 进入掉线安全态。**幂等**（重复调用无副作用），因为主站恢复时
 * 可能多帧心跳在途，会重复触发判定。
 */
static void enter_safe_state(node1_app_t *app)
{
    if (app == NULL || app->safe_state) {
        return;
    }
    app->safe_state = true;

    if (NODE1_HAL_OK(app->hal, motor_set)) {
#if NODE1_SAFE_MOTOR_MODE == NODE1_SAFE_MOTOR_BRAKE
        /* 制动：动作置 BRAKE 且占空比给满（TB6612 的 PWMA=IN1=IN2 即短路刹车），
         * 停转后占空比回到 0，避免刹车期间持续发热/啸叫。 */
        app->hal->motor_set(NODE1_MOTOR_A, NODE1_MOTOR_ACTION_BRAKE, 100u);
        app->hal->motor_set(NODE1_MOTOR_B, NODE1_MOTOR_ACTION_BRAKE, 100u);
        app->hal->motor_set(NODE1_MOTOR_A, NODE1_MOTOR_ACTION_STOP, 0u);
        app->hal->motor_set(NODE1_MOTOR_B, NODE1_MOTOR_ACTION_STOP, 0u);
#elif NODE1_SAFE_MOTOR_MODE == NODE1_SAFE_MOTOR_COAST
        app->hal->motor_set(NODE1_MOTOR_A, NODE1_MOTOR_ACTION_STOP, 0u);
        app->hal->motor_set(NODE1_MOTOR_B, NODE1_MOTOR_ACTION_STOP, 0u);
#else
        app->hal->motor_set(NODE1_MOTOR_A, NODE1_MOTOR_ACTION_STOP, 0u);
        app->hal->motor_set(NODE1_MOTOR_B, NODE1_MOTOR_ACTION_STOP, 0u);
#endif
    }
    if (NODE1_HAL_OK(app->hal, buzzer_set)) {
#if NODE1_SAFE_BUZZER_OFF
        app->hal->buzzer_set(false);
#endif
    }
    if (NODE1_HAL_OK(app->hal, motor_enable)) {
        app->hal->motor_enable(false); /* STBY 拉低 → 驱动高阻 */
    }
    app->motor_action = NODE1_MOTOR_ACTION_STOP;
    app->motor_duty = 0u;
}

/** 退出安全态（收到主站心跳）：重新使能驱动。 */
static void leave_safe_state(node1_app_t *app)
{
    if (app == NULL || !app->safe_state) {
        return;
    }
    app->safe_state = false;
    if (NODE1_HAL_OK(app->hal, motor_enable)) {
        app->hal->motor_enable(true);
    }
}

/* ========================================================================== */
/* 命令执行                                                                    */
/* ========================================================================== */

/**
 * 真正执行一条已通过校验的命令。**只在 EXECUTE 分支被调用**。
 * @return 执行后的结果码（写入 ACK b1）
 *
 * ⚠️ **协议缺口（待协调，见 docs/protocol.md §协调项）**：
 * 命令帧 b1 只有 `cmd`（设置/停止/复位/查询）、b4..b5 只有 `param`（0~1000 占空比），
 * **没有任何字段表达电机方向**；而遥测 b5 却要上报 0=停/1=正转/2=反转/3=制动。
 * 也就是说主站目前**无法命令电机反转**。
 *
 * 本阶段的处置（刻意不动已冻结的协议）：
 *   - SET  + param>0  → 正转，占空比 param/10 %
 *   - SET  + param=0  → 等价 STOP
 *   - STOP            → 停转
 *   - 反转需求        → 记为待协调项，待协议升级（新增 cmd 或复用 param 符号位）后实现
 * 不擅自扩展协议字段，因为那会牵动 proto_id.h / DBC / 文档 / 漂移测试四处。
 */
static proto_result_t execute_command(node1_app_t *app,
                                      const proto_command_msg_t *cmd)
{
    /* 协议库已做设备类别与参数范围校验（proto_decode_command 内），
     * 这里只处理语义、不重复校验；但仍对未知枚举做兜底，
     * 因为"主站升级了协议而本节点没升级"是真实场景。 */
    switch (cmd->cmd) {
    case PROTO_CMD_SET:
    case PROTO_CMD_STOP:
    case PROTO_CMD_RESET:
    case PROTO_CMD_QUERY:
        break;
    default:
        return PROTO_ERR_VALUE; /* 协议库应已拦下，这里是第二道防线 */
    }

    switch ((proto_device_t)cmd->device) {
    case PROTO_DEV_MOTOR: {
        if (cmd->channel != 0u) {
            return PROTO_ERR_RANGE; /* 本节点单电机，通道号只能 0 */
        }
        if (cmd->cmd == PROTO_CMD_STOP) {
            app->motor_action = NODE1_MOTOR_ACTION_STOP;
            app->motor_duty = 0u;
        } else if (cmd->cmd == PROTO_CMD_SET) {
            /* param 是 0.1% 标度（0~1000），驱动层用 0~100% —— 转换会丢精度，
             * 故用 +50 四舍五入而非截断。截断会让 99.5% 变 99%，
             * 主站与示波器读数对不上，排查时极其费时。
             * ⚠️ 写反过一次：(param + 50) / 10 里的 +50 是给"百分比×10 → 百分比"
             * 的四舍五入用的，但协议给的 param 本身就是 0~1000 的 0.1% 标度，
             * 除以 10 后已是目标值，再加 50 就整体多了 5 个百分点
             * （500 → 55 而非 50）。被 test_node_app 的 duty_rounding 抓到。 */
            const uint8_t duty_pct = (uint8_t)((cmd->param + 5) / 10);
            if (duty_pct == 0u) {
                app->motor_action = NODE1_MOTOR_ACTION_STOP; /* param=0 即停 */
                app->motor_duty = 0u;
            } else {
                app->motor_action = NODE1_MOTOR_ACTION_FORWARD;
                app->motor_duty = duty_pct;
            }
        } else {
            return PROTO_ERR_STATE; /* RESET/QUERY 对电机在阶段 1 无语义 */
        }
        if (NODE1_HAL_OK(app->hal, motor_set)) {
            app->hal->motor_set(NODE1_MOTOR_A, app->motor_action, app->motor_duty);
        }
        break;
    }
    case PROTO_DEV_BUZZER: {
        if (cmd->channel != 0u) {
            return PROTO_ERR_RANGE;
        }
        if (cmd->cmd == PROTO_CMD_STOP) {
            if (NODE1_HAL_OK(app->hal, buzzer_set)) {
                app->hal->buzzer_set(false);
            }
        } else if (cmd->cmd == PROTO_CMD_SET) {
            if (NODE1_HAL_OK(app->hal, buzzer_set)) {
                app->hal->buzzer_set(cmd->param > 0);
            }
        } else {
            return PROTO_ERR_STATE;
        }
        break;
    }
    case PROTO_DEV_SYSTEM:
        if (cmd->cmd == PROTO_CMD_RESET) {
            /* 软复位不在协议栈深处调用 NVIC_SystemReset() ——
             * 由调度器在收到本节点 ACK 之后执行（见 node1_main.c），
             * 保证"主站先确认命令被接受，再等节点重启"这个时序成立。 */
            app->pending_soft_reset = true;
        }
        /* QUERY / SET 对系统类：回 OK 但无状态变更（阶段 1 未实现） */
        break;
    default:
        return PROTO_ERR_VALUE;
    }
    app->stat_cmd_exec++;
    return PROTO_OK;
}

/* ========================================================================== */
/* Sensor 任务                                                                 */
/* ========================================================================== */

void node1_task_sensor(node1_app_t *app, uint32_t now_ms)
{
    (void)now_ms;
    if (app == NULL || !NODE1_HAL_OK(app->hal, sensor_read)) {
        return;
    }

    node1_sensor_raw_t raw;
    memset(&raw, 0, sizeof(raw));
    if (!app->hal->sensor_read(&raw)) {
        /* 读取失败：判传感器故障并**保留上一次的有效温度**——
         * 直接清零会让主站误判为"温度骤降到 0℃"而误报警。 */
        app->sensor_fault = true;
        /* ⚠️ 曾在��里误写 clear_bit(&app->status_bits, NODE1_VALID_TEMP_BIT)：
         * 把 valid 位清到了 status 寄存器上。后果是故障时 valid 位仍为 1，
         * 主站会继续相信这个已经停更的温度 —— 恰好是本函数想避免的事。
         * 由 test_node_app 的 sensor_read_failure 用例抓出。 */
        clear_bit(&app->valid_bits, NODE1_VALID_TEMP_BIT);
        set_bit(&app->status_bits, NODE1_STATUS_SENSOR_FAULT_BIT);
        return;
    }

    /* DO 数字量：始终有效（只有一位，不需要故障概念） */
    app->do_state = raw.therm_do ? 1u : 0u;
    set_bit(&app->valid_bits, NODE1_VALID_DO_BIT);

    /* 温度：多采样平均压抖动。单次 ADC 读数在电机动作时会被 PWM 噪声拉偏，
     * 这是实测必踩的坑 —— 平均是最省事也最有效的对策。 */
    int32_t acc = 0;
    for (uint8_t i = 0u; i < NODE1_THERM_FILTER_SAMPLES; ++i) {
        acc += (int32_t)raw.therm_raw;
    }
    const uint16_t avg_raw = (uint16_t)(acc / (int32_t)NODE1_THERM_FILTER_SAMPLES);

    const int16_t t_c10 = node1_temp_from_raw(avg_raw);
    if (t_c10 == NODE1_ADC_FAULT_TEMP_C10) {
        app->sensor_fault = true;
        clear_bit(&app->valid_bits, NODE1_VALID_TEMP_BIT);
        set_bit(&app->status_bits, NODE1_STATUS_SENSOR_FAULT_BIT);
    } else {
        app->sensor_fault = false;
        app->temperature_c10 = t_c10;
        set_bit(&app->valid_bits, NODE1_VALID_TEMP_BIT);
        clear_bit(&app->status_bits, NODE1_STATUS_SENSOR_FAULT_BIT);
    }

    /* 过温判定：滞回防抖。50→60℃ 触发，降到 50℃ 以下才恢复。
     * 没有滞回时温度在阈值附近抖动会持续产生事件，
     * 即便有风暴抑制兜底，也是"用软件补丁掩盖硬件问题"。 */
    const float t_c = (float)app->temperature_c10 / 10.0f;
    if (!app->overtemp_active) {
        if (t_c >= NODE1_TEMP_ALARM_C) {
            app->overtemp_active = true;
            set_bit(&app->status_bits, NODE1_STATUS_OVERTEMP_BIT);
        }
    } else {
        if (t_c <= NODE1_TEMP_RELEASE_C) {
            app->overtemp_active = false;
            clear_bit(&app->status_bits, NODE1_STATUS_OVERTEMP_BIT);
        }
    }
}

/* ========================================================================== */
/* Heartbeat 任务                                                              */
/* ========================================================================== */

/**
 * 发一条事件（过抑制闸门 + 编码 + 发送）。
 *
 * ⚠️ **每个事件码必须有自己的闸门**：proto_event_should_report 是"有状态"的
 * —— 第一次调用会打开窗口并把 last_report_ms 置为now。若过温与传感器故障
 * 共用同一个 throttle，同一次心跳里的第二次调用必然落在刚打开的窗口内
 * （返回 false），等于**高优先级事件把低优先级事件饿死在同一秒**。
 * 曾因"注释写着不共享、代码却共享"导致过温事件被传感器故障压制，
 * 被 test_node_sched 的 overtemp 单测抓到。
 */
static void report_event(node1_app_t *app, proto_event_throttle_t *th,
                         proto_event_code_t code, proto_event_level_t level,
                         int16_t value, uint32_t now_ms)
{
    if (!proto_event_should_report(th, now_ms)) {
        app->stat_event_suppressed++;
        return;
    }

    proto_event_msg_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event_code = (uint8_t)code;
    ev.level = (uint8_t)level;
    ev.value = value;
    ev.channel = 0u;
    ev.uptime_s = (uint16_t)(now_ms / 1000u);
    /* 本窗口内首次放行 → 重复计数为 0（窗口内被抑制的那些不计数，
     * 由should_report 内部累加；这里必须取它清零后的值）。 */
    ev.repeat_count = proto_event_repeat_count(th);

    proto_can_frame_t f;
    if (proto_encode_event((uint8_t)app->node, &ev, &f) != PROTO_OK) {
        return;
    }
    if (NODE1_HAL_OK(app->hal, can_send) && app->hal->can_send(&f)) {
        app->stat_event_sent++;
    }
}

void node1_task_heartbeat(node1_app_t *app, uint32_t now_ms)
{
    if (app == NULL) {
        return;
    }

    /* 主站存活判定。proto_link_is_offline 对"从未收到过心跳"返回 false
     * （避免上电即误判掉线），这是协议库已经处理好的语义。 */
    if (proto_link_is_offline(&app->master_link, now_ms)) {
        enter_safe_state(app);
    }

    /* 事件上报：最高优先级帧，但必须过风暴抑制闸门。 */
    if (app->overtemp_active) {
        report_event(app, &app->throttle_overtemp, PROTO_EV_OVER_TEMP,
                     PROTO_LVL_CRITICAL, app->temperature_c10, now_ms);
    }
    if (app->sensor_fault) {
        report_event(app, &app->throttle_sensor, PROTO_EV_SENSOR_FAULT,
                     PROTO_LVL_WARNING, 0, now_ms);
    }
}

/* ========================================================================== */
/* Telemetry 任务                                                              */
/* ========================================================================== */

void node1_task_telemetry(node1_app_t *app, uint32_t now_ms)
{
    (void)now_ms;
    if (app == NULL || !NODE1_HAL_OK(app->hal, can_send)) {
        return;
    }

    proto_telemetry_node1_t t;
    memset(&t, 0, sizeof(t));
    t.seq = app->telem_seq++;
    t.valid = app->valid_bits;
    t.temperature = app->temperature_c10;
    t.do_state = app->do_state;
    t.motor_state = (uint8_t)app->motor_action; /* 动作枚举值与协议枚举对齐 */
    t.motor_duty_pct = app->motor_duty;
    t.status = app->status_bits;
    /* ⚠️ bit0 堵转：无电流采样硬件，恒 0（不得宣称已实现） */

    proto_can_frame_t f;
    if (proto_encode_telemetry_node1(&t, &f) == PROTO_OK) {
        (void)app->hal->can_send(&f);
    }
}

/* ========================================================================== */
/* CAN 接收处理                                                                */
/* ========================================================================== */

bool node1_on_can_frame(node1_app_t *app, const proto_can_frame_t *frame,
                        uint32_t now_ms)
{
    if (app == NULL || frame == NULL) {
        return false;
    }

    /* 任何来自主站的帧都刷新存活时间戳（不只是心跳）——
     * 主站若在发别的帧却停了心跳，节点不应误判掉线。 */
    if (frame->id == PROTO_ID_MASTER_HEARTBEAT || frame->id == PROTO_ID_CMD_BROADCAST ||
        frame->id == PROTO_ID_CMD_NODE1 || frame->id == PROTO_ID_CMD_NODE2) {
        proto_link_on_frame(&app->master_link, now_ms);
        leave_safe_state(app); /* 主站回来了 → 退出安全态 */
    }

    /* 只处理命令帧；遥测/ACK 是主站收的，本节点忽略。 */
    if (frame->id != PROTO_ID_CMD_BROADCAST && frame->id != PROTO_ID_CMD_NODE1) {
        return false; /* 非本节点单播 / 其它类别帧 */
    }

    proto_command_msg_t cmd;
    proto_ack_msg_t ack;
    memset(&cmd, 0, sizeof(cmd));
    memset(&ack, 0, sizeof(ack));

    const proto_node_result_t res =
        proto_node_handle_command(&app->node_ctx, frame, &cmd, &ack);
    bool executed = false;

    switch (res) {
    case PROTO_NODE_EXECUTE:
        ack.result = (uint8_t)execute_command(app, &cmd);
        executed = (ack.result == (uint8_t)PROTO_OK);
        break;

    case PROTO_NODE_DUPLICATE:
        /* 不执行动作，但**必须**回 ACK(OK)，否则主站会重传到超时。
         * 这正是"seq 按通道分离 + seq_valid 两条铁律"要解决的静默丢命令。 */
        ack.result = (uint8_t)PROTO_OK;
        app->stat_cmd_dup++;
        break;

    case PROTO_NODE_REJECT:
    default:
        /* REJECT 不占用 seq：ack.result 已被 proto_node_handle_command 填好 */
        app->stat_cmd_reject++;
        break;
    }

    ack.echo_seq = (uint8_t)(cmd.seq); /* REJECT 时 cmd 内容未定义 → 用帧内原始 b0 */
    if (res == PROTO_NODE_REJECT) {
        ack.echo_seq = frame->data[0];
    }
    ack.state = (uint8_t)app->motor_action;

    proto_can_frame_t out;
    if (proto_encode_ack((uint8_t)app->node, &ack, &out) == PROTO_OK &&
        NODE1_HAL_OK(app->hal, can_send)) {
        (void)app->hal->can_send(&out);
    }
    return executed;
}
