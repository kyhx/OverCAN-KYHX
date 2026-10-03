/**
 * @file  proto_node.c
 * @brief 节点侧命令处理参考实现。
 */
#include "proto_node.h"

#include <string.h>

void proto_node_init(proto_node_ctx_t *ctx, uint8_t node)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->node = node;
    proto_node_reset(ctx);
}

void proto_node_reset(proto_node_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    /* 复位后 last_seq 无意义 → seq_valid = false，第一个命令无条件执行 */
    proto_rx_seq_reset(&ctx->rx_seq);
}

/** 组装错误应答：result 用协议错误码（非 0）。 */
static void fill_error_ack(proto_ack_msg_t *out_ack, uint8_t seq,
                           proto_result_t err, uint8_t state)
{
    if (out_ack == NULL) {
        return;
    }
    out_ack->echo_seq = seq;
    out_ack->result = (uint8_t)err;
    out_ack->state = state;
}

proto_node_result_t proto_node_handle_command(proto_node_ctx_t *ctx,
                                              const proto_can_frame_t *frame,
                                              proto_command_msg_t *out_cmd,
                                              proto_ack_msg_t *out_ack)
{
    if (ctx == NULL || frame == NULL || out_cmd == NULL || out_ack == NULL) {
        return PROTO_NODE_REJECT;
    }

    /* 先解码（内含 DLC → ID → 范围的固定校验顺序） */
    const proto_result_t dec =
        proto_decode_command(frame, ctx->node, out_cmd);

    /* 无法从报文中取出 seq（ID/DLC 错误）→ 只能回 echo_seq = 0 的错误 ACK。
     * 这类帧本来也不该由本节点收到（硬件验收滤波器是第一道防线）。 */
    if (dec == PROTO_ERR_LEN || dec == PROTO_ERR_ID) {
        fill_error_ack(out_ack, 0u, dec, ctx->last_ack_state);
        return PROTO_NODE_REJECT;
    }

    /* 到这里 seq 一定可用（ID 与 DLC 均已通过） */
    const uint8_t seq = frame->data[0];

    if (dec == PROTO_ERR_NODE) {
        /* 发往其它节点的单播：不应答（应答会干扰目标节点的主站匹配） */
        fill_error_ack(out_ack, seq, dec, ctx->last_ack_state);
        return PROTO_NODE_REJECT;
    }
    if (dec != PROTO_OK) {
        /* 参数越界 / 枚举非法：**不执行**，且**不占用 seq**（避免主站重发被误判重复） */
        fill_error_ack(out_ack, seq, dec, ctx->last_ack_state);
        return PROTO_NODE_REJECT;
    }

    /* 幂等判定：广播与单播各自独立 */
    const proto_rx_disposition_t disp =
        proto_rx_seq_handle(&ctx->rx_seq, frame->id, seq);

    if (disp == PROTO_RX_DUPLICATE) {
        /* 重复命令：不执行动作，但仍要回 ACK，否则主站会一直重传 */
        out_ack->echo_seq = seq;
        out_ack->result = (uint8_t)PROTO_OK;
        out_ack->state = ctx->last_ack_state;
        return PROTO_NODE_DUPLICATE;
    }
    if (disp == PROTO_RX_REJECT) {
        fill_error_ack(out_ack, seq, PROTO_ERR_STATE, ctx->last_ack_state);
        return PROTO_NODE_REJECT;
    }

    /* 首次（或新 seq）：交给调用方执行动作。这里先给成功应答，
     * 调用方可按执行结果改写 out_ack->result / state。 */
    out_ack->echo_seq = seq;
    out_ack->result = (uint8_t)PROTO_OK;
    out_ack->state = ctx->last_ack_state;
    return PROTO_NODE_EXECUTE;
}
