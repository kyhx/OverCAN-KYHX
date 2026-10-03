/**
 * @file  test_seq.c
 * @brief 幂等状态机 / 重传跟踪 / 事件抑制 / 掉线检测 的回归测试。
 *
 * 本文件专门钉死两个**曾经真实存在的缺陷**（见 docs/protocol.md §7.2）：
 *   1. 广播与单播共用一个 last_seq → 回绕后撞号 → 命令静默丢失
 *   2. 缺少 seq_valid → 复位后主站发 seq=0 被误判重复而丢弃
 */
#define UTEST_MAIN
#define UTEST_SUITE_NAME "proto_seq"

#include "utest.h"

#include "proto_node.h"
#include "proto_seq.h"

UTEST_CASE(idempotent_basic)
{
    proto_rx_seq_state_t st;
    proto_rx_seq_reset(&st);

    /* 复位后第一个命令（seq=0）必须执行 —— 这正是 seq_valid 的意义 */
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 0u), PROTO_RX_EXECUTE);
    /* 同一 seq 重传 → 重复，不执行 */
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 0u), PROTO_RX_DUPLICATE);
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 0u), PROTO_RX_DUPLICATE);
    /* 新 seq → 执行 */
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 1u), PROTO_RX_EXECUTE);
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 1u), PROTO_RX_DUPLICATE);
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 2u), PROTO_RX_EXECUTE);

    /* 非命令帧不得占用 seq（否则会污染命令通道的判定） */
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_TELEMETRY_NODE1, 9u), PROTO_RX_REJECT);
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_ACK_NODE1, 9u), PROTO_RX_REJECT);
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_MASTER_HEARTBEAT, 9u), PROTO_RX_REJECT);
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 2u), PROTO_RX_DUPLICATE);
}

/** 缺陷①的回归测试：广播与单播必须各维护一个 last_seq。 */
UTEST_CASE(channel_isolation)
{
    proto_rx_seq_state_t st;
    proto_rx_seq_reset(&st);

    UTEST_EQ_INT(proto_seq_channel_of(PROTO_ID_CMD_BROADCAST), PROTO_CH_BROADCAST);
    UTEST_EQ_INT(proto_seq_channel_of(PROTO_ID_CMD_NODE1), PROTO_CH_UNICAST);
    UTEST_EQ_INT(proto_seq_channel_of(PROTO_ID_CMD_NODE2), PROTO_CH_UNICAST);

    /* 主站先广播 seq=7，再单播 seq=7。
     * 共用计数器的实现会在此把单播判为重复 —— 命令静默丢失。 */
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_BROADCAST, 7u), PROTO_RX_EXECUTE);
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 7u), PROTO_RX_EXECUTE);

    /* 各自的重传仍应正确判重 */
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_BROADCAST, 7u), PROTO_RX_DUPLICATE);
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 7u), PROTO_RX_DUPLICATE);

    /* 节点一与节点二共用单播通道（同一节点不会同时是两个节点号，安全） */
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE2, 8u), PROTO_RX_EXECUTE);
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 8u), PROTO_RX_DUPLICATE);
}

/** seq 每 256 条回绕：执行过的 seq 再次出现应被判为重复（这是设计取舍）。 */
UTEST_CASE(seq_wrap_256)
{
    proto_rx_seq_state_t st;
    proto_rx_seq_reset(&st);

    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 0u), PROTO_RX_EXECUTE);
    for (unsigned s = 1u; s <= 255u; ++s) {
        UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, (uint8_t)s),
                     PROTO_RX_EXECUTE);
    }
    /* 回绕到 0：距上次执行 0 已有 255 条命令，但判定只看"最近一次"，
     * 因此回绕后的 0 只与紧接着的前一条（255）比较 → 执行。 */
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 0u), PROTO_RX_EXECUTE);
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 0u), PROTO_RX_DUPLICATE);
}

/** 缺陷②的回归测试：复位后 seq_valid 必须为 false。 */
UTEST_CASE(reset_seq_valid)
{
    proto_rx_seq_state_t st;
    proto_rx_seq_reset(&st);
    UTEST_CHECK(!st.seq_valid[PROTO_CH_BROADCAST]);
    UTEST_CHECK(!st.seq_valid[PROTO_CH_UNICAST]);

    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 0u), PROTO_RX_EXECUTE);
    UTEST_CHECK(st.seq_valid[PROTO_CH_UNICAST]);
    UTEST_CHECK(!st.seq_valid[PROTO_CH_BROADCAST]); /* 另一通道不受影响 */

    /* 模拟软复位 / OTA 跳转 */
    proto_rx_seq_reset(&st);
    UTEST_CHECK(!st.seq_valid[PROTO_CH_UNICAST]);
    UTEST_EQ_INT(proto_rx_seq_handle(&st, PROTO_ID_CMD_NODE1, 0u), PROTO_RX_EXECUTE);
}

UTEST_CASE(tx_retransmit)
{
    proto_tx_tracker_t tx;
    proto_tx_start(&tx, 10u, 1000u);
    UTEST_EQ_INT(tx.state, PROTO_TX_WAITING);
    UTEST_EQ_INT(tx.attempts, 1u);

    bool retransmit = false;
    /* 未到 50 ms 窗口 → 不重传 */
    UTEST_EQ_INT(proto_tx_poll(&tx, 1049u, &retransmit), PROTO_TX_WAITING);
    UTEST_CHECK(!retransmit);
    /* 到点 → 重传 */
    UTEST_EQ_INT(proto_tx_poll(&tx, 1050u, &retransmit), PROTO_TX_WAITING);
    UTEST_CHECK(retransmit);
    UTEST_EQ_INT(tx.attempts, 2u);
    UTEST_CHECK(proto_tx_poll(&tx, 1100u, &retransmit) == PROTO_TX_WAITING);
    UTEST_CHECK(retransmit);
    UTEST_EQ_INT(tx.attempts, 3u);
    UTEST_CHECK(proto_tx_poll(&tx, 1150u, &retransmit) == PROTO_TX_WAITING);
    UTEST_CHECK(retransmit);
    UTEST_EQ_INT(tx.attempts, 4u); /* 首次 + 3 次重传 */
    /* 第 4 次发送仍无 ACK → 失败，不再重传 */
    UTEST_EQ_INT(proto_tx_poll(&tx, 1200u, &retransmit), PROTO_TX_FAILED);
    UTEST_CHECK(!retransmit);

    /* 成功后不再重传 */
    proto_tx_start(&tx, 11u, 2000u);
    const proto_ack_msg_t ok = { .echo_seq = 11u, .result = (uint8_t)PROTO_OK, .state = 0u };
    UTEST_CHECK(proto_tx_on_ack(&tx, &ok));
    UTEST_EQ_INT(tx.state, PROTO_TX_DONE);
    UTEST_EQ_INT(proto_tx_poll(&tx, 3000u, &retransmit), PROTO_TX_DONE);
    UTEST_CHECK(!retransmit);

    /* 设备回错误码 → 直接失败（无需继续重传，重传也不会成功） */
    proto_tx_start(&tx, 12u, 4000u);
    const proto_ack_msg_t err = { .echo_seq = 12u,
                                  .result = (uint8_t)PROTO_ERR_RANGE,
                                  .state = 0u };
    UTEST_CHECK(proto_tx_on_ack(&tx, &err));
    UTEST_EQ_INT(tx.state, PROTO_TX_FAILED);
}

/** 迟到的 ACK 不得确认当前在途命令。 */
UTEST_CASE(tx_late_ack)
{
    proto_tx_tracker_t tx;
    proto_tx_start(&tx, 20u, 0u);

    /* echo_seq 不匹配 → 不确认 */
    const proto_ack_msg_t stale = { .echo_seq = 19u, .result = (uint8_t)PROTO_OK, .state = 0u };
    UTEST_CHECK(!proto_tx_on_ack(&tx, &stale));
    UTEST_EQ_INT(tx.state, PROTO_TX_WAITING);
    UTEST_EQ_INT(tx.seq, 20u);

    /* 匹配 → 确认 */
    const proto_ack_msg_t good = { .echo_seq = 20u, .result = (uint8_t)PROTO_OK, .state = 0u };
    UTEST_CHECK(proto_tx_on_ack(&tx, &good));
    UTEST_EQ_INT(tx.state, PROTO_TX_DONE);

    /* 命令结束后迟到的 ACK 一律忽略 */
    UTEST_CHECK(!proto_tx_on_ack(&tx, &good));
}

UTEST_CASE(event_throttle)
{
    proto_event_throttle_t th = { 0u, false, 0u };

    /* 首报放行 */
    UTEST_CHECK(proto_event_should_report(&th, 1000u));
    /* 200 ms 窗口内抑制，并累计重复计数 */
    UTEST_CHECK(!proto_event_should_report(&th, 1050u));
    UTEST_CHECK(!proto_event_should_report(&th, 1100u));
    UTEST_CHECK(!proto_event_should_report(&th, 1199u));
    UTEST_EQ_INT(proto_event_repeat_count(&th), 3u);
    /* 到窗口边界 → 放行 */
    UTEST_CHECK(proto_event_should_report(&th, 1200u));
    /* 放行后计数归零，避免把上一窗口的重复数重复上报 */
    UTEST_EQ_INT(proto_event_repeat_count(&th), 0u);
    /* 第二个窗口：重复计数从 0 重新累计，不继承上一窗口 */
    UTEST_CHECK(!proto_event_should_report(&th, 1250u));
    UTEST_CHECK(!proto_event_should_report(&th, 1300u));
    UTEST_EQ_INT(proto_event_repeat_count(&th), 2u);
    UTEST_CHECK(proto_event_should_report(&th, 1400u));
    UTEST_EQ_INT(proto_event_repeat_count(&th), 0u);

    /* 计数饱和于 255，不回绕 */
    proto_event_throttle_t th2 = { 0u, false, 0u };
    UTEST_CHECK(proto_event_should_report(&th2, 0u));
    for (unsigned i = 0; i < 300u; ++i) {
        UTEST_CHECK(!proto_event_should_report(&th2, 1u));
    }
    UTEST_EQ_INT(proto_event_repeat_count(&th2), 255u);
}

UTEST_CASE(link_monitor)
{
    proto_link_monitor_t m;
    proto_link_reset(&m);

    /* 从未通信过不算掉线（避免上电瞬间误报） */
    UTEST_CHECK(!proto_link_is_offline(&m, 10000u));
    proto_link_on_frame(&m, 1000u);
    UTEST_CHECK(!proto_link_is_offline(&m, 3999u));
    /* 3 s 边界：等于窗口不算掉线，超过才算 */
    UTEST_CHECK(!proto_link_is_offline(&m, 4000u));
    UTEST_CHECK(proto_link_is_offline(&m, 4001u));
    proto_link_on_frame(&m, 4001u);
    UTEST_CHECK(!proto_link_is_offline(&m, 4001u));
}

/** 节点参考实现的端到端行为：执行 / 重复仍回 ACK / 非法拒收。 */
UTEST_CASE(node_handler)
{
    proto_node_ctx_t node;
    proto_node_init(&node, 1u);
    UTEST_EQ_INT(node.node, 1u);

    const proto_command_msg_t cmd = {
        .seq = 0u, /* 复位后主站从 0 开始 —— 曾经的丢弃场景 */
        .cmd = PROTO_CMD_SET,
        .device = PROTO_DEV_MOTOR,
        .channel = 0u,
        .param = 500, /* 50.0 % */
        .options = 0u,
    };
    proto_can_frame_t f;
    UTEST_EQ_INT(proto_encode_command(1u, &cmd, &f), PROTO_OK);

    proto_command_msg_t out_cmd;
    proto_ack_msg_t ack;
    UTEST_EQ_INT(proto_node_handle_command(&node, &f, &out_cmd, &ack), PROTO_NODE_EXECUTE);
    UTEST_EQ_INT(ack.echo_seq, 0u);
    UTEST_EQ_INT(ack.result, PROTO_OK);
    UTEST_EQ_INT(out_cmd.param, 500);

    /* 主站重传同一 seq（因为 ACK 丢了）→ 必须"只回 ACK 不执行" */
    UTEST_EQ_INT(proto_node_handle_command(&node, &f, &out_cmd, &ack), PROTO_NODE_DUPLICATE);
    UTEST_EQ_INT(ack.echo_seq, 0u);
    UTEST_EQ_INT(ack.result, PROTO_OK);

    /* 广播用相同 seq 也必须执行（通道分离） */
    proto_can_frame_t fb;
    UTEST_EQ_INT(proto_encode_command(0u, &cmd, &fb), PROTO_OK);
    UTEST_EQ_INT(proto_node_handle_command(&node, &fb, &out_cmd, &ack), PROTO_NODE_EXECUTE);

    /* 发往节点二的单播：节点一必须拒收 */
    proto_can_frame_t f2;
    UTEST_EQ_INT(proto_encode_command(2u, &cmd, &f2), PROTO_OK);
    UTEST_EQ_INT(proto_node_handle_command(&node, &f2, &out_cmd, &ack), PROTO_NODE_REJECT);
    UTEST_EQ_INT(ack.result, PROTO_ERR_NODE);

    /* 非法参数（舵机 200°）不能占用 seq：随后同一 seq 的合法命令仍应执行 */
    proto_can_frame_t bad;
    bad.id = PROTO_ID_CMD_NODE1;
    bad.dlc = 8u;
    memset(bad.data, 0, sizeof(bad.data));
    bad.data[0] = 1u; /* 新 seq */
    bad.data[1] = (uint8_t)PROTO_CMD_SET;
    bad.data[2] = (uint8_t)PROTO_DEV_SERVO;
    bad.data[4] = 200u;
    UTEST_EQ_INT(proto_node_handle_command(&node, &bad, &out_cmd, &ack), PROTO_NODE_REJECT);
    UTEST_EQ_INT(ack.result, PROTO_ERR_RANGE);

    proto_command_msg_t good = cmd;
    good.seq = 1u;
    good.device = PROTO_DEV_SERVO;
    good.param = 90;
    UTEST_EQ_INT(proto_encode_command(1u, &good, &f), PROTO_OK);
    UTEST_EQ_INT(proto_node_handle_command(&node, &f, &out_cmd, &ack), PROTO_NODE_EXECUTE);

    /* DLC 错误 → 拒收 */
    proto_can_frame_t shortf = f;
    shortf.dlc = 4u;
    UTEST_EQ_INT(proto_node_handle_command(&node, &shortf, &out_cmd, &ack), PROTO_NODE_REJECT);
    UTEST_EQ_INT(ack.result, PROTO_ERR_LEN);

    /* 复位（OTA 跳转）后第一个命令再次无条件执行 */
    proto_node_reset(&node);
    proto_command_msg_t after = cmd;
    after.seq = 0u;
    UTEST_EQ_INT(proto_encode_command(1u, &after, &f), PROTO_OK);
    UTEST_EQ_INT(proto_node_handle_command(&node, &f, &out_cmd, &ack), PROTO_NODE_EXECUTE);
}
