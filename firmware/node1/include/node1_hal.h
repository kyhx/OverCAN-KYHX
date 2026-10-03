/**
 * @file    node1_hal.h
 * @brief 节点一硬件抽象层：**vtable 形式**的 HAL 接口。
 *
 * 为什么用 vtable 而不是一堆 extern 函数：
 *   1. **业务逻辑可 PC 单测**。本项目的协议库已经做到"零 HAL 可测"，
 *      同理把节点业务（命令执行、遥测组装、事件判定、安全态）也做成
 *      不依赖 stm32f1xx_hal.h，就能在 PC 上跑真测试，而不是"看起来对"。
 *   2. 换 MCU（ESP32 / 换 F4）时只需换一个 port 目录，业务代码零改动。
 *   3. 依赖注入显式化：谁在用哪套硬件一目了然，比全局 HAL 状态更好读。
 *
 * 代价：多一层间接调用。CAN 周期 1kHz、命令帧最坏 20Hz，
 * 间接调用的开销远小于 CAN 收发器本身的传播延迟，不构成瓶颈。
 *
 * 约定：HAL 层的函数**不检查入参**（内部调用方已保证），
 *       但**不得阻塞**（除明确的 delay_ms）、**不得分配内存**。
 */
#ifndef NODE1_HAL_H
#define NODE1_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "proto_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 电机通道（TB6612 双通道）。 */
typedef enum {
    NODE1_MOTOR_A = 0,
    NODE1_MOTOR_B = 1
} node1_motor_id_t;

/** 电机动作。语义与 proto_motor_state_t 对齐，但驱动层用自己的名字
 *  以免把"总线协议枚举"和"硬件动作"耦合在一起。 */
typedef enum {
    NODE1_MOTOR_ACTION_STOP = 0,  /**< 停转：IN1=0 IN2=0（惰行）      */
    NODE1_MOTOR_ACTION_FORWARD,   /**< 正转：IN1=1 IN2=0             */
    NODE1_MOTOR_ACTION_REVERSE,   /**< 反转：IN1=0 IN2=1             */
    NODE1_MOTOR_ACTION_BRAKE      /**< 制动：IN1=1 IN2=1（短路刹车）  */
} node1_motor_action_t;

/** ADC 原始读数（0~4095，12 位）。HAL 只给原始值，**不做工程量换算**——
 *  换算系数属可标定参数，必须留在业务层（可被单测覆盖），不能埋进驱动。 */
typedef struct {
    uint16_t therm_raw;  /**< 热敏 AO（ADC1_IN0）原始值 */
    bool      therm_do;  /**< 热敏 DO 数字量              */
} node1_sensor_raw_t;

/* ------------------------------------------------------------------------- */
/* HAL vtable                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief 全部硬件操作。每个函数指针都可能为 NULL ——业务层必须判空后跳过，
 *        这样"只跑部分功能"的板卡（如无电机调试板）也能编译运行。
 */
typedef struct {
    /* --- 时基 ------------------------------------------------------- */
    /** 毫秒单调时钟。溢出（49 天）必须正确回绕：内部用 uint32_t 自然溢出即可。 */
    uint32_t (*millis)(void);
    /** 忙等延时。**仅允许在初始化阶段使用**，任务循环里禁止。 */
    void     (*delay_ms)(uint32_t ms);

    /* --- 传感器 ----------------------------------------------------- */
    /** 读传感器原始值。阻塞直到转换完成（单次转换仅 ~13μs，可接受）。 */
    bool     (*sensor_read)(node1_sensor_raw_t *out);

    /* --- 执行器 ----------------------------------------------------- */
    /** 设置某通道电机动作与占空比（0~100%）。 */
    void     (*motor_set)(node1_motor_id_t id, node1_motor_action_t action,
                          uint8_t duty_pct);
    /** 驱动器使能（STBY 脚）。false = 待机高阻，**安全态必须置 false**。 */
    void     (*motor_enable)(bool on);
    /** 蜂鸣器开关。已按低电平有效处理，调用方只表达意图。 */
    void     (*buzzer_set)(bool on);

    /* --- CAN -------------------------------------------------------- */
    /** 发送一帧。返回 false = 邮箱满或总线关闭；**调用方必须检查**，
     *  不得假设一定成功（丢 ACK 会触发主站重传，丢遥测则只是少一个点）。 */
    bool     (*can_send)(const proto_can_frame_t *frame);
    /** 尝试取一帧。返回 true 且填入 *out 时表示取到。
     *  实现须在中断里入队、此处只出队（任务与中断职责分离）。 */
    bool     (*can_recv)(proto_can_frame_t *out);
} node1_hal_t;

/** 便捷判空宏：让业务层读起来不啰嗦。 */
#define NODE1_HAL_OK(hal, member) ((hal) != NULL && (hal)->member != NULL)

#ifdef __cplusplus
}
#endif

#endif /* NODE1_HAL_H */
