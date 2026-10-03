/**
 * @file  proto_codec.c
 * @brief 协议编解码实现。逐字节布局见 docs/protocol.md §5。
 *
 * 实现纪律：
 *   - 没有一处位域；所有多字节与位操作都走 proto_bytes.h
 *   - 解码函数除写入 *out 外不产生任何副作用（可重入）
 *   - 取值范围校验是**强制的**：越界即 PROTO_ERR_RANGE，不截断
 */
#include "proto_codec.h"

#include <string.h>

#include "proto_bytes.h"

/* ----------------------------- 枚举合法性校验 ----------------------------- */

static bool cmd_is_valid(uint8_t v)
{
    return v == PROTO_CMD_SET || v == PROTO_CMD_STOP ||
           v == PROTO_CMD_RESET || v == PROTO_CMD_QUERY;
}

static bool device_is_valid(uint8_t v)
{
    return (unsigned)v <= (unsigned)PROTO_DEV_SYSTEM;
}

static bool event_code_is_valid(uint8_t v)
{
    return v >= PROTO_EV_OVER_TEMP && v <= PROTO_EV_SENSOR_FAULT;
}

static bool level_is_valid(uint8_t v)
{
    return (unsigned)v <= (unsigned)PROTO_LVL_CRITICAL;
}

static bool motor_state_is_valid(uint8_t v)
{
    return (unsigned)v <= (unsigned)PROTO_MOTOR_BRAKE;
}

/** 写帧头：ID 由 proto_make_id 生成（保证不与手写常量脱节）。 */
static proto_result_t frame_init(proto_can_frame_t *f, proto_category_t cat,
                                 uint8_t node, proto_frame_type_t type)
{
    if (f == NULL) {
        return PROTO_ERR_ID;
    }
    const uint16_t id = proto_make_id(cat, node, type);
    if (id == 0u) {
        return PROTO_ERR_ID;
    }
    memset(f, 0, sizeof(*f)); /* 保留位/保留字节一律置 0 */
    f->id = id;
    f->dlc = PROTO_DLC;
    return PROTO_OK;
}

/** 解码前统一检查：指针、ID 匹配、DLC。 */
static proto_result_t frame_check(const proto_can_frame_t *f, uint16_t expect_id)
{
    if (f == NULL) {
        return PROTO_ERR_ID;
    }
    if (!proto_id_is_valid(f->id)) {
        return PROTO_ERR_ID;
    }
    if (f->id != expect_id) {
        return PROTO_ERR_ID;
    }
    if (f->dlc != (uint8_t)PROTO_DLC) {
        return PROTO_ERR_LEN;
    }
    return PROTO_OK;
}

/* ------------------------------- 编码实现 -------------------------------- */

proto_result_t proto_encode_telemetry_node1(const proto_telemetry_node1_t *m,
                                            proto_can_frame_t *out)
{
    if (m == NULL) {
        return PROTO_ERR_ID;
    }
    if (m->motor_duty_pct > PROTO_TELEM_DUTY_PCT_MAX) {
        return PROTO_ERR_RANGE;
    }
    if (!motor_state_is_valid(m->motor_state)) {
        return PROTO_ERR_VALUE;
    }
    const proto_result_t r = frame_init(out, PROTO_CAT_TELEMETRY,
                                        PROTO_NODE_1, PROTO_TYPE_NORMAL);
    if (r != PROTO_OK) {
        return r;
    }
    out->data[0] = m->seq;
    out->data[1] = m->valid;
    proto_put_i16_le(&out->data[2], m->temperature);
    out->data[4] = m->do_state;
    out->data[5] = m->motor_state;
    out->data[6] = m->motor_duty_pct;
    out->data[7] = m->status;
    return PROTO_OK;
}

proto_result_t proto_encode_telemetry_node2(const proto_telemetry_node2_t *m,
                                            proto_can_frame_t *out)
{
    if (m == NULL) {
        return PROTO_ERR_ID;
    }
    if (m->servo_angle > PROTO_SERVO_ANGLE_MAX ||
        m->ir_ao_pct > PROTO_PCT_MAX) {
        return PROTO_ERR_RANGE;
    }
    const proto_result_t r = frame_init(out, PROTO_CAT_TELEMETRY,
                                        PROTO_NODE_2, PROTO_TYPE_NORMAL);
    if (r != PROTO_OK) {
        return r;
    }
    out->data[0] = m->seq;
    out->data[1] = m->valid;
    proto_put_u16_le(&out->data[2], m->light_lux);
    out->data[4] = m->ir_do;
    out->data[5] = m->servo_angle;
    out->data[6] = m->ir_ao_pct;
    out->data[7] = m->status;
    return PROTO_OK;
}

proto_result_t proto_encode_event(uint8_t node, const proto_event_msg_t *m,
                                  proto_can_frame_t *out)
{
    if (m == NULL) {
        return PROTO_ERR_ID;
    }
    if (!event_code_is_valid(m->event_code)) {
        return PROTO_ERR_VALUE;
    }
    if (!level_is_valid(m->level)) {
        return PROTO_ERR_VALUE;
    }
    const proto_result_t r = frame_init(out, PROTO_CAT_EVENT, node,
                                        PROTO_TYPE_EVENT);
    if (r != PROTO_OK) {
        return r;
    }
    out->data[0] = m->event_code;
    out->data[1] = m->level;
    proto_put_i16_le(&out->data[2], m->value);
    out->data[4] = m->channel;
    proto_put_u16_le(&out->data[5], m->uptime_s);
    out->data[7] = m->repeat_count;
    return PROTO_OK;
}

proto_result_t proto_encode_command(uint8_t node, const proto_command_msg_t *m,
                                    proto_can_frame_t *out)
{
    if (m == NULL) {
        return PROTO_ERR_ID;
    }
    if (!cmd_is_valid(m->cmd)) {
        return PROTO_ERR_VALUE;
    }
    if (!device_is_valid(m->device)) {
        return PROTO_ERR_VALUE;
    }
    /* 发送侧也不放过越界参数：宁可在主站就报错，也不要让总线上出现非法命令 */
    if (!proto_param_valid_for((proto_device_t)m->device, m->param)) {
        return PROTO_ERR_RANGE;
    }
    const proto_result_t r = frame_init(out, PROTO_CAT_COMMAND, node,
                                        PROTO_TYPE_NORMAL);
    if (r != PROTO_OK) {
        return r;
    }
    out->data[0] = m->seq;
    out->data[1] = m->cmd;
    out->data[2] = m->device;
    out->data[3] = m->channel;
    proto_put_i16_le(&out->data[4], m->param);
    out->data[6] = m->options;
    return PROTO_OK;
}

proto_result_t proto_encode_ack(uint8_t node, const proto_ack_msg_t *m,
                                proto_can_frame_t *out)
{
    if (m == NULL) {
        return PROTO_ERR_ID;
    }
    const proto_result_t r = frame_init(out, PROTO_CAT_HEARTBEAT_ACK, node,
                                        PROTO_TYPE_NORMAL);
    if (r != PROTO_OK) {
        return r;
    }
    out->data[0] = m->echo_seq;
    out->data[1] = m->result;
    out->data[2] = m->state;
    return PROTO_OK;
}

proto_result_t proto_encode_heartbeat(const proto_heartbeat_msg_t *m,
                                      proto_can_frame_t *out)
{
    if (m == NULL) {
        return PROTO_ERR_ID;
    }
    const proto_result_t r = frame_init(out, PROTO_CAT_HEARTBEAT_ACK,
                                        PROTO_NODE_MASTER, PROTO_TYPE_NORMAL);
    if (r != PROTO_OK) {
        return r;
    }
    out->data[0] = m->heartbeat_count;
    out->data[1] = m->system_mode;
    proto_put_u16_le(&out->data[2], m->online_bitmap);
    out->data[4] = m->fw_version_uniform;
    out->data[5] = m->protocol_version;
    return PROTO_OK;
}

/* ------------------------------- 解码实现 -------------------------------- */

proto_result_t proto_decode_telemetry_node1(const proto_can_frame_t *f,
                                            proto_telemetry_node1_t *out)
{
    if (out == NULL) {
        return PROTO_ERR_ID;
    }
    proto_result_t r = frame_check(f, PROTO_ID_TELEMETRY_NODE1);
    if (r != PROTO_OK) {
        return r;
    }
    out->seq = f->data[0];
    out->valid = f->data[1];
    out->temperature = proto_get_i16_le(&f->data[2]);
    out->do_state = f->data[4];
    out->motor_state = f->data[5];
    out->motor_duty_pct = f->data[6];
    out->status = f->data[7];

    if (!motor_state_is_valid(out->motor_state)) {
        return PROTO_ERR_VALUE;
    }
    if (out->motor_duty_pct > PROTO_TELEM_DUTY_PCT_MAX) {
        return PROTO_ERR_RANGE;
    }
    return PROTO_OK;
}

proto_result_t proto_decode_telemetry_node2(const proto_can_frame_t *f,
                                            proto_telemetry_node2_t *out)
{
    if (out == NULL) {
        return PROTO_ERR_ID;
    }
    proto_result_t r = frame_check(f, PROTO_ID_TELEMETRY_NODE2);
    if (r != PROTO_OK) {
        return r;
    }
    out->seq = f->data[0];
    out->valid = f->data[1];
    out->light_lux = proto_get_u16_le(&f->data[2]);
    out->ir_do = f->data[4];
    out->servo_angle = f->data[5];
    out->ir_ao_pct = f->data[6];
    out->status = f->data[7];

    if (out->servo_angle > PROTO_SERVO_ANGLE_MAX) {
        return PROTO_ERR_RANGE;
    }
    if (out->ir_ao_pct > PROTO_PCT_MAX) {
        return PROTO_ERR_RANGE;
    }
    return PROTO_OK;
}

proto_result_t proto_decode_event(const proto_can_frame_t *f,
                                  proto_event_msg_t *out)
{
    if (out == NULL) {
        return PROTO_ERR_ID;
    }
    if (f == NULL || (f->id != PROTO_ID_NODE1_EVENT &&
                      f->id != PROTO_ID_NODE2_EVENT)) {
        return PROTO_ERR_ID;
    }
    proto_result_t r = frame_check(f, f->id);
    if (r != PROTO_OK) {
        return r;
    }
    out->event_code = f->data[0];
    out->level = f->data[1];
    out->value = proto_get_i16_le(&f->data[2]);
    out->channel = f->data[4];
    out->uptime_s = proto_get_u16_le(&f->data[5]);
    out->repeat_count = f->data[7];

    if (!event_code_is_valid(out->event_code)) {
        return PROTO_ERR_VALUE;
    }
    if (!level_is_valid(out->level)) {
        return PROTO_ERR_VALUE;
    }
    return PROTO_OK;
}

proto_result_t proto_decode_command(const proto_can_frame_t *f,
                                    uint8_t expect_node,
                                    proto_command_msg_t *out)
{
    if (out == NULL) {
        return PROTO_ERR_ID;
    }
    if (f == NULL) {
        return PROTO_ERR_ID;
    }
    if (f->id != PROTO_ID_CMD_BROADCAST && f->id != PROTO_ID_CMD_NODE1 &&
        f->id != PROTO_ID_CMD_NODE2) {
        return PROTO_ERR_ID;
    }
    const uint8_t node = proto_id_node(f->id);
    const bool is_broadcast = (node == PROTO_NODE_MASTER);

    /* 节点过滤：广播人人可收；单播只有目标节点收，其余节点必须丢弃。
     * 这不是冗余检查——硬件验收滤波器是第一道防线，这里是第二道。 */
    if (!is_broadcast && node != expect_node) {
        return PROTO_ERR_NODE;
    }
    proto_result_t r = frame_check(f, f->id);
    if (r != PROTO_OK) {
        return r;
    }
    out->seq = f->data[0];
    out->cmd = f->data[1];
    out->device = f->data[2];
    out->channel = f->data[3];
    out->param = proto_get_i16_le(&f->data[4]);
    out->options = f->data[6];

    if (!cmd_is_valid(out->cmd)) {
        return PROTO_ERR_VALUE;
    }
    if (!device_is_valid(out->device)) {
        return PROTO_ERR_VALUE;
    }
    if (!proto_param_valid_for((proto_device_t)out->device, out->param)) {
        return PROTO_ERR_RANGE;
    }
    return PROTO_OK;
}

proto_result_t proto_decode_ack(const proto_can_frame_t *f,
                                proto_ack_msg_t *out)
{
    if (out == NULL) {
        return PROTO_ERR_ID;
    }
    if (f == NULL ||
        (f->id != PROTO_ID_ACK_NODE1 && f->id != PROTO_ID_ACK_NODE2)) {
        return PROTO_ERR_ID;
    }
    proto_result_t r = frame_check(f, f->id);
    if (r != PROTO_OK) {
        return r;
    }
    out->echo_seq = f->data[0];
    out->result = f->data[1];
    out->state = f->data[2];
    return PROTO_OK;
}

proto_result_t proto_decode_heartbeat(const proto_can_frame_t *f,
                                      proto_heartbeat_msg_t *out)
{
    if (out == NULL) {
        return PROTO_ERR_ID;
    }
    proto_result_t r = frame_check(f, PROTO_ID_MASTER_HEARTBEAT);
    if (r != PROTO_OK) {
        return r;
    }
    out->heartbeat_count = f->data[0];
    out->system_mode = f->data[1];
    out->online_bitmap = proto_get_u16_le(&f->data[2]);
    out->fw_version_uniform = f->data[4];
    out->protocol_version = f->data[5];
    return PROTO_OK;
}

/* ------------------------------- 统一分派 -------------------------------- */

proto_result_t proto_decode_dispatch(const proto_can_frame_t *f,
                                     uint8_t expect_node,
                                     proto_decoded_t *out)
{
    if (f == NULL || out == NULL) {
        return PROTO_ERR_ID;
    }
    if (!proto_id_is_valid(f->id)) {
        return PROTO_ERR_ID;
    }
    out->id = f->id;
    switch (f->id) {
    case PROTO_ID_TELEMETRY_NODE1:
        return proto_decode_telemetry_node1(f, &out->u.telem1);
    case PROTO_ID_TELEMETRY_NODE2:
        return proto_decode_telemetry_node2(f, &out->u.telem2);
    case PROTO_ID_NODE1_EVENT:
    case PROTO_ID_NODE2_EVENT:
        return proto_decode_event(f, &out->u.event);
    case PROTO_ID_CMD_BROADCAST:
    case PROTO_ID_CMD_NODE1:
    case PROTO_ID_CMD_NODE2:
        return proto_decode_command(f, expect_node, &out->u.command);
    case PROTO_ID_ACK_NODE1:
    case PROTO_ID_ACK_NODE2:
        return proto_decode_ack(f, &out->u.ack);
    case PROTO_ID_MASTER_HEARTBEAT:
        return proto_decode_heartbeat(f, &out->u.heartbeat);
    default:
        return PROTO_ERR_ID; /* 表外 ID：丢弃，不猜测长度与含义 */
    }
}
