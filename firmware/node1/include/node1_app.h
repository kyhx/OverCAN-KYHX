/**
 * @file    node1_app.h
 * @brief 节点一**业务逻辑**（零 HAL 依赖，PC 可单测）。
 *
 * 这是本项目分层原则在节点侧的落地：
 *     应用层（本文件：该不该执行、执行什么、上报什么）
 *         ↓ 依赖
 *     协议库（proto_*：幂等 / 编解码 / 风暴抑制，纯 C 已验证）
 *         ↓ 通过
 *     node1_hal_t vtable（唯一接触硬件的地方）
 *         ↓
 *     stm32f103 port（bxCAN / ADC / TIM1 / GPIO）
 *
 * **本文件不碰任何寄存器、不 include 任何 HAL 头、不用浮点以外的数学库**，
 * 因此 `tests/test_node_app.c` 能在 PC 上注入 mock HAL 跑真测试。
 *
 * 职责划分（务必遵守，否则单测失效）：
 *   - 本层：决定"该不该执行动作""该回什么 ACK""该报什么事件"
 *   - HAL ：决定"寄存器怎么写"
 *   例：重复命令本层返回 DUPLICATE，**绝不**去调 motor_set()。
 */
#ifndef NODE1_APP_H
#define NODE1_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "node1_hal.h"
#include "proto_codec.h"
#include "proto_node.h"
#include "proto_seq.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 遥测 status 位定义（docs/protocol.md §5.2，对应 0x210 的 b7）。 */
#define NODE1_STATUS_STALL_BIT      0u /**< bit0 堵转 —— **待电流采样硬件**，恒 0 */
#define NODE1_STATUS_OVERTEMP_BIT   1u /**< bit1 过温                       */
#define NODE1_STATUS_SENSOR_FAULT_BIT 2u /**< bit2 传感器故障               */

/** 遥测 valid 位定义（对应 0x210 的 b1）。 */
#define NODE1_VALID_TEMP_BIT  0u /**< bit0 温度有效                    */
#define NODE1_VALID_DO_BIT    1u /**< bit1 DO 有效                     */
#define NODE1_VALID_CURRENT_BIT 2u /**< bit2 电流有效 —— **无采样硬件**，恒 0 */

/** 节点一应用上下文。常驻 RAM，不动态分配。 */
typedef struct {
    /* --- 依赖注入 ------------------------------------------------- */
    const node1_hal_t *hal;   /**< 硬件接口；NULL 时仅跑协议逻辑（可纯逻辑单测） */
    uint32_t node;            /**< 本节点号，必须为 1~15                    */

    /* --- 协议状态 ------------------------------------------------- */
    proto_node_ctx_t   node_ctx;      /**< 幂等状态（广播/单播各一份）        */
    /** 事件风暴抑制闸门：**每个事件码一个**。共用一个会让高优先级事件
     *  在同一次心跳里把窗口耗尽，低优先级事件被饿死（真实踩过）。 */
    proto_event_throttle_t throttle_overtemp;
    proto_event_throttle_t throttle_sensor;
    proto_link_monitor_t master_link;  /**< 主站存活监控 → 掉线安全态          */
    uint8_t telem_seq;                 /**< 遥测序号，用于主站侧丢帧统计      */

    /* --- 业务状态 ------------------------------------------------- */
    int16_t  temperature_c10;  /**< 温度，0.1℃（有符号，负温有效）            */
    uint8_t  do_state;         /**< 热敏 DO                                    */
    uint8_t  status_bits;      /**< NODE1_STATUS_*                              */
    uint8_t  valid_bits;       /**< NODE1_VALID_*                               */
    bool     overtemp_active;  /**< 过温状态（带滞回）                          */
    bool     sensor_fault;     /**< 传感器故障                                  */

    node1_motor_action_t motor_action; /**< 当前电机动作                    */
    uint8_t              motor_duty;   /**< 当前占空比 0~100                */
    bool                 safe_state;   /**< 是否处于掉线安全态              */
    bool                 pending_soft_reset; /**< 收到 RESET 命令，待调度器执行  */

    /* --- 统计（可用 RTT 打印，面试可讲"跑过"而不是"想清楚"）-------- */
    uint32_t stat_cmd_exec;      /**< 实际执行的动作数（不含重复）            */
    uint32_t stat_cmd_dup;       /**< 幂等拦下的重复命令数                    */
    uint32_t stat_cmd_reject;    /**< 非法命令数                              */
    uint32_t stat_event_sent;    /**< 事件上报数                              */
    uint32_t stat_event_suppressed; /**< 被风暴抑制的事件数                  */
} node1_app_t;

/* ------------------------------------------------------------------------- */
/* 生命周期                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * 初始化。
 * @param hal 硬件接口，可为 NULL（此时只跑协议逻辑，用于 PC 逻辑单测）
 * @param now_ms 当前时间戳
 *
 * 关键：内部调用 proto_node_init()，它会把 seq_valid 置 false ——
 * 这是"上电后首个命令无条件执行"铁律的前提，**不可省略**。
 */
void node1_app_init(node1_app_t *app, const node1_hal_t *hal, uint32_t now_ms);

/**
 * 软复位 / OTA 跳转后调用：清幂等状态与安全态标志。
 * 不清会怎样：跳转后主站可能重发复位前的 seq，被误判重复而丢弃。
 */
void node1_app_soft_reset(node1_app_t *app, uint32_t now_ms);

/* ------------------------------------------------------------------------- */
/* 任务周期函数（由调度器按 node1_config.h 的周期调用）                       */
/* ------------------------------------------------------------------------- */

/** Sensor 任务（100ms）：ADC 采样 + 滤波 + 过温判定 + 传感器故障判定。
 *  过温时置事件标志，由 Heartbeat/TX 任务上报——**不在这里直接发帧**，
 *  因为传感器任务是中优先级，不该碰总线。 */
void node1_task_sensor(node1_app_t *app, uint32_t now_ms);

/** Heartbeat 任务（1s）：推进主站掉线判定 → 安全态；上报待发事件。
 *  事件上报是"低频但紧急"，允许在本任务里发，但**仅在有新事件时**。 */
void node1_task_heartbeat(node1_app_t *app, uint32_t now_ms);

/** Telemetry 任务（1s）：组装并发送 0x210 遥测。 */
void node1_task_telemetry(node1_app_t *app, uint32_t now_ms);

/** 处理一帧收到的 CAN 数据。返回 true 表示**确实执行了动作**。
 *
 * 三种处置（与 proto_node_result_t 一一对应）：
 *   EXECUTE   → 执行动作 + 发 ACK(OK)
 *   DUPLICATE → 不执行，**但仍发 ACK(OK)**（否则主站会一直重传）
 *   REJECT    → 不执行，发 ACK(携带错误码)
 *
 * ⚠️ 本函数是幂等逻辑的**唯一执行点**。任何绕过它直接操作电机的代码，
 *    都会让"重复命令只回 ACK 不执行"这条铁律失效。
 */
bool node1_on_can_frame(node1_app_t *app, const proto_can_frame_t *frame,
                        uint32_t now_ms);

/* ------------------------------------------------------------------------- */
/* 温标换算（可被单测直接调用验证）                                            */
/* ------------------------------------------------------------------------- */

/**
 * ADC 原始值 → 温度（0.1℃）。
 * @return 有符号 0.1℃ 值；输入越界时返回 NODE1_ADC_FAULT_TEMP_C10
 *
 * 模型：NTC 分压 + Beta 方程（node1_config.h 里有全部系数）。
 * ⚠️ 系数是**待实测标定**的占位值，个体差异大，别当成精确值。
 */
#define NODE1_ADC_FAULT_TEMP_C10 (-32768)
int16_t node1_temp_from_raw(uint16_t raw);

/** 温度（0.1℃）→ ADC 原始值。**仅用于单测验证换算是单调且可逆的**，
 *  以及调试时反查"当前读数对应多少 ADC 值"。 */
uint16_t node1_temp_to_raw(int16_t c10);

#ifdef __cplusplus
}
#endif

#endif /* NODE1_APP_H */
