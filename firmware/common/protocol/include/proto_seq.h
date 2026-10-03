/**
 * @file  proto_seq.h
 * @brief 命令序号（seq）语义：接收侧幂等状态机 + 发送侧 ACK 重传状态机。
 *
 * 这一模块承载本项目**最容易写错、代价最大**的两条规则
 * （见 docs/protocol.md §7.2、[项目文档.md](../docs/项目文档.md) §5.3）：
 *
 *   1. **按通道分离 last_seq** —— 广播(0x100) 与单播(0x110/0x120) 各一个计数器。
 *      共用一个时，8 位 seq 每 256 条回绕，广播与单播必然撞号 → 节点判为重复 →
 *      只回 ACK 不执行 → 主站收到 ACK 却什么都没发生（**静默丢命令**）。
 *
 *   2. **seq_valid 标志** —— 上电 / 软复位 / OTA 跳转后 last_seq 无意义（默认 0）。
 *      此时若主站恰好发 seq=0，会被误判为重复而丢弃。因此第一个命令必须
 *      **无条件执行**，之后才启用重复判定。
 *
 * 纯 C99、零 HAL 依赖、无 malloc。
 */
#ifndef PROTO_SEQ_H
#define PROTO_SEQ_H

#include <stdbool.h>
#include <stdint.h>

#include "proto_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------ 时序参数 --------------------------------- */

/** ACK 等待窗口（ms）：超时即重传。 */
#define PROTO_ACK_TIMEOUT_MS 50u
/** 最大重传次数（首次发送之外）：最坏情况共发送 1 + 3 次。 */
#define PROTO_ACK_MAX_RETRY 3u
/** 掉线判定：3 s 内未收到某节点任何帧。 */
#define PROTO_OFFLINE_TIMEOUT_MS 3000u
/** 事件风暴抑制窗口：同一事件码最小上报间隔（ms）。 */
#define PROTO_EVENT_SUPPRESS_MS 200u
/** 主站心跳周期（ms）。 */
#define PROTO_HEARTBEAT_PERIOD_MS 1000u
/** 遥测周期（ms）。 */
#define PROTO_TELEMETRY_PERIOD_MS 1000u

/* ======================================================================== */
/* 接收侧：幂等状态机                                                        */
/* ======================================================================== */

/** 命令通道：广播与单播**必须**分别持有独立状态。 */
typedef enum {
    PROTO_CH_BROADCAST = 0, /**< 0x100 */
    PROTO_CH_UNICAST = 1,   /**< 0x110 / 0x120 */
    PROTO_CH_COUNT = 2
} proto_seq_channel_t;

/** 接收侧幂等状态（每个节点一份；可放入 RAM 常驻结构体）。 */
typedef struct {
    uint8_t last_seq[PROTO_CH_COUNT];      /**< 各通道最近一次已执行的 seq */
    bool seq_valid[PROTO_CH_COUNT];        /**< 复位后为 false，首个命令无条件生效 */
} proto_rx_seq_state_t;

/** 判定结果。 */
typedef enum {
    PROTO_RX_EXECUTE = 0, /**< 首次收到：**执行**动作，并回 ACK(OK)   */
    PROTO_RX_DUPLICATE,   /**< 重复 seq：**不执行**，只重发 ACK(OK)   */
    PROTO_RX_REJECT       /**< 参数非法（未占用 seq）                 */
} proto_rx_disposition_t;

/** 上电 / 软复位 / OTA 跳转后必须调用：清 last_seq 并置 seq_valid = false。 */
void proto_rx_seq_reset(proto_rx_seq_state_t *st);

/** 通道映射：由命令帧 ID 推断通道。 */
proto_seq_channel_t proto_seq_channel_of(uint16_t frame_id);

/**
 * 处理一条命令，返回应当如何处置。
 *
 * 语义（不可简化）：
 *   - !seq_valid[ch]  → EXECUTE（无条件执行）并记录 seq、置 valid
 *   - seq == last_seq → DUPLICATE（只回 ACK）
 *   - 其它            → EXECUTE 并记录 seq
 *
 * @note 本函数**只更新幂等状态**，不执行任何硬件动作；动作由调用方在
 *       返回 EXECUTE 时完成，保证"状态更新"与"动作执行"可分别测试。
 */
proto_rx_disposition_t proto_rx_seq_handle(proto_rx_seq_state_t *st,
                                           uint16_t frame_id, uint8_t seq);

/* ======================================================================== */
/* 发送侧：ACK 重传状态机                                                    */
/* ======================================================================== */

typedef enum {
    PROTO_TX_IDLE = 0,  /**< 无在途命令                     */
    PROTO_TX_WAITING,   /**< 已发送，等待 ACK                */
    PROTO_TX_DONE,      /**< 已确认成功                     */
    PROTO_TX_FAILED     /**< 重传用尽仍未确认 → 记故障        */
} proto_tx_state_t;

/** 发送侧单条命令的重传跟踪器。 */
typedef struct {
    proto_tx_state_t state;
    uint8_t seq;             /**< 在途命令的 seq（ACK 必须回显它才算成功） */
    uint8_t attempts;        /**< 已发送次数（含首次）                     */
    uint32_t deadline_ms;    /**< 下一次超时的时刻                         */
} proto_tx_tracker_t;

/** 发起一条命令：记录 seq、attempts = 1、deadline = now + PROTO_ACK_TIMEOUT_MS。 */
void proto_tx_start(proto_tx_tracker_t *tx, uint8_t seq, uint32_t now_ms);

/**
 * 收到一个 ACK。
 * 只有当 **ACK 的 echo_seq == 在途 seq** 时才判为成功 ——
 * 迟到的旧 ACK（对应已被判失败的旧命令）不得误判为当前命令成功。
 * @return true 表示本次 ACK 确实确认了在途命令。
 */
bool proto_tx_on_ack(proto_tx_tracker_t *tx, const proto_ack_msg_t *ack);

/**
 * 推进时钟。到点则要求重传。
 * @param out_retransmit 置 true 表示调用方应立即重发同一 seq
 * @return 跟踪器当前状态
 */
proto_tx_state_t proto_tx_poll(proto_tx_tracker_t *tx, uint32_t now_ms,
                               bool *out_retransmit);

/* ======================================================================== */
/* 事件风暴抑制（最高优先级帧的速率限制）                                      */
/* ======================================================================== */

/** 单事件码的抑制状态。 */
typedef struct {
    uint32_t last_report_ms;
    bool active;       /**< 是否已有抑制窗口内的累积 */
    uint8_t repeat_count; /**< 饱和于 255 */
} proto_event_throttle_t;

/**
 * 判定某个事件码现在是否允许上报。
 * @return true = 允许发送（并重置窗口）；false = 被抑制（仅累加 repeat_count）
 */
bool proto_event_should_report(proto_event_throttle_t *th, uint32_t now_ms);

/** 取当前累积的重复次数（发送时填入事件帧 b7）。 */
uint8_t proto_event_repeat_count(const proto_event_throttle_t *th);

/* ======================================================================== */
/* 健康监控：主站存活 / 节点掉线                                              */
/* ======================================================================== */

typedef struct {
    uint32_t last_rx_ms;
    bool ever_heard;
} proto_link_monitor_t;

void proto_link_reset(proto_link_monitor_t *m);

/** 收到该来源的任何一帧时调用。 */
void proto_link_on_frame(proto_link_monitor_t *m, uint32_t now_ms);

/** 是否已判定掉线（3 s 无帧；从未听到过则返回 false，避免上电即告警）。 */
bool proto_link_is_offline(const proto_link_monitor_t *m, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_SEQ_H */
