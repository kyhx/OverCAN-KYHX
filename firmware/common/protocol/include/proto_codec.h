/**
 * @file  proto_codec.h
 * @brief can_distributed 协议 v1.0 —— 编解码 API 与消息结构体。
 *
 * 逐字节布局见 docs/protocol.md §5；本文件是该布局的可执行形式。
 *
 * 设计约定（违反任何一条都会导致跨平台不一致）：
 *   - 多字节字段一律**小端**；有符号量一律**补码 int16**
 *   - **不使用 C 位域**，全部经 proto_bytes.h 手写移位掩码
 *   - 解码函数是**纯函数**：不修改任何全局状态、不分配内存
 *   - 解码顺序固定：DLC → ID → 取值范围（顺序不可颠倒）
 *   - 越界参数**拒绝**（PROTO_ERR_RANGE），**绝不静默截断**
 *
 * 纯 C99、零 HAL 依赖、无 malloc、无浮点。
 */
#ifndef PROTO_CODEC_H
#define PROTO_CODEC_H

#include <stdbool.h>
#include <stdint.h>

#include "proto_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* 错误码（全协议统一，与 docs/protocol.md §11 一致）                          */
/* ------------------------------------------------------------------------- */
typedef enum {
    PROTO_OK = 0,             /**< 成功                                   */
    PROTO_ERR_LEN = 1,        /**< DLC 与帧类型不符                       */
    PROTO_ERR_ID = 2,         /**< 帧 ID 不符合 3+4+4 规则或未定义        */
    PROTO_ERR_NODE = 3,       /**< 节点号不是本节点                       */
    PROTO_ERR_RANGE = 4,      /**< 参数越界（拒收不执行）                 */
    PROTO_ERR_VALUE = 5,      /**< 枚举值未定义                           */
    PROTO_ERR_STATE = 6,      /**< 当前状态不允许该操作                   */
    PROTO_ERR_BUSY = 7,       /**< 设备忙（如升级中）                     */
    PROTO_ERR_CRC = 8,        /**< CRC 校验失败（升级链路）               */
    PROTO_ERR_UNSUPPORTED = 9 /**< 功能未实现（如无电流采样时的堵转查询）  */
} proto_result_t;

static inline const char *proto_result_name(proto_result_t r)
{
    switch (r) {
    case PROTO_OK: return "OK";
    case PROTO_ERR_LEN: return "ERR_LEN";
    case PROTO_ERR_ID: return "ERR_ID";
    case PROTO_ERR_NODE: return "ERR_NODE";
    case PROTO_ERR_RANGE: return "ERR_RANGE";
    case PROTO_ERR_VALUE: return "ERR_VALUE";
    case PROTO_ERR_STATE: return "ERR_STATE";
    case PROTO_ERR_BUSY: return "ERR_BUSY";
    case PROTO_ERR_CRC: return "ERR_CRC";
    case PROTO_ERR_UNSUPPORTED: return "ERR_UNSUPPORTED";
    default: return "ERR_UNKNOWN";
    }
}

/* ------------------------------------------------------------------------- */
/* 枚举（附录 A）                                                            */
/* ------------------------------------------------------------------------- */

/** 命令字（命令帧 b1）。 */
typedef enum {
    PROTO_CMD_SET = 0x01,   /**< 设置          */
    PROTO_CMD_STOP = 0x02,  /**< 停止          */
    PROTO_CMD_RESET = 0x03, /**< 复位          */
    PROTO_CMD_QUERY = 0x04  /**< 查询          */
} proto_cmd_t;

/** 设备类别（命令帧 b2）。 */
typedef enum {
    PROTO_DEV_MOTOR = 0,   /**< 电机：param 0~1000（0.1% 分辨率） */
    PROTO_DEV_SERVO = 1,   /**< 舵机：param 0~180（度）           */
    PROTO_DEV_BUZZER = 2,  /**< 蜂鸣器：param 0~1000              */
    PROTO_DEV_SYSTEM = 3   /**< 系统：param 语义由 cmd 决定        */
} proto_device_t;

/** 事件码（事件帧 b0）。 */
typedef enum {
    PROTO_EV_OVER_TEMP = 0x01,    /**< 超温                       */
    PROTO_EV_TEMP_LIMIT = 0x02,   /**< 温度越限                   */
    PROTO_EV_IR_TRIGGER = 0x03,   /**< 红外触发                   */
    PROTO_EV_LIGHT_LIMIT = 0x04,  /**< 光照越限                   */
    PROTO_EV_STALL = 0x05,        /**< 堵转（**待电流采样硬件**）  */
    PROTO_EV_SENSOR_FAULT = 0x06  /**< 传感器故障                 */
} proto_event_code_t;

/** 事件等级（事件帧 b1）。 */
typedef enum {
    PROTO_LVL_INFO = 0,     /**< 提示 */
    PROTO_LVL_WARNING = 1,  /**< 警告 */
    PROTO_LVL_CRITICAL = 2  /**< 严重 */
} proto_event_level_t;

/** 电机状态（遥测 0x210 b5）。 */
typedef enum {
    PROTO_MOTOR_STOP = 0,   /**< 停               */
    PROTO_MOTOR_FORWARD = 1,/**< 正转             */
    PROTO_MOTOR_REVERSE = 2,/**< 反转             */
    PROTO_MOTOR_BRAKE = 3   /**< 制动             */
} proto_motor_state_t;

/* ------------------------------------------------------------------------- */
/* CAN 帧载体                                                                */
/* ------------------------------------------------------------------------- */
typedef struct {
    uint16_t id;  /**< 11 位标准帧 ID（不使用扩展帧） */
    uint8_t dlc;  /**< 数据场长度 0~8                 */
    uint8_t data[8];
} proto_can_frame_t;

/* ------------------------------------------------------------------------- */
/* 取值约束（解码期强校验；越界 → PROTO_ERR_RANGE）                            */
/* ------------------------------------------------------------------------- */
#define PROTO_MOTOR_DUTY_MIN 0
#define PROTO_MOTOR_DUTY_MAX 1000
#define PROTO_SERVO_ANGLE_MIN 0
#define PROTO_SERVO_ANGLE_MAX 180
#define PROTO_TELEM_DUTY_PCT_MAX 100
#define PROTO_PCT_MAX 100

static inline bool proto_motor_param_valid(int16_t v)
{
    return v >= PROTO_MOTOR_DUTY_MIN && v <= PROTO_MOTOR_DUTY_MAX;
}

static inline bool proto_servo_param_valid(int16_t v)
{
    return v >= PROTO_SERVO_ANGLE_MIN && v <= PROTO_SERVO_ANGLE_MAX;
}

/** 按设备类别校验命令参数；系统类别不限制范围。 */
static inline bool proto_param_valid_for(proto_device_t dev, int16_t v)
{
    switch (dev) {
    case PROTO_DEV_MOTOR:
    case PROTO_DEV_BUZZER:
        return proto_motor_param_valid(v);
    case PROTO_DEV_SERVO:
        return proto_servo_param_valid(v);
    case PROTO_DEV_SYSTEM:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------- */
/* 消息结构体                                                                */
/* ------------------------------------------------------------------------- */

/** 0x210 节点一遥测。 */
typedef struct {
    uint8_t seq;            /**< b0    遥测序号（丢帧统计用，不参与幂等） */
    uint8_t valid;          /**< b1    bit0=温度 bit1=DO bit2=电流(待硬件) */
    int16_t temperature;    /**< b2..b3 单位 0.1 ℃（有符号，负温有效）    */
    uint8_t do_state;       /**< b4    热敏 DO 状态                       */
    uint8_t motor_state;    /**< b5    proto_motor_state_t                */
    uint8_t motor_duty_pct; /**< b6    占空比 0~100 %                     */
    uint8_t status;         /**< b7    bit0=堵转(待硬件) bit1=过温 bit2=传感器故障 */
} proto_telemetry_node1_t;

/** 0x220 节点二遥测。 */
typedef struct {
    uint8_t seq;            /**< b0   遥测序号                     */
    uint8_t valid;          /**< b1   bit0=光照 bit1=红外DO bit2=红外AO */
    uint16_t light_lux;     /**< b2..b3 单位 lux                   */
    uint8_t ir_do;          /**< b4   红外数字量                   */
    uint8_t servo_angle;    /**< b5   0~180 度                     */
    uint8_t ir_ao_pct;      /**< b6   红外模拟量 0~100 %           */
    uint8_t status;         /**< b7   **待定义**：当前发送 0、接收忽略 */
} proto_telemetry_node2_t;

/** 0x011 / 0x021 事件帧。 */
typedef struct {
    uint8_t event_code;     /**< b0    proto_event_code_t                  */
    uint8_t level;          /**< b1    proto_event_level_t                 */
    int16_t value;          /**< b2..b3 触发值，单位按事件码解释            */
    uint8_t channel;        /**< b4    相关通道                            */
    uint16_t uptime_s;      /**< b5..b6 节点运行秒数低 16 位（事件排序用）   */
    uint8_t repeat_count;   /**< b7    重复计数，饱和于 255（风暴抑制配套）  */
} proto_event_msg_t;

/** 0x100 / 0x110 / 0x120 命令帧。 */
typedef struct {
    uint8_t seq;      /**< b0    命令序号（按通道独立计数，ACK 回显） */
    uint8_t cmd;      /**< b1    proto_cmd_t                          */
    uint8_t device;   /**< b2    proto_device_t                       */
    uint8_t channel;  /**< b3    通道 / 子索引                        */
    int16_t param;    /**< b4..b5 参数（范围按设备类别校验）           */
    uint8_t options;  /**< b6    选项位（当前未定义，置 0）            */
} proto_command_msg_t;

/** 0x310 / 0x320 ACK 帧。 */
typedef struct {
    uint8_t echo_seq; /**< b0    回显的命令 seq              */
    uint8_t result;   /**< b1    proto_result_t             */
    uint8_t state;    /**< b2    执行后状态                 */
} proto_ack_msg_t;

/** 0x300 主站心跳。 */
typedef struct {
    uint8_t heartbeat_count;      /**< b0    心跳计数，回绕              */
    uint8_t system_mode;          /**< b1    系统模式字（未定义，置 0）   */
    uint16_t online_bitmap;       /**< b2..b3 **bit N = 节点 N 在线**    */
    uint8_t fw_version_uniform;   /**< b4    0=一致，非 0=存在不一致      */
    uint8_t protocol_version;     /**< b5    协议版本（≠ 固件版本）       */
} proto_heartbeat_msg_t;

/** 在线位图辅助：节点号 n（1~15）→ 位掩码。 */
static inline uint16_t proto_node_bit(uint8_t node)
{
    return (node >= 1u && node <= PROTO_NODE_MAX) ? (uint16_t)(1u << node) : 0u;
}

/** 判断心跳位图中某节点是否在线（节点号 0 或 >15 一律 false）。 */
static inline bool proto_heartbeat_node_online(const proto_heartbeat_msg_t *hb,
                                               uint8_t node)
{
    const uint16_t bit = proto_node_bit(node);
    return (hb != NULL) && (bit != 0u) && ((hb->online_bitmap & bit) != 0u);
}

/* ------------------------------------------------------------------------- */
/* 编码：参数结构体 → CAN 帧                                                  */
/* ------------------------------------------------------------------------- */

/** 0x210 节点一遥测；motor_duty_pct > 100 或 motor_state 非法 → ERR_RANGE/VALUE。 */
proto_result_t proto_encode_telemetry_node1(const proto_telemetry_node1_t *m,
                                            proto_can_frame_t *out);
/** 0x220 节点二遥测；servo_angle > 180 或 ir_ao_pct > 100 → 错误。 */
proto_result_t proto_encode_telemetry_node2(const proto_telemetry_node2_t *m,
                                            proto_can_frame_t *out);

/** 事件帧。@param node 事件来源节点（1~15），决定 ID 为 0x011 / 0x021 … */
proto_result_t proto_encode_event(uint8_t node, const proto_event_msg_t *m,
                                  proto_can_frame_t *out);

/** 命令帧。@param node 0 = 广播(0x100)，1/2 = 单播(0x110/0x120)。 */
proto_result_t proto_encode_command(uint8_t node, const proto_command_msg_t *m,
                                    proto_can_frame_t *out);

/** ACK 帧。@param node 回 ACK 的节点（1~15）。 */
proto_result_t proto_encode_ack(uint8_t node, const proto_ack_msg_t *m,
                                proto_can_frame_t *out);

/** 主站心跳。 */
proto_result_t proto_encode_heartbeat(const proto_heartbeat_msg_t *m,
                                      proto_can_frame_t *out);

/* ------------------------------------------------------------------------- */
/* 解码：CAN 帧 → 参数结构体                                                  */
/* 所有解码函数都要求帧 ID 与函数语义匹配（ID 不符 → PROTO_ERR_ID）。           */
/* ------------------------------------------------------------------------- */

proto_result_t proto_decode_telemetry_node1(const proto_can_frame_t *f,
                                            proto_telemetry_node1_t *out);
proto_result_t proto_decode_telemetry_node2(const proto_can_frame_t *f,
                                            proto_telemetry_node2_t *out);

/** 解码事件帧（0x011 / 0x021）。 */
proto_result_t proto_decode_event(const proto_can_frame_t *f,
                                  proto_event_msg_t *out);

/**
 * 解码命令帧，并做**节点过滤 + 参数范围校验**。
 * @param expect_node 本节点号（1~15）。广播 0x100 与本节点单播均被接受；
 *                    发往其它节点的单播 → PROTO_ERR_NODE。
 */
proto_result_t proto_decode_command(const proto_can_frame_t *f,
                                    uint8_t expect_node,
                                    proto_command_msg_t *out);

/** 解码 ACK 帧（0x310 / 0x320）。 */
proto_result_t proto_decode_ack(const proto_can_frame_t *f,
                                proto_ack_msg_t *out);

/** 解码主站心跳（0x300）。 */
proto_result_t proto_decode_heartbeat(const proto_can_frame_t *f,
                                      proto_heartbeat_msg_t *out);

/**
 * 按 ID 分派解码到调用方提供的联合体。便于节点/主站只写一次分派逻辑。
 */
typedef struct {
    uint16_t id;
    union {
        proto_telemetry_node1_t telem1;
        proto_telemetry_node2_t telem2;
        proto_event_msg_t event;
        proto_command_msg_t command;
        proto_ack_msg_t ack;
        proto_heartbeat_msg_t heartbeat;
    } u;
} proto_decoded_t;

/** @param expect_node 本节点号（接收视角）或主站视角传 PROTO_NODE_MASTER。 */
proto_result_t proto_decode_dispatch(const proto_can_frame_t *f,
                                     uint8_t expect_node,
                                     proto_decoded_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_CODEC_H */
