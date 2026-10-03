/**
 * @file  test_codec.c
 * @brief 编解码回归测试：黄金报文字节、负温符号扩展、越界拒收、ID 规则。
 */
#define UTEST_MAIN
#define UTEST_SUITE_NAME "proto_codec"

#include "utest.h"

#include "proto_codec.h"
#include "proto_seq.h"

/* ------------------------------------------------------------------------- */
/* 黄金报文：一旦这些字节变了，说明wire格式被改动 —— 必须同步 DBC 与文档        */
/* ------------------------------------------------------------------------- */

UTEST_CASE(telemetry_node1)
{
    /* 28.5 ℃ → 285 → 0x011D → 小端 1D 01；电机正转、占空比 60 % */
    const proto_telemetry_node1_t m = {
        .seq = 7u,
        .valid = 0x03u,      /* 温度 + DO 有效 */
        .temperature = 285,  /* 28.5 ℃ */
        .do_state = 0u,
        .motor_state = PROTO_MOTOR_FORWARD,
        .motor_duty_pct = 60u,
        .status = 0u,
    };
    proto_can_frame_t f;
    UTEST_EQ_INT(proto_encode_telemetry_node1(&m, &f), PROTO_OK);
    UTEST_EQ_INT(f.id, PROTO_ID_TELEMETRY_NODE1);
    UTEST_EQ_INT(f.dlc, 8u);
    const uint8_t want[8] = { 0x07, 0x03, 0x1D, 0x01, 0x00, 0x01, 0x3C, 0x00 };
    UTEST_EQ_BYTES(f.data, want, 8);

    proto_telemetry_node1_t back;
    UTEST_EQ_INT(proto_decode_telemetry_node1(&f, &back), PROTO_OK);
    UTEST_EQ_INT(back.seq, 7u);
    UTEST_EQ_INT(back.temperature, 285);
    UTEST_EQ_INT(back.motor_state, PROTO_MOTOR_FORWARD);
    UTEST_EQ_INT(back.motor_duty_pct, 60u);
}

UTEST_CASE(telemetry_node2)
{
    /* 光照 1234 lux → 0x04D2 → 小端 D2 04 */
    const proto_telemetry_node2_t m = {
        .seq = 1u,
        .valid = 0x07u,
        .light_lux = 1234u,
        .ir_do = 1u,
        .servo_angle = 90u,
        .ir_ao_pct = 42u,
        .status = 0u,
    };
    proto_can_frame_t f;
    UTEST_EQ_INT(proto_encode_telemetry_node2(&m, &f), PROTO_OK);
    UTEST_EQ_INT(f.id, PROTO_ID_TELEMETRY_NODE2);
    const uint8_t want[8] = { 0x01, 0x07, 0xD2, 0x04, 0x01, 0x5A, 0x2A, 0x00 };
    UTEST_EQ_BYTES(f.data, want, 8);

    /* 舵机角度越界必须被拒（而不是截断到 180） */
    proto_telemetry_node2_t bad = m;
    bad.servo_angle = 181u;
    UTEST_EQ_INT(proto_encode_telemetry_node2(&bad, &f), PROTO_ERR_RANGE);

    /* 解码侧同样强制校验：把越界值直接塞进帧里 */
    proto_can_frame_t raw = f;
    raw.id = PROTO_ID_TELEMETRY_NODE2;
    raw.dlc = 8u;
    raw.data[5] = 200u;
    proto_telemetry_node2_t back;
    UTEST_EQ_INT(proto_decode_telemetry_node2(&raw, &back), PROTO_ERR_RANGE);
}

UTEST_CASE(event)
{
    /* 超温、警告级、触发值 85.0 ℃ = 850 = 0x0352、运行 300 s、重复 2 次 */
    const proto_event_msg_t m = {
        .event_code = PROTO_EV_OVER_TEMP,
        .level = PROTO_LVL_WARNING,
        .value = 850,
        .channel = 0u,
        .uptime_s = 300u,
        .repeat_count = 2u,
    };
    proto_can_frame_t f;
    UTEST_EQ_INT(proto_encode_event(1u, &m, &f), PROTO_OK);
    UTEST_EQ_INT(f.id, PROTO_ID_NODE1_EVENT);
    const uint8_t want[8] = { 0x01, 0x01, 0x52, 0x03, 0x00, 0x2C, 0x01, 0x02 };
    UTEST_EQ_BYTES(f.data, want, 8);

    /* 节点二的事件帧 ID 必须是 0x021，布局相同 */
    proto_can_frame_t f2;
    UTEST_EQ_INT(proto_encode_event(2u, &m, &f2), PROTO_OK);
    UTEST_EQ_INT(f2.id, PROTO_ID_NODE2_EVENT);
    UTEST_EQ_BYTES(f2.data, want, 8);

    proto_event_msg_t back;
    UTEST_EQ_INT(proto_decode_event(&f, &back), PROTO_OK);
    UTEST_EQ_INT(back.event_code, PROTO_EV_OVER_TEMP);
    UTEST_EQ_INT(back.level, PROTO_LVL_WARNING);
    UTEST_EQ_INT(back.value, 850);
    UTEST_EQ_INT(back.uptime_s, 300u);
    UTEST_EQ_INT(back.repeat_count, 2u);

    /* 未定义事件码 → ERR_VALUE */
    proto_event_msg_t bad = m;
    bad.event_code = 0x7Fu;
    UTEST_EQ_INT(proto_encode_event(1u, &bad, &f), PROTO_ERR_VALUE);
}

UTEST_CASE(command)
{
    /* 舵机转到 90°：cmd=SET、device=SERVO、param=90 → 0x005A → 5A 00 */
    const proto_command_msg_t m = {
        .seq = 42u,
        .cmd = PROTO_CMD_SET,
        .device = PROTO_DEV_SERVO,
        .channel = 0u,
        .param = 90,
        .options = 0u,
    };
    proto_can_frame_t f;
    UTEST_EQ_INT(proto_encode_command(2u, &m, &f), PROTO_OK);
    UTEST_EQ_INT(f.id, PROTO_ID_CMD_NODE2);
    const uint8_t want[8] = { 0x2A, 0x01, 0x01, 0x00, 0x5A, 0x00, 0x00, 0x00 };
    UTEST_EQ_BYTES(f.data, want, 8);

    /* 广播命令 ID 必须是 0x100 */
    proto_can_frame_t fb;
    UTEST_EQ_INT(proto_encode_command(0u, &m, &fb), PROTO_OK);
    UTEST_EQ_INT(fb.id, PROTO_ID_CMD_BROADCAST);

    /* 节点一收到发往节点二的单播 → ERR_NODE */
    proto_command_msg_t back;
    UTEST_EQ_INT(proto_decode_command(&f, 1u, &back), PROTO_ERR_NODE);
    /* 节点一收到广播 → 接受 */
    UTEST_EQ_INT(proto_decode_command(&fb, 1u, &back), PROTO_OK);
    UTEST_EQ_INT(back.seq, 42u);

    /* 舵机参数越界（181 > 180）→ 发送侧即拒 */
    proto_command_msg_t bad = m;
    bad.param = 181;
    UTEST_EQ_INT(proto_encode_command(2u, &bad, &fb), PROTO_ERR_RANGE);

    /* 电机可用 0~1000 精细标度（舵机的 180 上限不适用于电机） */
    proto_command_msg_t motor = m;
    motor.device = PROTO_DEV_MOTOR;
    motor.param = 1000;
    UTEST_EQ_INT(proto_encode_command(1u, &motor, &fb), PROTO_OK);

    /* 电机参数 1001 → 越界 */
    motor.param = 1001;
    UTEST_EQ_INT(proto_encode_command(1u, &motor, &fb), PROTO_ERR_RANGE);

    /* 把越界参数直接塞进线上（模拟出错或恶意节点） */
    proto_can_frame_t raw;
    raw.id = PROTO_ID_CMD_NODE1;
    raw.dlc = 8u;
    memset(raw.data, 0, sizeof(raw.data));
    raw.data[1] = (uint8_t)PROTO_CMD_SET;
    raw.data[2] = (uint8_t)PROTO_DEV_SERVO;
    raw.data[4] = 0xFFu; /* param = 255 > 180 */
    raw.data[5] = 0x00u;
    UTEST_EQ_INT(proto_decode_command(&raw, 1u, &back), PROTO_ERR_RANGE);

    /* 未定义命令字 → ERR_VALUE */
    raw.data[1] = 0x9Fu;
    raw.data[2] = (uint8_t)PROTO_DEV_MOTOR;
    raw.data[4] = 0x00u;
    UTEST_EQ_INT(proto_decode_command(&raw, 1u, &back), PROTO_ERR_VALUE);
}

UTEST_CASE(ack)
{
    const proto_ack_msg_t m = {
        .echo_seq = 42u,
        .result = (uint8_t)PROTO_OK,
        .state = 3u,
    };
    proto_can_frame_t f;
    UTEST_EQ_INT(proto_encode_ack(1u, &m, &f), PROTO_OK);
    UTEST_EQ_INT(f.id, PROTO_ID_ACK_NODE1);
    const uint8_t want[8] = { 0x2A, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00 };
    UTEST_EQ_BYTES(f.data, want, 8);

    UTEST_EQ_INT(proto_encode_ack(2u, &m, &f), PROTO_OK);
    UTEST_EQ_INT(f.id, PROTO_ID_ACK_NODE2);

    proto_ack_msg_t back;
    UTEST_EQ_INT(proto_decode_ack(&f, &back), PROTO_OK);
    UTEST_EQ_INT(back.echo_seq, 42u);
    UTEST_EQ_INT(back.result, PROTO_OK);
    UTEST_EQ_INT(back.state, 3u);
}

UTEST_CASE(heartbeat)
{
    const proto_heartbeat_msg_t m = {
        .heartbeat_count = 9u,
        .system_mode = 0u,
        .online_bitmap = (uint16_t)(proto_node_bit(1u) | proto_node_bit(2u)),
        .fw_version_uniform = 0u,
        .protocol_version = PROTO_PROTOCOL_VERSION,
    };
    proto_can_frame_t f;
    UTEST_EQ_INT(proto_encode_heartbeat(&m, &f), PROTO_OK);
    UTEST_EQ_INT(f.id, PROTO_ID_MASTER_HEARTBEAT);
    /* bit1|bit2 = 0x0006 → 小端 06 00；b5 = 协议版本 */
    const uint8_t want[8] = { 0x09, 0x00, 0x06, 0x00, 0x00, 0x01, 0x00, 0x00 };
    UTEST_EQ_BYTES(f.data, want, 8);

    proto_heartbeat_msg_t back;
    UTEST_EQ_INT(proto_decode_heartbeat(&f, &back), PROTO_OK);
    UTEST_EQ_INT(back.online_bitmap, 6u);
    UTEST_EQ_INT(back.protocol_version, PROTO_PROTOCOL_VERSION);
    UTEST_CHECK(proto_heartbeat_node_online(&back, 1u));
    UTEST_CHECK(proto_heartbeat_node_online(&back, 2u));
    UTEST_CHECK(!proto_heartbeat_node_online(&back, 3u));
    /* 节点号 0 与 >15 都不是有效节点，不得判为在线 */
    UTEST_CHECK(!proto_heartbeat_node_online(&back, 0u));
    UTEST_CHECK(!proto_heartbeat_node_online(&back, 16u));
}

UTEST_CASE(id_rules)
{
    /* 3+4+4 编码与常量自洽 */
    UTEST_EQ_INT(proto_make_id(PROTO_CAT_EVENT, 1u, PROTO_TYPE_EVENT), 0x011u);
    UTEST_EQ_INT(proto_make_id(PROTO_CAT_COMMAND, 0u, PROTO_TYPE_NORMAL), 0x100u);
    UTEST_EQ_INT(proto_make_id(PROTO_CAT_TELEMETRY, 2u, PROTO_TYPE_NORMAL), 0x220u);
    UTEST_EQ_INT(proto_make_id(PROTO_CAT_HEARTBEAT_ACK, 2u, PROTO_TYPE_NORMAL), 0x320u);
    /* 节点三扩展：事件 0x031 / 命令 0x130 / 遥测 0x230 / ACK 0x330 */
    UTEST_EQ_INT(proto_make_id(PROTO_CAT_EVENT, 3u, PROTO_TYPE_EVENT), 0x031u);
    UTEST_EQ_INT(proto_make_id(PROTO_CAT_COMMAND, 3u, PROTO_TYPE_NORMAL), 0x130u);
    UTEST_EQ_INT(proto_make_id(PROTO_CAT_TELEMETRY, 3u, PROTO_TYPE_NORMAL), 0x230u);

    /* 越界参数不得生成"看起来合法"的 ID */
    UTEST_EQ_INT(proto_make_id(PROTO_CAT_COUNT, 1u, PROTO_TYPE_EVENT), 0u);
    UTEST_EQ_INT(proto_make_id(PROTO_CAT_EVENT, 16u, PROTO_TYPE_EVENT), 0u);
    UTEST_EQ_INT(proto_make_id(PROTO_CAT_EVENT, 1u, PROTO_TYPE_MAX + 1), 0u);

    /* 全部 10 个常量必须合法 */
    for (size_t i = 0; i < PROTO_FRAME_COUNT; ++i) {
        UTEST_CHECK(proto_id_is_valid(PROTO_FRAME_TABLE[i].id));
        UTEST_EQ_INT(PROTO_FRAME_TABLE[i].dlc, 8u);
    }

    /* 语义规则：事件类别必须是 type=1；主站不能是遥测/ACK 来源；ID 0 非法 */
    UTEST_CHECK(!proto_id_is_valid(0x000u));
    UTEST_CHECK(!proto_id_is_valid(0x010u)); /* 事件类别但 type=0 */
    UTEST_CHECK(!proto_id_is_valid(0x200u)); /* 遥测但 node=0（主站） */
    UTEST_CHECK(!proto_id_is_valid(0x400u)); /* 超出 11 位 */
    /* 0x0F0 = 0b0_1111_0000 → 类别 0（事件）+ 节点 15 + type 0：
     * 事件类别必须是 type=1，因此非法。 */
    UTEST_CHECK(!proto_id_is_valid(0x0F0u));
    /* 0x1F0 = 类别 1（命令）+ 节点 15 + type 0：结构合法，仅本协议未定义该帧 */
    UTEST_CHECK(proto_id_is_valid(0x1F0u));
    UTEST_CHECK(proto_frame_by_id(0x1F0u) == NULL);
    /* 主站帧必须合法：0x300 心跳、0x100 广播命令。
     * 回归点：曾因把"必须来自从节点"套用到整个 HEARTBEAT_ACK 类别上，
     * 而把主站自己的心跳判成非法。 */
    UTEST_CHECK(proto_id_is_valid(PROTO_ID_MASTER_HEARTBEAT));
    UTEST_CHECK(proto_id_is_valid(PROTO_ID_CMD_BROADCAST));
    /* 遥测 / ACK 都不能来自主站（node=0）；只有心跳允许 node=0 */
    UTEST_CHECK(!proto_id_is_valid(proto_make_id(PROTO_CAT_TELEMETRY, 0u,
                                                 PROTO_TYPE_NORMAL)));
    UTEST_CHECK(proto_id_is_valid(PROTO_ID_ACK_NODE1));
    UTEST_CHECK(proto_id_is_valid(PROTO_ID_ACK_NODE2));
    /* ACK 类别 + node=0 + type=1（非 NORMAL）→ 非法 */
    UTEST_CHECK(!proto_id_is_valid(0x301u));
    /* 扩展节点的 ACK 帧结构合法（仅本协议未定义） */
    UTEST_CHECK(proto_id_is_valid(0x380u));
    UTEST_CHECK(proto_frame_by_id(0x380u) == NULL);
    /* 事件也必须有明确的从节点来源：0x001（node=0）非法 */
    UTEST_CHECK(!proto_id_is_valid(0x001u));

    /* 表外但结构合法的 ID 仍是"合法 ID"（扩展节点用），只是本协议未定义 */
    UTEST_CHECK(proto_id_is_valid(0x130u));
    UTEST_CHECK(proto_frame_by_id(0x130u) == NULL);
    UTEST_CHECK(proto_frame_by_id(PROTO_ID_CMD_NODE1) != NULL);

    /* 按名字查表：DBC 名与代码名必须一致 */
    const proto_frame_info_t *info = proto_frame_by_name("TELEMETRY_NODE1");
    UTEST_CHECK(info != NULL);
    if (info != NULL) {
        UTEST_EQ_INT(info->id, PROTO_ID_TELEMETRY_NODE1);
        UTEST_EQ_INT(info->cycle_ms, 1000u);
    }
    UTEST_CHECK(proto_frame_by_name("NO_SUCH_FRAME") == NULL);
    UTEST_CHECK(proto_frame_by_name(NULL) == NULL);

    /* 广播 ID 必须小于单播 ID（否则广播会被单播持续压制） */
    UTEST_CHECK(PROTO_ID_CMD_BROADCAST < PROTO_ID_CMD_NODE1);
    UTEST_CHECK(PROTO_ID_CMD_BROADCAST < PROTO_ID_CMD_NODE2);
}

UTEST_CASE(dispatch)
{
    const proto_telemetry_node1_t m = {
        .seq = 3u, .valid = 0x01u, .temperature = -55, /* −5.5 ℃，负温 */
        .do_state = 0u, .motor_state = PROTO_MOTOR_STOP,
        .motor_duty_pct = 0u, .status = 0u,
    };
    proto_can_frame_t f;
    UTEST_EQ_INT(proto_encode_telemetry_node1(&m, &f), PROTO_OK);
    /* −55 = 0xFFC9 → 小端 C9 FF：验证负温符号扩展 */
    UTEST_EQ_INT(f.data[2], 0xC9u);
    UTEST_EQ_INT(f.data[3], 0xFFu);

    proto_decoded_t d;
    UTEST_EQ_INT(proto_decode_dispatch(&f, PROTO_NODE_MASTER, &d), PROTO_OK);
    UTEST_EQ_INT(d.id, PROTO_ID_TELEMETRY_NODE1);
    UTEST_EQ_INT(d.u.telem1.temperature, -55);

    /* 表外 ID 必须被分派器拒绝 */
    f.id = 0x130u; /* 合法结构、但本协议未定义 */
    UTEST_EQ_INT(proto_decode_dispatch(&f, PROTO_NODE_MASTER, &d), PROTO_ERR_ID);
}
