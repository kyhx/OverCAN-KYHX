/**
 * @file    node1_bsp.c
 * @brief 节点一 STM32F103 BSP 实现：bxCAN / ADC / TIM4 PWM / GPIO / IWDG。
 *
 * 本文件是整个固件里**唯一**直接接触 STM32 外设的地方。所有寄存器操作
 * 都集中在此，业务层（node1_app.c）完全不知道硬件存在 —— 这正是业务逻辑
 * 能在 PC 上做单测的前提。
 *
 * 三个必须做对的工程细节（docs/项目文档.md §9.5，均为实测踩过的坑）：
 *   1. **PWM 落在哪个定时器，由实物指定的引脚决定，不能反推**。
 *      历史过程：最初用 TIM1 的 PA8/PA9（代价：PA9 兼 USART1_TX，且 TIM1 必须
 *      使能 MOE，否则"寄存器全对、引脚无波形"）→ 为回避而改到 TIM4 的 PB6/PB7。
 *      **现按用户 2026-10-04 的实物接线改用 PB15，而 PB15 在 LQFP48 上只有
 *      TIM1_CH3N 一个定时器功能**，于是又回到 TIM1，那两个坑必须正面处理：
 *      ① `HAL_TIMEx_PWMN_Start()` 内部会 `__HAL_TIM_MOE_ENABLE()`——
 *         互补通道必须用这个函数启动（`HAL_TIM_PWM_Start` 只开主通道 CCxE）；
 *      ② 占空比仍由主通道 CCR3 决定（CC3NE 只是把 OC3REF 引到 PB15）。
 *      教训：**引脚决定定时器，而不是定时器决定引脚**。
 *   2. **bxCAN 的 BS1 只有 4 bit**（TS1[19:16]），最大 16 tq。
 *      网上常见的"24 tq @ 500kbps"方案在 F103 上根本编译不出来/跑不起来。
 *      本项目用 8 tq：P=9 BS1=6 BS2=1 SJW=1 → 采样点 87.5%。
 *   3. **PA11 兼作 USB D-**：启用 CAN 后板上 USB 口不可用，调试只能走 SWD。
 *
 * 硬件前提（务必按 node1_config.h 接线）：
 *   - 热敏 AO → PA0 (ADC1_IN0)，DO → PA1
 *   - TB6612（**只用电机 A**）: PWMA=PB15(TIM1_CH3N) AIN1=PB14 AIN2=PB13 STBY=PB12
 *     ⚠️ PB15 只有 TIM1_CH3N 这一个定时器功能 → 必须使能 MOE + 用 PWMN_Start
 *   - 蜂鸣器 **PA6**（**低电平有效**）
 *   - 调试串口 USART1: TX=PA9 RX=PA10（PA10 是 TIM1_CH3 主通道，但我们不初始化它，
 *     且只启动互补通道，故 PA10 不受 PWM 影响）
 *   - CAN1: TX=PA12 RX=PA11，**两端必须各接 120Ω 终端电阻**
 *   - SWD: PA13/PA14 **必须保留**
 */
#include "node1_bsp.h"

#include <string.h>

/* HAL 的模块启用项（HAL_CAN/TIM/IWDG_MODULE_ENABLED）由工程的 CMakeLists
 * 以编译定义方式提供，**不在这里 include 头文件**。
 * 原因见 CAN_Node1/CMakeLists.txt 的注释：HAL 驱动源码的整个文件体被
 * #ifdef HAL_xxx_MODULE_ENABLED 包着，而它们不包含我们的守卫头，
 * 只有"定义在命令行上"才能同时覆盖它们与我们自己的源文件。 */
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

    /* --- PB15：TIM1_CH3N 复用推挽输出（电机 PWM）---------------------
     * ⚠️ PB15 在 LQFP48 上只有 TIM1_CH3N 这一个定时器功能（见 node1_config.h）。
     * 只初始化 PB15；PA10（TIM1_CH3 主通道）**保持默认浮空输入**，
     * 因此 CC3E 即使被配置也不会在 PA10 上产生波形，USART1_RX 不受影响。 */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_15);
    g.Mode = GPIO_MODE_AF_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;   /* 20kHz PWM 需要较高的输出翻转速度 */
    g.Pin = GPIO_PIN_15;
    HAL_GPIO_Init(GPIOB, &g);

    /* --- 方向控制脚：PB14(PB13 同一组寄存器) 推挽输出 ---------------- */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_14 | GPIO_PIN_13);
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pin = GPIO_PIN_14 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOB, &g);

    /* 上电先全部拉低：IN1=IN2=0 → 惰行（电机不转）。
     * 放在 gpio_init 末尾统一做，避免 GPIO 模式切换的瞬间出现浮空输入。 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14 | GPIO_PIN_13, GPIO_PIN_RESET);

    /* --- PB12 STBY：⚠️ 上电立即拉低（待机），见 bsp_motor_enable 注释 --- */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_12);
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pin = GPIO_PIN_12;
    HAL_GPIO_Init(GPIOB, &g);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET); /* 安全优先 */

    /* --- PA6 蜂鸣器：⚠️ 低电平有效 → 初始必须写高（静音） -------------
     * 蜂鸣器已从 PB13 改到 PA6（用户 2026-10-04 指定），
     * 所以这里操作的是 **GPIOA** —— 与 node1_config.h 末尾的断言一致。 */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_6);
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pin = GPIO_PIN_6;
    HAL_GPIO_Init(GPIOA, &g);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET); /* 高 = 不响 */

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
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.NbrOfDiscConversion = 0;

    /* ⚠️ 校准的使能/关闭**由 HAL 自己完成**，不要再手写 HAL_ADC_Enable/Disable：
     * F1 的 HAL 里根本没有这两个函数（它们是 F3/F4 才有的 API），写了只会得到
     * "implicit declaration" 警告 + 链接错误。F1 的
     * HAL_ADCEx_Calibration_Start() 内部顺序是：
     *     ADC_ConversionStop_Disable() → ADC_Enable() → 启动校准 → 关 ADC
     * 也就是说调用它本身就把"必须先在开启状态才能校准"这件事满足了。
     * 初版把它放在 HAL_ADC_Init 之前（ADC 尚未配置）属于静默失效：
     * 编译能过，但校准没有真正生效、精度不达标。 */
    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        bsp_log("adc init failed\r\n");
    }
    (void)HAL_ADCEx_Calibration_Start(&hadc1);

    ch.Channel = NODE1_THERM_ADC_CHANNEL;
    ch.Rank = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_239CYCLES_5; /* 源阻抗高时必须取最长 */
    (void)HAL_ADC_ConfigChannel(&hadc1, &ch);
}

/* ========================================================================== */
/* TIM1 → PWM（20kHz，PB15 = TIM1_CH3N 互补输出）                               */
/* ========================================================================== */

/** 电机 PWM 定时器句柄。
 *
 * ⚠️ 这里用 TIM1（高级控制定时器）而**不是** TIM4，因为实物指定的 PWMA 是
 *    **PB15**，而 PB15 只有 `TIM1_CH3N` 这一个定时器功能（TIM4 不映射到 PB15）。
 *    回到 TIM1 就必须处理两件当初被刻意回避的事：
 *      ① **MOE 必须使能**，否则引脚上没有任何波形（寄存器全对也没用）；
 *      ② **互补通道要单独启动**：`HAL_TIMEx_PWMN_Start()`，
 *         而不是 `HAL_TIM_PWM_Start()`（后者只开主通道 CCxE）。
 *    这两点都写在 node1_config.h 的对应注释里，改引脚时请一并阅读。 */
static TIM_HandleTypeDef htim_motor;

static void pwm_init(void)
{
    TIM_OC_InitTypeDef oc = {0};
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* PB15 必须在**本函数内**再配一次为复用推挽：
     * 虽然 gpio_init 已配过，但 gpio_init 末尾对方向脚/STBY/蜂鸣器的写操作
     * 以及后续可能的重配置都可能影响，PWM 引脚的正确性直接决定电机能否转动，
     * 这里做一次幂等的显式配置，避免"顺序改动导致没波形"。 */
    g.Mode = GPIO_MODE_AF_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin = GPIO_PIN_15;
    HAL_GPIO_Init(GPIOB, &g);

    htim_motor.Instance = TIM1;
    htim_motor.Init.Prescaler = NODE1_MOTOR_PWM_PSC;
    htim_motor.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim_motor.Init.Period = NODE1_MOTOR_PWM_ARR;  /* 72MHz/1/3600 = 20kHz */
    htim_motor.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim_motor.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    /* 高级定时器专用字段：重复计数器为 0（每周期都产生更新事件）。
     * 不初始化它时（CubeMX 之外手写代码）HAL 会用结构体里的值，
     * 若上次是脏值会导致更新事件频率不对。 */
    htim_motor.Init.RepetitionCounter = 0u;
    if (HAL_TIM_PWM_Init(&htim_motor) != HAL_OK) {
        bsp_log("tim1 pwm init failed\r\n");
    }

    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = 0; /* ⚠️ 必须为 0：非 0 会让电机一上电就转 */
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;  /* 互补通道同相：占空比语义与主通道一致 */
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    /* 空闲状态（刹车/停机时引脚电平）。设 RESET = 输出低，配合 TB6612 的
     * PWM 输入语义（低 = 不驱动）。若不设，TIM1 在 MOE 关闭瞬间会按
     * OCIdleState 输出，可能给电机一个意外的窄脉冲。 */
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;

    (void)HAL_TIM_PWM_ConfigChannel(&htim_motor, &oc, TIM_CHANNEL_3); /* PB15 = CH3N */

    /* ⚠️① 互补通道必须用 PWMN_Start：HAL_TIM_PWM_Start 只置 CC3E（主通道），
     *      而 PB15 上的输出由 CC3NE 控制——用错函数就是"配置全对、没有波形"。
     * ⚠️② 该函数内部会调用 __HAL_TIM_MOE_ENABLE() 使能主输出（MOE）。
     *      这是 TIM1 与 TIM4 最本质的区别：高级定时器默认关闭输出。 */
    (void)HAL_TIMEx_PWMN_Start(&htim_motor, TIM_CHANNEL_3);

    bsp_log("tim1 pwm ready (PB15/CH3N, MOE on)\r\n");
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

/** bxCAN 句柄。**必须是非 static**：Core/Src/stm32f1xx_it.c 里的
 *  USB_LP_CAN1_RX0_IRQHandler 需要把 &hcan1 交给 HAL_CAN_IRQHandler，
 *  而那个中断函数在另一个编译单元里。 */
CAN_HandleTypeDef hcan1;

/** 1ms 时间基准：CAN 位定时需要精确的 APB1 频��，SysTick 兼作毫秒计数。 */
static volatile uint32_t g_millis = 0;

/** SysTick 中断里由 Core/Src/stm32f1xx_it.c 调用，推进毫秒时基。
 *
 * ⚠️ 这里**刻意不定义 SysTick_Handler**：向量表里的那个必须唯一，而 CubeMX
 * 生成的 Core/Src/stm32f1xx_it.c 已经定义了一个（并调用 HAL_IncTick）。
 * 两边同时定义会在链接期报 multiple definition，或更糟——取决于链接顺序，
 * HAL_IncTick 不再被调用，于是 HAL_Delay/HAL_GetTick 静默失效。
 * 正确做法：在 it.c 的 `USER CODE BEGIN SysTick_IRQn 1` 段里调用本函数。 */
void node1_bsp_tick_ms(void)
{
    g_millis++;
}

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

    /* ⚠️ 位时序不只要"双方一致"，还要**把采样点对齐对端**。
     *
     * 位速率公式：  位速率 = tq_clock / (1 + BS1 + BS2)
     *               tq_clock = APB1 / (Prescaler + 1) = 36MHz / (P+1)
     * 约束：        bxCAN 总 tq 数 (1+BS1+BS2) ≤ 16 且 BS2 ≥ 1
     *
     * ⭐ 当前用 **500 kbps**（协议规定值）：
     *     P=11 → tq = 36/12 = **3MHz**
     *     1 + BS1(4) + BS2(1) = **6 tq** → 3MHz/6 = **500 kbps** ✅
     *     采样点 = (1+4)/6 = **83.3%**
     *
     * ⚠️ 为什么不是 80%（对端 ESP32 的值）：**500k 下 80.0% 数学上无法命中**。
     *   - 要 80% 需 (1+BS1)/(1+BS1+BS2)=0.8，即总 tq 是 5 的倍数：5/10/15/20…
     *   - 而 tq_clock 必须是 36MHz/(P+1) 且要整除到位速率：500k × 总tq
     *   - 总 tq≤16 的合法组合只有三组能命中 500k：
     *       P=5  → 12 tq → BS1=8,BS2=3 → **75.0%**（差 5.0）
     *       P=8  →  8 tq → BS1=6,BS2=1 → 87.5%（差 7.5）
     *       P=11 →  6 tq → BS1=4,BS2=1 → **83.3%**（差 3.3）★ 最接近
     *   （P=3 → 9MHz 需总 18 tq，超过 bxCAN 的 16 tq 上限，不可行）
     *
     * ⚠️ 2026-10-04 实机已验证：**采样点不是本项目的故障原因**。
     *   节点一从 75.0% 改到 83.3% 后，对端 ESP32 的读数完全不变
     *   （仍 `re=128 rx=0`），且**降到 125kbps 后错误方向翻转成 `te=128`** ——
     *   说明问题在**物理通路**（收发器供电/共地/CAN_H-L 接反/RXD 未接），
     *   不在时序参数。**不要再靠调时序去"修"它。**
     *   参考：128kbps 下用 16 tq 可精确命中 80.0%（P=17, BS1=15, BS2=4）。 */
    hcan1.Init.Prescaler = 11;               /* APB1=36MHz → tq = 36/12 = 3MHz */
    hcan1.Init.Mode = CAN_MODE_NORMAL;
    hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;  /* 总只有 6 tq，SJW 不宜超过 1 */
    /* 采样点 = (1 + BS1) / (1 + BS1 + BS2) = (1+4)/6 = **83.3%**
     * ⚠️ BS1 只有 4 bit（最大 16 tq），网上"24tq@500k"方案在 F103 上做不到。 */
    hcan1.Init.TimeSeg1 = CAN_BS1_4TQ;
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

/** 从 CAN 硬件搬一帧进队列（中断上下文调用）。定义在本文件下方。 */
void node1_can_rx_isr_handler(void);

/** CAN RX 帧已到达 FIFO0 的 HAL 回调。
 *
 * 调用链（三步缺一不可，且中间断了不会报错）：
 *     USB_LP_CAN1_RX0_IRQHandler()            ← Core/Src/stm32f1xx_it.c（手工补，启动文件里是 weak 空实现）
 *         └─ HAL_CAN_IRQHandler(&hcan1)        ← HAL 清标志 + 派发
 *               └─ HAL_CAN_RxFifo0MsgPendingCallback()  ← 本函数
 * 由 can_init() 里的 HAL_CAN_ActivateNotification(CAN_IT_RX_FIFO0_MSG_PENDING) 使能。 */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    if (hcan == NULL || hcan->Instance != CAN1) {
        return;
    }
    node1_can_rx_isr_handler();
}

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

    /* 硬件只有**一路**电机（用户 2026-10-04 指定：PWMA=PB15 / AIN1=PB14 /
     * AIN2=PB13）。对电机 B 的请求**静默忽略**并保持 A 的状态不变：
     * 上层（node1_app.c / 掉线安全态）仍会按"两台电机"的模型同时调 A 与 B，
     * 若这里对 B 做任何动作（例如误当作 A 处理），就会出现"命令 A 却
     * 影响了正在转的电机"这类难以定位的行为。显式忽略是最安全的语义。 */
    if (id != NODE1_MOTOR_A) {
        return;
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
    /* 用 __HAL_TIM_SET_COMPARE 宏而不是 HAL_TIM_SetCompare 函数：
     * 后者在 F1 的 HAL 里不存在（是 F0/F3/F4 的 API），写了会链接失败。
     * 通道用 TIM_CHANNEL_3 —— 互补输出的占空比仍由主通道的 CCR3 决定
     * （CC3NE 只是把 OC3REF 引到 PB15 上）。 */
    __HAL_TIM_SET_COMPARE(&htim_motor, TIM_CHANNEL_3, ccr);

    HAL_GPIO_WritePin(GPIOB, (uint16_t)(1u << NODE1_MOTOR_A_IN1_PIN), in1);
    HAL_GPIO_WritePin(GPIOB, (uint16_t)(1u << NODE1_MOTOR_A_IN2_PIN), in2);
}

static void bsp_motor_enable(bool on)
{
    /* ⚠️ 这里曾经是 `#if NODE1_MOTOR_STBY_PORT == 'B'`，而宏当时写成了裸
     * 标识符 `B`（未定义 → 预处理展开为空）→ 表达式退化成 `== 'B'` →
     * **恒假** → 本函数体被整段预处理掉 → STBY 永远保持 gpio_init() 里
     * 拉低的状态 → TB6612 停在待机、输出高阻 → **电机永远不转**。
     * 而且：编译零告警（代码根本没进编译）、运行零报错（引脚就是低电平）。
     *
     * 现在不再用 #if 包住函数体：STBY 就在 GPIOB 上，包一层条件编译
     * 没有收益，只多出一类"代码被悄悄删掉"的失败模式。
     * 端口归属改由 node1_config.h 末尾的编译期断言保证。 */
    HAL_GPIO_WritePin(GPIOB, (uint16_t)(1u << NODE1_MOTOR_STBY_PIN),
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void bsp_buzzer_set(bool on)
{
    /* ⚠️ 低电平有效：意图"响" → 输出低。写反了就是上电狂叫。
     * ⚠️ 蜂鸣器在 **PA6（GPIOA）**，不是 GPIOB —— 引脚从 PB13 改到 PA6 后
     *    这里必须一起改；只改宏不改这里就是"编译通过、蜂鸣器永远不响"。 */
    const GPIO_PinState level =
#if NODE1_BUZZER_ACTIVE_LOW
        (on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#else
        (on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#endif
    HAL_GPIO_WritePin(GPIOA, (uint16_t)(1u << NODE1_BUZZER_PIN), level);
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

void node1_bsp_init_system(void)
{
    /* 时钟与 HAL 由 CubeMX 的 main() 负责，这里只做"如果没人配，我们就配"的兜底，
     * 让本文件在两种工程形态下都能工作：
     *   ① CAN_Node1（CubeMX 工程）：main.c 已调 HAL_Init + SystemClock_Config
     *      → SystemCoreClock 已是 72MHz → 这里直接返回，绝不重复配置；
     *   ② 独立裸机入口（不经 CubeMX main）：此时 SystemCoreClock 还是复位默认的
     *      8MHz（HSI），于是这里完成 72MHz 初始化。
     * 判据用 SystemCoreClock 实测值而不是"谁调用了我"，避免两处都以为自己该配。 */
    if (SystemCoreClock >= 72000000u) {
        return;
    }

    HAL_Init();

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
    clk.APB2CLKDivider = RCC_HCLK_DIV1;    /* PCLK2 = 72MHz */
    (void)HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2);
}

void node1_bsp_init(const node1_hal_t **out_hal)
{
    /* 兜底时钟（CubeMX 工程里会直接返回，见上面的注释） */
    node1_bsp_init_system();

    /* 1kHz SysTick → 既作 HAL 延时基准（HAL_IncTick），也作 g_millis 时基
     * （由 stm32f1xx_it.c 调 node1_bsp_tick_ms）。 */
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
    /* 复位前把执行器置于安全态：电机停 + STBY 拉低。 */
    bsp_motor_set(NODE1_MOTOR_A, NODE1_MOTOR_ACTION_STOP, 0u);
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
