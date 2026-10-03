/**
 * @file  proto_seq.c
 * @brief 幂等状态机与重传跟踪实现。见 proto_seq.h 顶部的规则说明。
 */
#include "proto_seq.h"

#include "proto_id.h"

/* ------------------------------ 接收侧 ----------------------------------- */

void proto_rx_seq_reset(proto_rx_seq_state_t *st)
{
    if (st == NULL) {
        return;
    }
    for (unsigned i = 0; i < (unsigned)PROTO_CH_COUNT; ++i) {
        st->last_seq[i] = 0u;
        /* 关键：复位后 seq_valid 必须为 false。
         * 若忘记这一条，主站发出的 seq=0 会被当作"重复"而丢弃。 */
        st->seq_valid[i] = false;
    }
}

proto_seq_channel_t proto_seq_channel_of(uint16_t frame_id)
{
    return (frame_id == PROTO_ID_CMD_BROADCAST) ? PROTO_CH_BROADCAST
                                                : PROTO_CH_UNICAST;
}

proto_rx_disposition_t proto_rx_seq_handle(proto_rx_seq_state_t *st,
                                           uint16_t frame_id, uint8_t seq)
{
    if (st == NULL) {
        return PROTO_RX_REJECT;
    }
    /* 只接受命令类帧；其它 ID 属调用方错误，一律拒绝且不占用 seq */
    if (frame_id != PROTO_ID_CMD_BROADCAST && frame_id != PROTO_ID_CMD_NODE1 &&
        frame_id != PROTO_ID_CMD_NODE2) {
        return PROTO_RX_REJECT;
    }
    const proto_seq_channel_t ch = proto_seq_channel_of(frame_id);

    /* 规则 2：复位/上电后的第一个命令无条件执行 */
    if (!st->seq_valid[ch]) {
        st->last_seq[ch] = seq;
        st->seq_valid[ch] = true;
        return PROTO_RX_EXECUTE;
    }
    /* 规则 1：同通道内 seq 相同 → 重复，只回 ACK */
    if (st->last_seq[ch] == seq) {
        return PROTO_RX_DUPLICATE;
    }
    st->last_seq[ch] = seq;
    return PROTO_RX_EXECUTE;
}

/* ------------------------------ 发送侧 ----------------------------------- */

void proto_tx_start(proto_tx_tracker_t *tx, uint8_t seq, uint32_t now_ms)
{
    if (tx == NULL) {
        return;
    }
    tx->state = PROTO_TX_WAITING;
    tx->seq = seq;
    tx->attempts = 1u;
    tx->deadline_ms = now_ms + PROTO_ACK_TIMEOUT_MS;
}

bool proto_tx_on_ack(proto_tx_tracker_t *tx, const proto_ack_msg_t *ack)
{
    if (tx == NULL || ack == NULL) {
        return false;
    }
    if (tx->state != PROTO_TX_WAITING) {
        return false; /* 已结束的命令，迟到的 ACK 一律忽略 */
    }
    /* 必须回显同一个 seq —— 否则是旧命令的迟到 ACK */
    if (ack->echo_seq != tx->seq) {
        return false;
    }
    tx->state = (ack->result == (uint8_t)PROTO_OK) ? PROTO_TX_DONE
                                                   : PROTO_TX_FAILED;
    return true;
}

proto_tx_state_t proto_tx_poll(proto_tx_tracker_t *tx, uint32_t now_ms,
                               bool *out_retransmit)
{
    if (out_retransmit != NULL) {
        *out_retransmit = false;
    }
    if (tx == NULL || tx->state != PROTO_TX_WAITING) {
        return (tx == NULL) ? PROTO_TX_IDLE : tx->state;
    }
    /* 注意用有符号比较，避免 now_ms 回绕时误判超时 */
    if ((int32_t)(now_ms - tx->deadline_ms) < 0) {
        return PROTO_TX_WAITING;
    }
    if (tx->attempts >= (1u + PROTO_ACK_MAX_RETRY)) {
        tx->state = PROTO_TX_FAILED; /* 首次 + 3 次重传 = 4 次发送后放弃 */
        return PROTO_TX_FAILED;
    }
    tx->attempts++;
    tx->deadline_ms = now_ms + PROTO_ACK_TIMEOUT_MS;
    if (out_retransmit != NULL) {
        *out_retransmit = true;
    }
    return PROTO_TX_WAITING;
}

/* ---------------------------- 事件风暴抑制 -------------------------------- */

bool proto_event_should_report(proto_event_throttle_t *th, uint32_t now_ms)
{
    if (th == NULL) {
        return false;
    }
    if (!th->active) {
        th->active = true;
        th->last_report_ms = now_ms;
        th->repeat_count = 0u;
        return true; /* 首次上报 */
    }
    if ((uint32_t)(now_ms - th->last_report_ms) < PROTO_EVENT_SUPPRESS_MS) {
        /* 抑制窗口内：只累加重复计数，饱和于 255（不回绕，避免"重复 0 次"歧义） */
        if (th->repeat_count < 255u) {
            th->repeat_count++;
        }
        return false;
    }
    /* 窗口已过：放行本次上报，并清零重复计数 —— 否则上一窗口的计数会被
     * 重复带到下一次上报里（"距上次上报的重复次数"语义就不成立了） */
    th->last_report_ms = now_ms;
    th->repeat_count = 0u;
    return true;
}

uint8_t proto_event_repeat_count(const proto_event_throttle_t *th)
{
    return (th == NULL) ? 0u : th->repeat_count;
}

/* ------------------------------ 链路监控 --------------------------------- */

void proto_link_reset(proto_link_monitor_t *m)
{
    if (m == NULL) {
        return;
    }
    m->last_rx_ms = 0u;
    m->ever_heard = false;
}

void proto_link_on_frame(proto_link_monitor_t *m, uint32_t now_ms)
{
    if (m == NULL) {
        return;
    }
    m->last_rx_ms = now_ms;
    m->ever_heard = true;
}

bool proto_link_is_offline(const proto_link_monitor_t *m, uint32_t now_ms)
{
    if (m == NULL || !m->ever_heard) {
        return false; /* 尚未通信过 ≠ 掉线，避免上电瞬间误报 */
    }
    return (uint32_t)(now_ms - m->last_rx_ms) > PROTO_OFFLINE_TIMEOUT_MS;
}
