/**
 * @file    node2_config.h
 * @brief 节点二（STM32F103C8T6）板级配置：**所有引脚与可调参数的单一来源**。
 *
 * 与 node1_config.h 保持同一套结构：驱动代码不含任何魔数，换板只改此文件。
 * 引脚分配依据：docs/项目文档.md §4.3（节点二）与 docs/引脚分配.md
 * 电气约束依据：docs/项目文档.md §9.5
 *
 * 节点二职责：光照采集、SG90 舵机、红外检测。
 */
#ifndef NODE2_CONFIG_H
#define NODE2_CONFIG_H

/* 需要 PROTO_OFFLINE_TIMEOUT_MS 等协议时序常量（超时窗口由协议规定，
 * 节点不得自行其是）。显式包含而非依赖调用方先包含，避免"换个 include
 * 顺序就编译不过"的脆弱依赖。 */
#include "proto_seq.h"

/* ========================================================================== */
/* 身份                                                                       */
/* ========================================================================== */

/** 本节点号。proto_id.h 的 ID 编码用 ID[7:4] 承载节点号。 */
#define NODE2_NODE_ID          2u

/** 本节点实现的协议版本（心跳类帧上报用；与固件版本语义分离）。 */
#define NODE2_PROTOCOL_VERSION 1u

/** 固件版本（仅调试日志，不上行总线）。 */
#define NODE2_FW_VERSION_MAJOR 0
#define NODE2_FW_VERSION_MINOR 1
#define NODE2_FW_VERSION_PATCH 0

/* ========================================================================== */
/* 引脚分配（docs/项目文档.md §4.3）                                           */
/* ========================================================================== */

/* --- CAN1（bxCAN）-------------------------------------------------------- */
/* ⚠️ PA11/PA12 兼作 USB D-/D+：占用后板上 USB 不可用，调试只能走 SWD/RTT。
 * ⚠️ 另需检查 Blue Pill 板上 PA12 是否焊有 USB 上拉电阻（见 docs/引脚分配.md
 *    的风险表 R-6）——若存在会与 CAN 总线电平相互干扰。 */
#define NODE2_CAN_PORT          A
#define NODE2_CAN_TX_PIN        12   /**< PA12 = CAN1_TX */
#define NODE2_CAN_RX_PIN        11   /**< PA11 = CAN1_RX */
#define NODE2_CAN_BITRATE_BPS   500000u

/* --- 光敏模块 ------------------------------------------------------------- */
/* AO = ADC1_IN0（PA0）；DO = PA1。
 * ⚠️ STM32 的 ADC 引脚**完全不支持 5V**：模块若由 5V 供电，AO 必须外部分压；
 *    本项目模拟模块统一 3.3V 供电，可直连。DO 阈值需逐个电位器标定。 */
#define NODE2_LIGHT_ADC_CHANNEL   0u   /**< ADC1_IN0 = PA0 */
#define NODE2_LIGHT_DO_PORT       A
#define NODE2_LIGHT_DO_PIN        1u   /**< PA1 */

/* --- 红外模块 ------------------------------------------------------------- */
/* AO = ADC1_IN2（PA2）；DO = PA3。
 * ⚠️ 占用 PA2/PA3 即**失去 USART2** → 调试必须走 SEGGER RTT（SWD 通道）。 */
#define NODE2_IR_ADC_CHANNEL      2u   /**< ADC1_IN2 = PA2 */
#define NODE2_IR_DO_PORT          A
#define NODE2_IR_DO_PIN           3u   /**< PA3 */

/* --- SG90 舵机（PWM）------------------------------------------------------ */
/* PA6 = TIM3_CH1。选 TIM3 而非 TIM1 的理由：
 *   ① 释放 TIM1 高级定时器，避开"必须使能 MOE（TIM_CtrlPWMOutputs）"的坑；
 *   ② 与任何电机 PWM 分属不同定时器，节拍互不影响。
 * 注：50Hz 与 20kHz 是**两个不同用途**——舵机必须 50Hz（0.5~2.5ms 脉宽），
 *     电机 PWM 才用 20kHz 避开可闻声。两者不可混用同一频率。 */
#define NODE2_SERVO_PWM_TIM        3u  /**< TIM3 */
#define NODE2_SERVO_PWM_CH         1u  /**< TIM3_CH1 = PA6 */
#define NODE2_SERVO_PWM_FREQ_HZ    50u /**< SG90 标准 50Hz（周期 20ms） */
#define NODE2_SERVO_TIM_CLOCK_HZ   72000000u
#define NODE2_SERVO_MIN_US         500u   /**< 0°   对应脉宽 */
#define NODE2_SERVO_MAX_US         2500u  /**< 180° 对应脉宽 */
#define NODE2_SERVO_ANGLE_MIN      0u
#define NODE2_SERVO_ANGLE_MAX      180u

/** 可选：第二路舵机（PA7 = TIM3_CH2）。仅备料，默认不启用。 */
#define NODE2_SERVO2_ENABLE        0
#define NODE2_SERVO2_PWM_CH        2u  /**< TIM3_CH2 = PA7 */

/* --- SWD：必须保留，是烧写与调试的唯一通道 -------------------------------- */
/* PA13 = SWDIO，PA14 = SWCLK。这两个脚**不可**挪作他用。
 * 节点二无可用 UART 调试口，因此调试一律走 SEGGER RTT（复用 SWD，不占引脚）。 */

/* ========================================================================== */
/* 任务周期与栈预算（docs/项目文档.md §7，20KB SRAM 是硬约束）                */
/* ========================================================================== */

#define NODE2_SENSOR_PERIOD_MS        100u  /**< ADC + DO 采集 */
#define NODE2_HEARTBEAT_PERIOD_MS     1000u /**< 运行计数、喂狗 */
#define NODE2_TELEMETRY_PERIOD_MS     1000u /**< 遥测上报 */

/** 任务栈深度（单位：字，4 字节/字）。合计 ≈10KB，余量留给中断嵌套与栈保护。 */
#define NODE2_STACK_CAN_RX_WORDS     512u /**< 2KB */
#define NODE2_STACK_CAN_TX_WORDS     256u /**< 1KB */
#define NODE2_STACK_SENSOR_WORDS     256u /**< 1KB */
#define NODE2_STACK_SERVO_WORDS      512u /**< 2KB：角度换算 + 格式化 */
#define NODE2_STACK_HEARTBEAT_WORDS  128u /**< 0.5KB */

/** 任务优先级：CAN 收发最高，业务其次，心跳最低。 */
#define NODE2_PRIO_CAN_RX     5u
#define NODE2_PRIO_CAN_TX     5u
#define NODE2_PRIO_SENSOR     3u
#define NODE2_PRIO_SERVO      3u
#define NODE2_PRIO_HEARTBEAT  1u

/* ========================================================================== */
/* 传感器标定与阈值（⚠️ 光敏为**待实测标定**的占位值）                        */
/* ========================================================================== */

/** ⚠️ 光敏模块的 AO 输出与照度**没有标准关系**：模块上是光敏电阻分压，
 *  输出随供电、电位器、器件离散性变化，因此无法直接换算 lux。
 *  两种处置：
 *    (a) 保持现模块 → 只上报"归一化百分比"，`valid` 位 bit0 的 lux 字段
 *        必须按标定曲线填，未标定前置 0 并清 valid 位；
 *    (b) 换 BH1750（I²C，直接输出 lux）→ 才算真正有了照度指标。
 *  协议帧字段是 uint16 lux，硬件未升级前**不得填未标定值充数**。 */
#define NODE2_LIGHT_USE_BH1750     0     /**< 1 = 已换 BH1750（I²C） */
#define NODE2_LIGHT_ADC_RAW_MIN    100u  /**< 判接线/供电异常用 */
#define NODE2_LIGHT_ADC_RAW_MAX    4000u
#define NODE2_LIGHT_FILTER_SAMPLES 8u

/** 红外 DO 阈值由模块板上电位器决定，个体差异大 → 逐个标定。 */
#define NODE2_IR_FILTER_SAMPLES    8u
#define NODE2_IR_TRIGGER_DEBOUNCE_MS 50u /**< 去抖：防触发瞬间连发事件帧 */

/** 事件阈值（占位，待实测回填）。 */
#define NODE2_LIGHT_LOW_PCT        10u   /**< 低于此百分比视为"光照越限" */
#define NODE2_LIGHT_HIGH_PCT       90u

/* ========================================================================== */
/* 掉线安全态（docs/protocol.md §7.2）                                        */
/* ========================================================================== */

/** 掉线判定：3s 内未收到主站任何帧。 */
#define NODE2_MASTER_OFFLINE_MS  PROTO_OFFLINE_TIMEOUT_MS

/** 安全态：舵机**保持最后角度**（而非断电/归零）。
 *  理由：舵机断电会因负载自重掉落而改变角度，在云台/遮阳帘类执行器上反而危险；
 *  "保持"是可预期的确定状态，且不产生额外电流冲击。 */
#define NODE2_SAFE_SERVO_HOLD   1

/* ========================================================================== */
/* 编译期校验                                                                  */
/* ========================================================================== */

/* 舵机 PWM 计数：72MHz / (PSC+1) / (ARR+1) = 50Hz → 周期 20000 计数，
 * 1µs 分辨率（PSC=71 → 1MHz 计数，ARR=19999）。 */
#define NODE2_SERVO_PWM_PSC  71u
#define NODE2_SERVO_PWM_ARR  ((NODE2_SERVO_TIM_CLOCK_HZ / \
                               (NODE2_SERVO_PWM_PSC + 1u) / \
                               NODE2_SERVO_PWM_FREQ_HZ) - 1u)

/* 栈预算不得超 16KB：20KB SRAM 还要留给中断栈、堆与 FreeRTOS 自身。 */
#if (NODE2_STACK_CAN_RX_WORDS + NODE2_STACK_CAN_TX_WORDS + \
     NODE2_STACK_SENSOR_WORDS + NODE2_STACK_SERVO_WORDS + \
     NODE2_STACK_HEARTBEAT_WORDS) > 4096u
#error "node2 task stacks exceed 16KB budget (20KB SRAM is the hard limit)"
#endif

/* 舵机角度范围必须落在协议允许的 0~180 内（proto_codec.h 会强校验）。 */
#if (NODE2_SERVO_ANGLE_MAX > 180u)
#error "node2 servo angle max exceeds the protocol limit of 180 degrees"
#endif

#endif /* NODE2_CONFIG_H */
