/**
 * @file    node1_bsp.c
 * @brief 节点一 STM32F103 BSP 实现：bxCAN / ADC / TIM4 PWM / GPIO / IWDG。
 *
 * 本文件是整个固件里**唯一**直接接触 STM32 外设的地方。所有寄存器操作
 * 都集中在此，业务层（node1_app.c）完全不知道硬件存在 —— 这正是业务逻辑
 * 能在 PC 上做单测的前提。
 *
 * 三个必须做对的工程细节（docs/项目文档.md §9.5，均为实测踩过的坑）：
 *   1. **PWM 放在通用定时器 TIM4（PB6/PB7），不放 TIM1**。原方案用 TIM1 的
 *      PA8/PA9，有两个代价：① PA9 兼作 USART1_TX，占用后节点一失去唯一调试
 *      串口；② TIM1 是高级控制定时器，输出默认关闭，**必须显式调用
 *      `HAL_TIM_CtrlPWMOutputs(TIM1, ENABLE)` 使能 MOE**，漏掉这行的现象极具
 *      迷惑性：寄存器配置全对、定时器在跑、引脚上却没有波形。
 *      改到 TIM4 后这两个问题一起消失（通用定时器无 MOE 概念）。
 *      依据：docs/引脚分配.md §3.2 方案 A。
 *   2. **bxCAN 的 BS1 只有 4 bit**（TS1[19:16]），最大 16 tq。
 *      网上常见的"24 tq @ 500kbps"方案在 F103 上根本编译不出来/跑不起来。
 *      本项目用 8 tq：P=9 BS1=6 BS2=1 SJW=1 → 采样点 87.5%。
 *   3. **PA11 兼作 USB D-**：启用 CAN 后板上 USB 口不可用，调试只能走 SWD。
 *
 * 硬件前提（务必按 node1_config.h 接线）：
 *   - 热敏 AO → PA0 (ADC1_IN0)，DO → PA1
 *   - TB6612: PWMA=PB6(TIM4_CH1) PWMB=PB7(TIM4_CH2) A组 IN1=PB0 IN2=PB1
 *             B组 IN1=PB10 IN2=PB11 STBY=PB12
 *   - 蜂鸣器 PB13（**低电平有效**）
 *   - 调试串口 USART1: TX=PA9 RX=PA10（PWM 移走后已释放）
 *   - CAN1: TX=PA12 RX=PA11，**两端必须各接 120Ω 终端电阻**
 *   - SWD: PA13/PA14 **必须保留**
 */
#include "node1_bsp.h"

#include <string.h>

#include "stm32f1xx_hal.h"

#include "node1_config.h"

/* ========================================================================== */
/* 时钟与串口调试（可选，编译期开关）                                          */
/* ========================================================================== */

/** 调试输出开关：1 = 初始化 USART1（PA9/PA10，115200）。
 *  PWM 移到 TIM4 后 PA9 已释放，因此这里可以默认开启串口调试。
 *  注意：**必须在 gpio_init 之前调用 MX_USART1_UART_Init**（HAL 要求外设
 *  的 GPIO 由 HAL_UART_MspInit 配置），本文件在 bsp_init 里按序调用。 */
#define NODE1_UART_LOG_ENABLE  1

#if NODE1_UART_LOG_ENABLE
static UART_HandleTypeDef huart1;

/** USART1 初始化（PA9=TX / PA10=RX，115200-8-N-1）。
 *  PWM 移到 TIM4 后 PA9/PA10 已释放，节点一重新有了标准调试串口。
 *  注意：不要在中断里调用；且**不用 printf/浮点**（libm 会拖入数 KB，
 *  F103 只有 20KB SRAM）。 */
static void uart_log_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA9 = USART1_TX，复用推挽 */
    g.Pin = GPIO_PIN_9;
    g.Mode = GPIO_MODE_AF_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &g);
    /* PA10 = USART1_RX，浮空输入（外部模块驱动） */
    g.Pin = GPIO_PIN_10;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &g);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200u;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) {
        /* 串口起不来不应阻塞上线：CAN 才是主通道，调试口只是辅助 */
    }
}
#endif

/** 极简日志：优先走 USART1（若启用），否则静默。
 *  禁止在中断与栈保护路径里调用。 */
static void bsp_log(const char *msg)
{
#if NODE1_UART_LOG_ENABLE
    if (msg != NULL) {
        /* 用阻塞发送（超时 10ms）而不是中断/DMA：调试日志不该引入并发问题 */
        (void)HAL_UART_Transmit(&huart1, (const uint8_t *)msg,
                                (uint16_t)strlen(msg), 10u);
    }
#else
    (void)msg;
#endif
}

/* ========================================================================== */
/* GPIO                                                                        */
/* ========================================================================== */

static void gpio_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* --- PB6/PB7：TIM4_CH1/CH2 复用推挽输出（电机 PWM）-------------- */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_6 | GPIO_PIN_7);
    g.Mode = GPIO_MODE_AF_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;   /* 20kHz PWM 需要较高的输出翻转速度 */
    g.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOB, &g);

    /* --- 方向控制脚：PB0 PB1 PB10 PB11 推挽输出 ---------------------- */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_10 | GPIO_PIN_11);
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_10 | GPIO_PIN_11;
    HAL_GPIO_Init(GPIOB, &g);

    /* 上电先全部拉低：电机停、IN 组合为"惰行"。放在 gpio_init 末尾统一做，
     * 避免 GPIO 模式切换的瞬间出现浮空输入。 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_1, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10 | GPIO_PIN_11, GPIO_PIN_RESET);

    /* --- PB12 STBY：⚠️ 上电立即拉低（待机），见 node1_bsp_init 注释 --- */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_12);
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pin = GPIO_PIN_12;
    HAL_GPIO_Init(GPIOB, &g);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET); /* 安全优先 */

    /* --- PB13 蜂鸣器：⚠️ 低电平有效 → 初始必须写高（静音） ----------- */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_13);
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pin = GPIO_PIN_13;
    HAL_GPIO_Init(GPIOB, &g);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_SET); /* 高 = 不响 */

    /* --- PA1 热敏 DO：输入下拉（悬空时读 0，避免随机误报）----------- */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1);
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLDOWN;
    g.Pin = GPIO_PIN_1;
    HAL_GPIO_Init(GPIOA, &g);

    /* --- PA13/PA14 SWD：**不初始化、绝不复用** ----------------------
     * 复用后烧写与调试都会断，属于"改一次就救不回来"的错误。
     * 故此处刻意不做任何 GPIO 配置。 */
}

/* ========================================================================== */
/* ADC1（热敏 AO）                                                            */
/* ========================================================================== */

static ADC_HandleTypeDef hadc1;

static void adc_init(void)
{
    ADC_ChannelConfTypeDef ch = {0};
    __HAL_RCC_ADC1_CLK_ENABLE();

    hadc1.Instance = ADC1;
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;      /* 只用 1 个通道，省时间 */
    hadc1.Init.ContinuousConvMode = DISABLE;         /* 单次转换，采样任务触发 */
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion = 1;

    /* ADC 预分频：72MHz / 6 = 12MHz（ADC 最高 14MHz）
     * 采样时间取 239.5 周期 → 单次转换约 20μs，对应 12 位精度所需的高源阻抗。 */
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
    (void)HAL_ADCEx_Calibration_Start(&hadc1); /* 必须在使能前校准 */

    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        bsp_log("adc init failed\r\n");
    }

    ch.Channel = NODE1_THERM_ADC_CHANNEL;
    ch.Rank = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_239CYCLES_5; /* 源阻抗高时必须取最长 */
    (void)HAL_ADC_ConfigChannel(&hadc1, &ch);
}

/* ========================================================================== */
/* TIM4 → PWM（20kHz，PB6/PB7）                                                */
/* ========================================================================== */

/** 电机 PWM 定时器句柄。用 TIM4（通用定时器）而非 TIM1：
 *  ① 无需 MOE 使能；② 不占用 PA9/PA10 的 USART1 调试口。 */
static TIM_HandleTypeDef htim_motor;

static void pwm_init(void)
{
    TIM_OC_InitTypeDef oc = {0};
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_TIM4_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* PB6/PB7 必须在**本函数内**再配一次为复用推挽：
     * 虽然 gpio_init 已配过，但 gpio_init 末尾对方向脚/STBY/蜂鸣器的写操作
     * 以及后续可能的重配置都可能影响，PWM 引脚的正确性直接决定电机能否转动，
     * 这里做一次幂等的显式配置，避免"顺序改动导致没波形"。 */
    g.Mode = GPIO_MODE_AF_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOB, &g);

    htim_motor.Instance = TIM4;
    htim_motor.Init.Prescaler = NODE1_MOTOR_PWM_PSC;
    htim_motor.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim_motor.Init.Period = NODE1_MOTOR_PWM_ARR;  /* 72MHz/1/3600 = 20kHz */
    htim_motor.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim_motor.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_PWM_Init(&htim_motor) != HAL_OK) {
        bsp_log("tim4 pwm init failed\r\n");
    }

    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = 0; /* ⚠️ 必须为 0：非 0 会让电机一上电就转 */
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    /* 注：TIM4 是通用定时器，没有互补输出与 MOE，故不再设置
     *     OCIdleState / OCNIdleState 等高级定时器专用字段。 */

    (void)HAL_TIM_PWM_ConfigChannel(&htim_motor, &oc, TIM_CHANNEL_1); /* PB6 */
    (void)HAL_TIM_PWM_ConfigChannel(&htim_motor, &oc, TIM_CHANNEL_2); /* PB7 */

    (void)HAL_TIM_PWM_Start(&htim_motor, TIM_CHANNEL_1);
    (void)HAL_TIM_PWM_Start(&htim_motor, TIM_CHANNEL_2);

    /* 说明：这里**刻意不需要** HAL_TIM_CtrlPWMOutputs —— MOE 是 TIM1/TIM8
     * 等高级控制定时器才有的主输出使能位，通用定时器 TIM4 无此概念。
     * 若将来把 PWM 换回 TIM1，必须补上这一行，否则没有波形。 */
}

/* ========================================================================== */
/* bxCAN                                                                       */
/* ========================================================================== */

/** 接收环形缓冲：中断入队、任务出队。
 *  深度 16 是权衡结果：FIFO 只有 3 个邮箱，中断里必须立刻搬走，
 *  否则高负载时会丢帧（见技术方案 OTA 章节的 T-18 实验）。 */
#define CAN_RX_Q_CAP 16
static proto_can_frame_t g_can_rx_q[CAN_RX_Q_CAP];
static volatile uint8_t  g_can_rx_head = 0;  /* 中断写 */
static volatile uint8_t  g_can_rx_tail = 0;  /* 任务读 */
static volatile bool     g_can_rx_overflow = false; /* 溢出统计（丢帧证据） */

static CAN_HandleTypeDef hcan1;

/** 1ms 时间基准：CAN 位定时需要精确的 APB1 频��，SysTick 兼作毫秒计数。 */
static volatile uint32_t g_millis = 0;
void SysTick_Handler(void) { g_millis++; }

static void can_filters_init(void)
{
    CAN_FilterTypeDef f = {0};

    /* bxCAN 有 **14 组硬件滤波器**（stm32f1xx 有 CAN1/CAN2 共 28 个，
     * 单个 CAN 控制器用 14 个）。这是安全建模里"ID 白名单"的**零成本**
     * 硬件落地：不属于本节点的帧由硬件直接丢弃，CPU 根本不会被唤醒。
     *
     * 节点一只该收 3 类帧：
     *   0x100 广播命令   0x110 发给节点一的单播   0x300 主站心跳
     *
     * 分两组配置（bxCAN 每个过滤器组 = 2×16bit 列表模式）：
     *   组 0：0x100/0x300 → 标准 ID 左移 5 位后用掩码 0x7E0 一次匹配两者的
     *         公共位（0x100>>5=0x08, 0x300>>5=0x18，掩码 0x7E0 覆盖位 5~10）
     *   组 1：0x110 单播精确匹配
     *
     * ⚠️ Bootloader 的过滤配置**不同**（还要收 ISO-TP 的诊断帧），
     * 跳转前必须重配 —— 见技术方案 §4.3。 */
    f.FilterBank = 0;
    f.FilterMode = CAN_FILTERMODE_IDMASK;
    f.FilterScale = CAN_FILTERSCALE_32BIT;
    f.FilterIdHigh = (uint16_t)(PROTO_ID_MASTER_HEARTBEAT << 5);
    f.FilterIdLow = 0;
    f.FilterMaskIdHigh = (uint16_t)(0x7E0u << 5);
    f.FilterMaskIdLow = 0;
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterActivation = ENABLE;
    (void)HAL_CAN_ConfigFilter(&hcan1, &f);

    f.FilterBank = 1;
    f.FilterMode = CAN_FILTERMODE_IDMASK;
    f.FilterScale = CAN_FILTERSCALE_32BIT;
    f.FilterIdHigh = (uint16_t)(PROTO_ID_CMD_NODE1 << 5);
    f.FilterMaskIdHigh = (uint16_t)(0x7FFu << 5); /* 精确匹配单个 ID */
    f.FilterMaskIdLow = 0;
    (void)HAL_CAN_ConfigFilter(&hcan1, &f);

    /* 组 2~13 全部关掉：不关的话它们默认放行一切，等于白配。 */
    for (uint8_t bank = 2u; bank < 14u; ++bank) {
        CAN_FilterTypeDef d = {0};
        d.FilterBank = bank;
        d.FilterMode = CAN_FILTERMODE_IDMASK;
        d.FilterScale = CAN_FILTERSCALE_32BIT;
        d.FilterIdHigh = 0;
        d.FilterMaskIdHigh = 0;
        d.FilterFIFOAssignment = CAN_RX_FIFO0;
        d.FilterActivation = DISABLE;
        (void)HAL_CAN_ConfigFilter(&hcan1, &d);
    }
}

static void can_init(void)
{
    __HAL_RCC_CAN1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* CAN 引脚：复用推挽。⚠️ PA11 兼作 USB D-，占用后 USB 不可用。 */
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    g.Mode = GPIO_MODE_AF_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;   /* 500kbps 需要边沿够陡 */
    HAL_GPIO_Init(GPIOA, &g);

    hcan1.Instance = CAN1;
    hcan1.Init.Prescaler = 9;              /* APB1=36MHz → tq = 36/(9+1) = 3.6MHz */
    hcan1.Init.Mode = CAN_MODE_NORMAL;
    hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
    /* 采样点 = (1 + TS1) / (1 + TS1 + TS2) = (1+6)/8 = 87.5%
     * ⚠️ TS1 只有 4 bit（最大 16 tq），网上"24tq@500k"方案在 F103 上做不到。 */
    hcan1.Init.TimeSeg1 = CAN_BS1_6TQ;
    hcan1.Init.TimeSeg2 = CAN_BS2_1TQ;
    hcan1.Init.TimeTriggeredMode = DISABLE;
    hcan1.Init.AutoBusOff = ENABLE;  /* Bus-Off 后自动恢复，避免节点永久掉线 */
    hcan1.Init.AutoWakeUp = ENABLE;
    hcan1.Init.AutoRetransmission = ENABLE; /* 重发直到被 ACK 或超时 */

    if (HAL_CAN_Init(&hcan1) != HAL_OK) {
        bsp_log("can init failed\r\n");
        return;
    }
    can_filters_init();
}

/** CAN RX 中断（USB_LP_CAN1_RX0_IRQHandler 由 stm32f1xx_it.c 转发到此）。 */
void node1_can_rx_isr_handler(void)
{
    CAN_RxHeaderTypeDef hdr;
    uint8_t data[8];

    if (HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &hdr, data) != HAL_OK) {
        return;
    }
    if (hdr.IDE != CAN_ID_STD) {
        return; /* 本协议只用 11 位标准帧，扩展帧直接丢 */
    }

    /* 中断里只做"搬进队列"，**不做解析、不做业务**。
     * 理由：中断执行时间必须可预测，否则会拖累其它中断；
     * 且 parse + 幂等判定的耗时不该占用中断窗口。 */
    const uint8_t next = (uint8_t)((g_can_rx_head + 1u) % CAN_RX_Q_CAP);
    if (next == g_can_rx_tail) {
        /* 队列满 → 丢帧。置标志而不是静默丢弃：
         * 排查"偶尔丢命令"时，没有这个标志根本无从下手。 */
        g_can_rx_overflow = true;
        return;
    }
    proto_can_frame_t *slot = &g_can_rx_q[g_can_rx_head];
    slot->id = (uint16_t)hdr.StdId;
    slot->dlc = (uint8_t)(hdr.DLC > 8u ? 8u : hdr.DLC);
    memcpy(slot->data, data, slot->dlc);
    g_can_rx_head = next;
}

/* ========================================================================== */
/* HAL vtable 实现                                                             */
/* ========================================================================== */

static uint32_t bsp_millis(void) { return g_millis; }
static void bsp_delay_ms(uint32_t ms) { HAL_Delay(ms); }

static bool bsp_sensor_read(node1_sensor_raw_t *out)
{
    if (out == NULL) {
        return false;
    }
    if (HAL_ADC_Start(&hadc1) != HAL_OK) {
        return false;
    }
    if (HAL_ADC_PollForConversion(&hadc1, 5u) != HAL_OK) {
        (void)HAL_ADC_Stop(&hadc1);
        return false;
    }
    out->therm_raw = (uint16_t)HAL_ADC_GetValue(&hadc1);
    (void)HAL_ADC_Stop(&hadc1);

    /* DO 读 GPIO。模块输出高/低的含义因电位器而异，这里统一成
     * "高 = 超过阈值"，与遥测 do_state 的 0/1 语义一致。 */
    out->therm_do = (HAL_GPIO_ReadPin(GPIOA,
        (uint16_t)(1u << NODE1_THERM_DO_PIN)) == GPIO_PIN_SET);
    return true;
}

static void bsp_motor_set(node1_motor_id_t id, node1_motor_action_t action,
                          uint8_t duty_pct)
{
    GPIO_PinState in1 = GPIO_PIN_RESET;
    GPIO_PinState in2 = GPIO_PIN_RESET;
    uint16_t pin_in1;
    uint16_t pin_in2;
    GPIO_TypeDef *port_in1;
    GPIO_TypeDef *port_in2;
    uint32_t channel;

    if (id == NODE1_MOTOR_A) {
        port_in1 = GPIOB; pin_in1 = (uint16_t)(1u << NODE1_MOTOR_A_IN1_PIN);
        port_in2 = GPIOB; pin_in2 = (uint16_t)(1u << NODE1_MOTOR_A_IN2_PIN);
        channel = TIM_CHANNEL_1; /* PB6 */
    } else {
        port_in1 = GPIOB; pin_in1 = (uint16_t)(1u << NODE1_MOTOR_B_IN1_PIN);
        port_in2 = GPIOB; pin_in2 = (uint16_t)(1u << NODE1_MOTOR_B_IN2_PIN);
        channel = TIM_CHANNEL_2; /* PB7 */
    }

    switch (action) {
    case NODE1_MOTOR_ACTION_STOP:    /* 惰行 */
        in1 = GPIO_PIN_RESET; in2 = GPIO_PIN_RESET;
        break;
    case NODE1_MOTOR_ACTION_FORWARD: /* IN1=1 IN2=0 */
        in1 = GPIO_PIN_SET;   in2 = GPIO_PIN_RESET;
        break;
    case NODE1_MOTOR_ACTION_REVERSE: /* IN1=0 IN2=1 */
        in1 = GPIO_PIN_RESET; in2 = GPIO_PIN_SET;
        break;
    case NODE1_MOTOR_ACTION_BRAKE:   /* 短路刹车：IN1=IN2=1 */
        in1 = GPIO_PIN_SET;   in2 = GPIO_PIN_SET;
        break;
    default:
        break;
    }

    /* ⚠️ 顺序很关键：**先设 PWM 占空比，再切换方向**。
     * 反过来（先切方向再给占空比）的话，方向切换瞬间 PWM 仍是上一个
     * 值的高电平，电机会被"瞬间反打"，大电流冲击驱动芯片。 */
    if (duty_pct > 100u) {
        duty_pct = 100u;
    }
    const uint32_t ccr = ((uint32_t)duty_pct * (htim_motor.Init.Period + 1u)) / 100u;
    (void)HAL_TIM_SetCompare(&htim_motor, channel, ccr);

    HAL_GPIO_WritePin(port_in1, pin_in1, in1);
    HAL_GPIO_WritePin(port_in2, pin_in2, in2);
}

static void bsp_motor_enable(bool on)
{
#if NODE1_MOTOR_STBY_PORT == 'B'
    HAL_GPIO_WritePin(GPIOB, (uint16_t)(1u << NODE1_MOTOR_STBY_PIN),
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#endif
}

static void bsp_buzzer_set(bool on)
{
    /* ⚠️ 低电平有效：意图"响" → 输出低。写反了就是上电狂叫。 */
    const GPIO_PinState level =
#if NODE1_BUZZER_ACTIVE_LOW
        (on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#else
        (on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#endif
    HAL_GPIO_WritePin(GPIOB, (uint16_t)(1u << NODE1_BUZZER_PIN), level);
}

static bool bsp_can_send(const proto_can_frame_t *f)
{
    if (f == NULL || f->dlc > 8u) {
        return false;
    }
    CAN_TxHeaderTypeDef tx;
    uint32_t mailbox;
    uint32_t t0 = g_millis;

    tx.StdId = f->id;
    tx.IDE = CAN_ID_STD;
    tx.RTR = CAN_RTR_DATA;
    tx.DLC = f->dlc;

    /* 邮箱满时短暂等待重试（最多 5ms）。绝不能无限重试：
     * 那样会导致 CAN 任务卡死，进而丢失心跳 —— 一个发送失败
     * 演变成"整个节点离线"的连锁故障。 */
    while (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) == 0u) {
        if ((g_millis - t0) > 5u) {
            return false;
        }
    }
    if (HAL_CAN_AddTxMessage(&hcan1, &tx, (uint8_t *)f->data, &mailbox) != HAL_OK) {
        return false;
    }
    return true;
}

static bool bsp_can_recv(proto_can_frame_t *out)
{
    if (out == NULL) {
        return false;
    }
    /* 关中断读 tail，避免与中断写 head 竞争（8 位对齐，1 条指令窗口） */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    const bool got = (g_can_rx_head != g_can_rx_tail);
    if (got) {
        *out = g_can_rx_q[g_can_rx_tail];
        g_can_rx_tail = (uint8_t)((g_can_rx_tail + 1u) % CAN_RX_Q_CAP);
    }
    __set_PRIMASK(primask);
    return got;
}

static const node1_hal_t g_hal = {
    bsp_millis, bsp_delay_ms, bsp_sensor_read, bsp_motor_set,
    bsp_motor_enable, bsp_buzzer_set, bsp_can_send, bsp_can_recv
};

/* ========================================================================== */
/* 对外接口                                                                    */
/* ========================================================================== */

void node1_bsp_init(const node1_hal_t **out_hal)
{
    HAL_Init();

    /* 72MHz：HSE 8MHz × PLL(×9)。CAN 依赖 APB1=36MHz，ADC 依赖 72MHz。 */
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL = RCC_PLL_MUL9;
    (void)HAL_RCC_OscConfig(&osc);

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;   /* HCLK  = 72MHz */
    clk.APB1CLKDivider = RCC_HCLK_DIV2;    /* PCLK1 = 36MHz（CAN 上限 36MHz）*/
    clk.APB2CLKDivider = RCC_HCLK_DIV1;    /* PCLK2 = 72MHz（ADC 上限 14MHz）*/
    (void)HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2);

    /* 1kHz SysTick → 既作 HAL 延时基准，也作 g_millis 时基 */
    HAL_SYSTICK_Config(SystemCoreClock / 1000u);

    gpio_init();
#if NODE1_UART_LOG_ENABLE
    uart_log_init(); /* 放在 gpio_init 之后：两者都操作 GPIOA，顺序明确可读 */
#endif
    adc_init();
    pwm_init();
    can_init();

    /* 启动 CAN 接收中断 */
    if (HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK) {
        bsp_log("can irq activate failed\r\n");
    }
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 1u); /* 高优先级：总线收发不能被拖 */
    HAL_NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);

    /* 独立看门狗：约 2s 超时。
     * ⚠️ IWDG 启动后**软件无法关闭**，所以所有长循环（含 Bootloader、
     * ISO-TP 等待、flash 擦除）都必须喂狗，否则必然复位。 */
    IWDG_HandleTypeDef iwdg = {0};
    iwdg.Instance = IWDG;
    iwdg.Init.Prescaler = IWDG_PRESCALER_64;  /* 40kHz/64 = 625Hz */
    iwdg.Init.Reload = 1250u - 1u;             /* 1250/625 = 2.0s */
    (void)HAL_IWDG_Init(&iwdg);

    if (out_hal != NULL) {
        *out_hal = &g_hal;
    }
}

void node1_bsp_feed_watchdog(void)
{
    /* 句柄在 init 内部，需保持一份静态引用 */
    static IWDG_HandleTypeDef hiwdg = {0};
    static bool inited = false;
    if (!inited) {
        hiwdg.Instance = IWDG;
        hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
        hiwdg.Init.Reload = 1250u - 1u;
        inited = true;
    }
    (void)HAL_IWDG_Refresh(&hiwdg);
}

void node1_bsp_soft_reset(void)
{
    /* 复位前把执行器置于安全态：电机停 + STBY 拉低。
     * 否则复位瞬间电机仍在转，或驱动芯片在高阻/低阻间跳变。
     * 这一步在有执行器的系统里不是可选项。 */
    bsp_motor_set(NODE1_MOTOR_A, NODE1_MOTOR_ACTION_STOP, 0u);
    bsp_motor_set(NODE1_MOTOR_B, NODE1_MOTOR_ACTION_STOP, 0u);
    bsp_motor_enable(false);
    bsp_buzzer_set(false);

    HAL_Delay(2u); /* 让驱动级把 STBY 拉低真正生效 */
    NVIC_SystemReset();
}

void node1_bsp_print_stack_watermarks(void)
{
    /* 移植到 FreeRTOS 后在此调用 uxTaskGetStackHighWaterMark()，
     * 结果走 RTT。当前裸机版本无任务概念，留空但保留接口，
     * 避免以后为了加日志而改动所有调用点。 */
}
