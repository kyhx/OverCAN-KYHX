/**
 * @file    node1_config.h
 * @brief 节点一（STM32F103C8T6）板级配置：**所有引脚与可调参数的单一来源**。
 *
 * 为什么要集中：本项目要烧到两种"节点一"（C8T6 64KB / CBT6 128KB），
 * 且文档里的引脚表是设计值、未必与实物一致。参数散落在驱动各处时，
 * 换板子就要全局搜索改数字——这是嵌入式里最常见的低级错误来源。
 * 改这里一处即可，驱动代码不含任何魔数。
 *
 * 引脚分配依据：docs/项目文档.md §4.2（节点一）
 * 电气约束依据：docs/项目文档.md §9.5
 */
#ifndef NODE1_CONFIG_H
#define NODE1_CONFIG_H

/* ========================================================================== */
/* 身份                                                                       */
/* ========================================================================== */

/** 本节点号。proto_id.h 的 ID 编码用 ID[7:4] 承载节点号。 */
#define NODE1_NODE_ID          1u

/** 协议版本（心跳 b5 上报）。与 PROTO_PROTOCOL_VERSION 必须一致，
 *  但语义不同：那是协议版本，这里是本节点实现的协议版本。 */
#define NODE1_PROTOCOL_VERSION 1u

/** 固件版本（用于调试日志，不上行到总线）。语义与协议版本分离。 */
#define NODE1_FW_VERSION_MAJOR 0
#define NODE1_FW_VERSION_MINOR 1
#define NODE1_FW_VERSION_PATCH 0

/* ========================================================================== */
/* 引脚分配（docs/项目文档.md §4.2）                                           */
/* ========================================================================== */

/* --- CAN1（bxCAN）-------------------------------------------------------- */
/* ⚠️ PA11 与 USB D- 共用：占用后板上 USB 不可用，调试只能走 SWD。 */
#define NODE1_CAN_PORT          A
#define NODE1_CAN_TX_PIN        12   /**< PA12 = CAN1_TX */
#define NODE1_CAN_RX_PIN        11   /**< PA11 = CAN1_RX */
#define NODE1_CAN_BITRATE_BPS   500000u

/* --- 热敏传感器 ----------------------------------------------------------- */
/* AO = ADC1_IN0（PA0）；DO = PA1。
 * ⚠️ STM32F103 的 ADC 引脚**完全不支持 5V**，模拟输入不得超过 VDDA。
 * 3.3V 供电时模块输出 ≤3.3V 可直连；5V 供电**必须**外部分压。 */
#define NODE1_THERM_ADC_CHANNEL   0u   /**< ADC1_IN0 = PA0 */
#define NODE1_THERM_DO_PORT       A
#define NODE1_THERM_DO_PIN        1u   /**< PA1，热敏模块数字量输出 */

/* --- TB6612FNG 电机驱动（双通道）------------------------------------------ */
/* PWMA = PA8 = TIM1_CH1；PWMB = PA9 = TIM1_CH2。
 * ⚠️ PA9 = USART1_TX，占用后**失去标准调试串口** → 调试走 SEGGER RTT。 */
#define NODE1_MOTOR_PWM_TIM          1u
#define NODE1_MOTOR_A_PWM_CH         1u  /**< TIM1_CH1 = PA8 */
#define NODE1_MOTOR_B_PWM_CH         2u  /**< TIM1_CH2 = PA9 */
/** PWM 频率 20 kHz：高于人耳听觉上限，电机啸叫不可闻。
 *  72MHz / (PSC+1) / (ARR+1) = 20kHz → PSC=0, ARR=3599。 */
#define NODE1_MOTOR_PWM_FREQ_HZ      20000u
#define NODE1_MOTOR_PWM_TIM_CLOCK_HZ 72000000u

/* 方向控制脚（与上面的 PWMA/PWMB 配对：A 组一组，B 组一组） */
#define NODE1_MOTOR_A_IN1_PORT    B
#define NODE1_MOTOR_A_IN1_PIN     0u  /**< PB0 */
#define NODE1_MOTOR_A_IN2_PORT    B
#define NODE1_MOTOR_A_IN2_PIN     1u  /**< PB1 */
#define NODE1_MOTOR_B_IN1_PORT    B
#define NODE1_MOTOR_B_IN1_PIN     10u /**< PB10 */
#define NODE1_MOTOR_B_IN2_PORT    B
#define NODE1_MOTOR_B_IN2_PIN     11u /**< PB11 */

/** STBY：高电平使能驱动芯片，低电平进入待机（输出高阻）。
 *  掉线安全态必须拉低——见 NODE1_FAILSAFE_STBY。 */
#define NODE1_MOTOR_STBY_PORT    B
#define NODE1_MOTOR_STBY_PIN     12u /**< PB12 */

/* --- 蜂鸣器（仅节点一）---------------------------------------------------- */
/* ⚠️ 低电平有效：绝大多数模块都是 GND 与 IO 短接即鸣。 */
#define NODE1_BUZZER_PORT        B
#define NODE1_BUZZER_PIN         13u /**< PB13 */
#define NODE1_BUZZER_ACTIVE_LOW  1

/* --- SWD：必须保留，是烧写与调试的唯一通道 -------------------------------- */
/* PA13 = SWDIO，PA14 = SWCLK。这两个脚**不可**挪作他用。 */

/* ========================================================================== */
/* 任务周期与栈预算（docs/项目文档.md §7，20KB SRAM 是硬约束）                */
/* ========================================================================== */

#define NODE1_SENSOR_PERIOD_MS        100u  /**< ADC + DO 采集 */
#define NODE1_HEARTBEAT_PERIOD_MS     1000u /**< 运行计数、喂狗 */
#define NODE1_TELEMETRY_PERIOD_MS     1000u /**< 遥测上报（与 PROTO_TELEMETRY_PERIOD_MS 一致） */

/** 任务栈深度（单位：字，4 字节/字）。合计 ≈10KB，给中断嵌套与栈保护留 ≥4KB。
 *  数值来自 docs/项目文档.md §7 栈预算表，落地后必须用
 *  uxTaskGetStackHighWaterMark() 打印水位并回填验收记录。 */
#define NODE1_STACK_CAN_RX_WORDS     512u /**< 2KB：收帧+解析+入队，栈需求最大 */
#define NODE1_STACK_CAN_TX_WORDS     256u /**< 1KB */
#define NODE1_STACK_SENSOR_WORDS     256u /**< 1KB */
#define NODE1_STACK_MOTOR_WORDS      512u /**< 2KB：PWM 更新 + 格式化 */
#define NODE1_STACK_BUZZER_WORDS     128u /**< 0.5KB */
#define NODE1_STACK_HEARTBEAT_WORDS  128u /**< 0.5KB */

/** 任务优先级：CAN 收发最高（总线超时不容忍），业务其次，心跳最低。 */
#define NODE1_PRIO_CAN_RX    5u
#define NODE1_PRIO_CAN_TX    5u
#define NODE1_PRIO_SENSOR   3u
#define NODE1_PRIO_MOTOR    3u
#define NODE1_PRIO_BUZZER   2u
#define NODE1_PRIO_HEARTBEAT 1u

/* ========================================================================== */
/* 传感器标定与阈值（⚠️ 全部为**待实测标定**的占位值，见下方说明）              */
/* ========================================================================== */

/** ⚠️ 热敏模块的 AO 电压→温度关系取决于模块上的分压电阻与 NTC 规格，
 *  个体差异极大。本项目 BOM 是"热敏模块"（未指定型号），因此这里用
 *  **占位标定**，首次上电必须按 docs 的验收流程实测回填。
 *
 *  换算模型（与 bsp_analog.c 的实现一致）：
 *      R_ntc  = R_PULL * (VREF / V_ao - 1)
 *      T      = 1 / (1/T0 + ln(R_ntc/R0) / B) - 273.15
 *  若实测偏差大，优先改 NODE1_THERM_B / R_PULL，而不是改代码。 */
#define NODE1_THERM_R_PULL_OHMS     10000.0f /**< 分压上拉电阻，默认 10k */
#define NODE1_THERM_R0_OHMS         10000.0f /**< NTC 25℃ 阻值，默认 10k */
#define NODE1_THERM_B                3950.0f  /**< NTC B 值 */
#define NODE1_THERM_T0_K             298.15f  /**< NTC 25℃ 绝对温度 */
#define NODE1_THERM_VREF_MV          3300.0f   /**< VDDA */

#define NODE1_THERM_FILTER_SAMPLES   8u  /**< 中值/均值滤波次数 */

/** 过温报警阈值（℃）：超过则上报 PROTO_EV_OVER_TEMP 并进安全态。 */
#define NODE1_TEMP_ALARM_C           60.0f
/** 过温恢复阈值（℃）：滞回，防阈值附近抖动导致事件风暴。 */
#define NODE1_TEMP_RELEASE_C         50.0f
/** 传感器故障判定：ADC 原始值超出此范围视为接线/供电异常。 */
#define NODE1_ADC_RAW_MIN            100u
#define NODE1_ADC_RAW_MAX            4000u

/* ========================================================================== */
/* 掉线安全态（docs/protocol.md §7.2）                                        */
/* ========================================================================== */

/** 掉线判定：3s 内未收到主站心跳。 */
#define NODE1_MASTER_OFFLINE_MS  PROTO_OFFLINE_TIMEOUT_MS

/** 安全态动作（可被协议覆盖）。**电机必须制动停而非自由滑行**——
 *  执行器失控是总线故障时最可能造成物理损害的路径。 */
typedef enum {
    NODE1_SAFE_MOTOR_BRAKE = 0, /**< 制动停（TB6612 IN1=IN2 高 → 短路刹车）*/
    NODE1_SAFE_MOTOR_COAST,      /**< 自由滑行（IN1=IN2 低）            */
    NODE1_SAFE_MOTOR_STOP        /**< 停转（IN1=IN2 不同 → 惰行）        */
} node1_safe_motor_mode_t;

#define NODE1_SAFE_MOTOR_MODE   NODE1_SAFE_MOTOR_BRAKE
#define NODE1_SAFE_BUZZER_OFF   1   /**< 掉线时蜂鸣器静默（避免总线上持续噪声） */

/* ========================================================================== */
/* 编译期校验                                                                  */
/* ========================================================================== */

/* PWM 计数：72MHz / (PSC+1) = 计数频率，计数频率 / (ARR+1) = PWM 频率 */
#define NODE1_MOTOR_PWM_PSC  0u
#define NODE1_MOTOR_PWM_ARR  ((NODE1_MOTOR_PWM_TIM_CLOCK_HZ / \
                              (NODE1_MOTOR_PWM_PSC + 1u) / NODE1_MOTOR_PWM_FREQ_HZ) - 1u)

/* 栈预算不得超 16KB：20KB SRAM 还要留给中断栈、堆与 FreeRTOS 自身。 */
#if (NODE1_STACK_CAN_RX_WORDS + NODE1_STACK_CAN_TX_WORDS + \
     NODE1_STACK_SENSOR_WORDS + NODE1_STACK_MOTOR_WORDS + \
     NODE1_STACK_BUZZER_WORDS + NODE1_STACK_HEARTBEAT_WORDS) > 4096u
#error "node1 task stacks exceed 16KB budget (20KB SRAM is the hard limit)"
#endif

#endif /* NODE1_CONFIG_H */
