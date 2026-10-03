/**
 * @file  proto_node.h
 * @brief 节点侧命令处理参考实现：把幂等状态机 + 参数校验 + ACK 组装串起来。
 *
 * 这是"协议库能直接用"的证明，也是节点固件 main 循环的骨架：
 *
 *     收到 CAN 帧 → proto_node_handle_command() → 得到处置结论
 *                       ├─ EXECUTE   → 执行动作，然后发 *out_ack
 *                       ├─ DUPLICATE → 不执行动作，直接发 *out_ack
 *                       └─ REJECT    → 不占用 seq，发 *out_ack（携带错误码）
 *
 * 注意职责边界：**协议库不碰硬件**。真正的电机/舵机动作由调用方在 EXECUTE
 * 分支里完成；本函数只做"该不该执行"的判定与报文应答。
 *
 * 纯 C99、零 HAL 依赖、无 malloc。
 */
#ifndef PROTO_NODE_H
#define PROTO_NODE_H

#include <stdint.h>

#include "proto_codec.h"
#include "proto_seq.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /** 首次收到该 seq：调用方**应执行**动作，并发送 *out_ack。 */
    PROTO_NODE_EXECUTE = 0,
    /** 重复命令：**不得执行**动作，但仍应重发 *out_ack（否则主站会一直重传）。 */
    PROTO_NODE_DUPLICATE,
    /** 报文非法（DLC/枚举/范围/非本节点）：不占用 seq，发送携带错误码的 *out_ack。 */
    PROTO_NODE_REJECT
} proto_node_result_t;

/** 节点上下文：每个节点一份。 */
typedef struct {
    uint8_t node;                 /**< 本节点号 1~15                      */
    uint8_t last_ack_state;       /**< 最近一次执行后的设备状态（填 ACK b2） */
    proto_rx_seq_state_t rx_seq;  /**< 幂等状态（**广播与单播各一份内部状态**） */
} proto_node_ctx_t;

/** 初始化：置节点号、清幂等状态（seq_valid = false）。 */
void proto_node_init(proto_node_ctx_t *ctx, uint8_t node);

/** 复位幂等状态（软复位 / OTA 跳转后必须调用）。 */
void proto_node_reset(proto_node_ctx_t *ctx);

/**
 * 处理一条命令帧。
 * @param ctx      节点上下文
 * @param frame    收到的 CAN 帧
 * @param out_cmd  解码后的命令（REJECT 时内容未定义，勿使用）
 * @param out_ack  应答帧（三种结论下都应发送）
 * @return 处置结论
 */
proto_node_result_t proto_node_handle_command(proto_node_ctx_t *ctx,
                                              const proto_can_frame_t *frame,
                                              proto_command_msg_t *out_cmd,
                                              proto_ack_msg_t *out_ack);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_NODE_H */
