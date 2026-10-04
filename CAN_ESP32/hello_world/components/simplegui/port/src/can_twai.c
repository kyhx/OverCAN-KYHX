/*****************************************************************************
** File: can_twai.c
** Description: 核心机 CAN 端口层实现（ESP-IDF v6.1 / ESP32-S3 内置 TWAI）
**
** ⚠️ 为什么用新版 `esp_twai.h` 而不是 `driver/twai.h`：
**   v6.1 的旧驱动头文件第一行就是
**       #warning "The legacy TWAI driver is deprecated, please use esp_twai.h"
**   本项目构建带 **-Werror**，`#warning` 会被当成错误直接中断构建。
**   （Kconfig 有 `TWAI_SUPPRESS_DEPRECATE_WARN` 可压制，但那是把问题藏起来；
**     新 API 还能直接读到 TEC/REC 与错误标志位，对"显示通信参数"这个需求更合适。）
**
** v6 新 API 的模型与旧版差别很大，有三点必须按它的规矩来：
**   ① **收帧是回调驱动的**：`on_rx_done` 回调里**不带**帧数据，
**      必须自己调 `twai_node_receive_from_isr()` 把帧取进缓冲。
**      因此本文件自建一个环形缓冲，中断入队、任务出队。
**   ② 发帧时 `twai_frame_t.buffer` 指向的**内存必须在发送完成前保持有效**
**      （驱动只存指针，不拷贝）。本文件的发送函数用**栈上的局部缓冲**，
**      并借助 `twai_node_transmit()` 的 timeout 语义在返回前完成搬运。
**   ③ 统计数据（TEC/REC/状态）来自 `twai_node_get_info()`，
**      而总线错误/仲裁丢失/ACK 错误来自 `on_error` 回调的标志位——
**      两者都要，缺一个都判断不了"总线是干扰还是断线"。
*****************************************************************************/
#include "can_twai.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "can";

/** 发送超时（ms）。绝不能无限等：总线断线时队列会一直满，
 *  主循环卡在这里 → 显示不刷新、心跳停发，一个"发不出去"演变成"核心机假死"。 */
#define CAN_TX_TIMEOUT_MS   (50)

/** 软件接收环形缓冲深度。必须是 2 的幂（索引用位与代替取模）。
 *  深度 32 的权衡：核心机 20 ms 才轮询一次，中间可能有突发遥测；
 *  太浅会丢帧，太深只是白占 RAM。 */
#define CAN_RX_RING_LEN     (32)
#define CAN_RX_RING_MASK    (CAN_RX_RING_LEN - 1)

static twai_node_handle_t s_node    = NULL;
static bool               s_ready   = false;
static int                s_last_err = 0;

/* --- 软件接收环形缓冲：on_rx_done 回调（中断）写 head，任务读 tail --- */
static proto_can_frame_t s_rx_ring[CAN_RX_RING_LEN];
static volatile uint32_t s_rx_head = 0;   /* 中断写 */
static volatile uint32_t s_rx_tail = 0;   /* 任务读 */

/* --- 统计 --- */
static volatile uint32_t s_tx_frames = 0;
static volatile uint32_t s_rx_frames = 0;
static volatile uint32_t s_tx_failed = 0;
static volatile uint32_t s_rx_missed = 0;
static volatile uint32_t s_bus_errors = 0;
static volatile uint32_t s_arb_lost   = 0;
static volatile uint32_t s_ack_errors = 0;
static volatile uint32_t s_bit_errors   = 0;
static volatile uint32_t s_stuff_errors = 0;
static volatile uint32_t s_form_errors  = 0;

int can_last_error(void) { return s_last_err; }
bool can_ready(void)     { return s_ready; }

const char *can_state_str(uint8_t state)
{
    switch (state) {
    case CAN_STATE_ACTIVE:  return "RUN";
    case CAN_STATE_WARNING: return "WARN";
    case CAN_STATE_PASSIVE: return "PASSIVE";
    case CAN_STATE_BUS_OFF: return "BUSOFF";
    default:                return "?";
    }
}

/* ---------------------------------------------------------------------------
 * 接收回调（中断上下文）
 *
 * ⚠️ 三件事必须做对：
 *  1. 回调里**没有**帧数据，必须调 twai_node_receive_from_isr() 自己取；
 *  2. 只能用 IRAM 安全的操作：不 malloc、不 printf、不加锁；
 *  3. 中断里**不做协议解析**（只搬字节），否则会拖长中断时间影响
 *     其它中断的实时性。
 * ------------------------------------------------------------------------- */
static IRAM_ATTR bool can_on_rx_done(twai_node_handle_t handle,
                                     const twai_rx_done_event_data_t *edata,
                                     void *user_ctx)
{
    uint8_t          buf[TWAI_FRAME_MAX_LEN];
    twai_frame_t     rx;
    const uint32_t   next = (s_rx_head + 1u) & CAN_RX_RING_MASK;

    (void)edata;
    (void)user_ctx;

    memset(&rx, 0, sizeof(rx));
    rx.buffer     = buf;
    rx.buffer_len = sizeof(buf);

    if (twai_node_receive_from_isr(handle, &rx) != ESP_OK) {
        return false;
    }
    /* 本协议只用 11 位标准帧。扩展帧/远程帧直接丢弃：
     * 否则畸形帧会以"看似合法的 id"进入协议层，造成难以定位的误判。 */
    if (rx.header.ide || rx.header.rtr) {
        return false;
    }
    if (next == s_rx_tail) {
        /* 环形缓冲满 → 丢帧并计数。置计数而不是静默丢弃：
         * 排查"偶尔丢遥测"时，没有这个数根本无从下手。 */
        s_rx_missed++;
        return false;
    }

    {
        proto_can_frame_t *slot = &s_rx_ring[s_rx_head];
        uint16_t           dlc  = (uint16_t)rx.header.dlc;
        if (dlc > 8u) {
            dlc = 8u;
        }
        slot->id  = (uint16_t)rx.header.id;
        slot->dlc = (uint8_t)dlc;
        memcpy(slot->data, buf, dlc);
        s_rx_head = next;
    }
    return false;   /* 不需要唤醒高优先级任务：我们靠任务侧轮询 */
}

/* 错误事件回调：把错误分类计数。
 * 分"仲裁丢失 / 无应答 / 其它总线错误"三类，因为它们指向完全不同的原因：
 *   arb_lost    → 两个节点 ID 撞车（配置问题）
 *   ack_errors  → 对端不在或终端电阻缺失（接线问题）
 *   其它        → 干扰、位速率不匹配、线太长（物理层问题）
 * 只统计一个"错误总数"是没法定位的。 */
static IRAM_ATTR bool can_on_error(twai_node_handle_t handle,
                                   const twai_error_event_data_t *edata,
                                   void *user_ctx)
{
    (void)handle;
    (void)user_ctx;
    s_bus_errors++;
    if (edata->err_flags.arb_lost) { s_arb_lost++; }
    if (edata->err_flags.ack_err)  { s_ack_errors++; }
    /* ⭐ 补记"位错误""格式错误""填充错误"。
     * 为什么必需：当 `re` 增长而 `rx=0` 时，只有区分到**具体错误类型**才能定位。
     * 结论对照（驱动 `twai_error_flags_t` 只有这 5 位，**没有专门的 CRC 位**）：
     *   bit_err    → 采样时刻读到的电平与发出的不符（位速率/采样点/信号完整性）
     *   stuff_err  → 位填充违例（连续 6 个同极性位）→ 位速率严重不匹配或干扰
     *   form_err   → 帧固定格式位违例 → 常由"无应答"间接引起
     *   ack_err    → 对端不在（接线/未上电）
     *   arb_lost   → 多节点 ID 冲突（正常总线也会有少量）
     * 实机踩坑（2026-10-04）：这些计数器**一直在采集，却忘了打印**，
     * 白白多花了两轮实验才想到要看。
     * **采集了却不输出，等于没采集。** */
    if (edata->err_flags.bit_err)   { s_bit_errors++; }
    if (edata->err_flags.stuff_err) { s_stuff_errors++; }
    if (edata->err_flags.form_err)  { s_form_errors++; }
    return false;
}

int can_init(void)
{
    twai_onchip_node_config_t cfg = {0};
    twai_event_callbacks_t    cbs = {0};
    esp_err_t                 err;

    cfg.io_cfg.tx              = (gpio_num_t)CAN_TWAI_TX_GPIO;
    cfg.io_cfg.rx              = (gpio_num_t)CAN_TWAI_RX_GPIO;
    cfg.io_cfg.quanta_clk_out  = GPIO_NUM_NC;   /* 不用时钟输出脚 */
    cfg.io_cfg.bus_off_indicator = GPIO_NUM_NC; /* 不用 Bus-Off 指示脚 */
    /* ⚠️ 位时序用 CAN_TWAI_BITRATE_BPS（**按位速率让驱动自己算时序**），
     * 而不是用 `TWAI_TIMING_CONFIG_500KBITS()` 这类**固定预设宏**。
     * 原因：那些预设是写死的（500k 用 quanta_resolution_hz=10MHz,tseg1=15,tseg2=4
     * → 采样点 80%），**改不了位速率**，联调时想做"降速判别"就必须换宏重编。
     * 直接把 bitrate 交给驱动，降速只是改一个数字（见 can_twai.h 的说明）。 */
    cfg.bit_timing.bitrate     = CAN_TWAI_BITRATE_BPS;

    /* 只听模式（诊断用，见 can_twai.h 的说明）。
     * 打开后控制器不发任何位（连错误帧都不发），因此不会干扰总线上的其它节点，
     * 可以干净地观察"对方是否真的在发帧"。 */
#if CAN_TWAI_LISTEN_ONLY
    cfg.flags.enable_listen_only = 1;
    ESP_LOGW(TAG, "TWAI 处于【只听模式】——不发送也不应答，仅供联调诊断");
#endif

#if CAN_LOOPBACK_TEST
    /* ⭐ 只置 `enable_self_test`，**不要**同时置 `enable_loopback`。
     *
     * 2026-10-04 我在这里犯过一个错：两个标志一起置，结果自发自收失败、
     * TEC 一路攀升到 Bus-Off。对照 IDF 官方示例
     * `examples/peripherals/twai/twai_error_recovery/main/twai_recovery_main.c`
     * 才发现正确写法**只置 self_test**：
     *
     *     .flags.enable_self_test = true,      // 官方只这一位
     *
     * 两者语义不同、不是叠加关系：
     *   enable_self_test —— 发送不需要外部应答（控制器自产 ACK），用于自测；
     *   enable_loopback  —— 收到的帧由自己回环，且**明确"不应答它们"**。
     * 一起置就变成"一边要求自产 ACK、一边又抑制 ACK"，自相矛盾 →
     * 发送完不成 → TEC 累加 → 最终 Bus-Off。
     * **这正是我那个回环实验失败的真正原因（是我的配置错，不是器件坏）。**
     * 详见 can_twai.h 中 CAN_LOOPBACK_TEST 的说明。 */
    cfg.flags.enable_self_test = 1;
    ESP_LOGW(TAG, ">>> 自测模式已启用（enable_self_test，自发自收不需要对端）<<<");
#endif
    /* ⚠️ fail_retry_cnt 必须**有界**，不能填 -1（无限重传）。
     *
     * 实机踩坑（2026-10-04，COM4 上实测）：最初填 -1，理由是"与节点一 bxCAN 的
     * AutoRetransmission=ENABLE 语义对齐"。**这个类比是错的**：
     *   - bxCAN 重传发生在**硬件层**，不占用软件队列，且错误计数到 256 会进
     *     Bus-Off 并自动恢复（节点自救）；
     *   - ESP32 新驱动的重传意味着**帧一直留在发送队列里不被丢弃**。
     * 后果：总线无 ACK 时（对端未上电/没接收发器），第一帧永久占住队列，
     * 之后每次发送都失败并打印：
     *     E (...) esp_twai: _node_queue_tx(617): tx queue full
     * 即**队列被一个死帧卡死**，即使总线后来恢复也发不出去。
     *
     * 改为 3 次（CAN 常见做法、与 ISO 11898 的"错误帧后重试"惯例接近）：
     * 发送失败后帧会被丢弃、TEC 递增，最终进 Bus-Off 并自动恢复 —— 与节点一
     * 的真实行为一致，且不会把队列堵住。 */
    cfg.fail_retry_cnt         = 3;
    cfg.tx_queue_depth         = CAN_TWAI_TX_QUEUE_LEN;

    err = twai_new_node_onchip(&cfg, &s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_new_node_onchip failed: %s (TX=GPIO%d RX=GPIO%d)",
                 esp_err_to_name(err), CAN_TWAI_TX_GPIO, CAN_TWAI_RX_GPIO);
        s_last_err = (int)err;
        return (int)err;
    }

    cbs.on_rx_done = can_on_rx_done;
    cbs.on_error   = can_on_error;
    err = twai_node_register_event_callbacks(s_node, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register callbacks failed: %s", esp_err_to_name(err));
        s_last_err = (int)err;
        return (int)err;
    }

    err = twai_node_enable(s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_node_enable failed: %s", esp_err_to_name(err));
        s_last_err = (int)err;
        return (int)err;
    }

    /* ⭐ 使能后**主动做一次错误状态恢复**。
     *
     * 实机发现（2026-10-04）：把应用层发送全部关掉（`CAN_TX_DISABLE=1`）、
     * 一帧都没发的情况下，诊断读数仍是 `st=PASSIVE ... te=0 re=128`——
     * 即 **`re=128` 是控制器里残留的错误状态，不是本次通信产生的**
     * （控制器上电复位后本应从 0 开始）。
     * 处于错误被动（REC≥128）的控制器难以正常参与总线，这很可能就是
     * 之前"两端各自都认为健康却互不相通"的真正原因之一。
     *
     * `twai_node_recover()` 会让控制器走一次 Bus-Off 恢复流程
     * （等待 128 次 11 个连续隐性位）并**把 TEC/REC 清零**，使收发从头开始。
     * 上电时主动做一次，代价可忽略，却能把"上次运行/异常残留"的影响清干净。
     *
     * 注：总线一直无活动时恢复流程会一直等待，因此**它的返回值不能当作失败**，
     * 这里只做一次、不看结果，把状态留给诊断日志去反映。 */
    (void)twai_node_recover(s_node);

    s_ready = true;
#if CAN_TX_DISABLE
    /* 这一行是"开关生效"的证据。判读读数前**必须先看到它**。 */
    ESP_LOGW(TAG, ">>> 纯接收诊断模式已启用：应用层不发任何帧（CAN_TX_DISABLE=1） <<<");
#endif
    ESP_LOGI(TAG, "TWAI ready: %u bps, TX=GPIO%d RX=GPIO%d, txq=%d rxring=%d",
             (unsigned)CAN_TWAI_BITRATE_BPS, CAN_TWAI_TX_GPIO, CAN_TWAI_RX_GPIO,
             CAN_TWAI_TX_QUEUE_LEN, CAN_RX_RING_LEN);
    return 0;
}

int can_send(const proto_can_frame_t *frame)
{
    uint8_t      buf[TWAI_FRAME_MAX_LEN];   /* 栈缓冲：驱动只存指针，见文件头 ② */
    twai_frame_t tx;
    esp_err_t    err;

#if CAN_TX_DISABLE
    /* 纯接收诊断模式：应用层不发任何帧（见 can_twai.h 的说明）。
     * 注意这里**必须**在函数最前面返回，连计数都不增加 ——
     * 否则 `tx` 仍会增长，又变成"看起来发了其实没发"的不可判读状态。 */
    (void)frame;
    (void)buf;
    (void)tx;
    (void)err;
    return 0;
#endif

    if (NULL == frame || frame->dlc > 8u) {
        return -1;
    }
    if (!s_ready) {
        return -2;
    }

    memset(&tx, 0, sizeof(tx));
    memset(buf, 0, sizeof(buf));
    memcpy(buf, frame->data, frame->dlc);

    tx.header.id  = frame->id;
    tx.header.dlc = frame->dlc;
    tx.header.ide = 0;   /* 标准帧 */
    tx.header.rtr = 0;   /* 数据帧 */
    tx.header.fdf = 0;   /* 经典 CAN，非 FD */
    tx.buffer     = buf;
    tx.buffer_len = frame->dlc;

    err = twai_node_transmit(s_node, &tx, CAN_TX_TIMEOUT_MS);
    if (err != ESP_OK) {
        s_tx_failed++;
        s_last_err = (int)err;
        return (int)err;
    }
    s_tx_frames++;
    return 0;
}

/** 环形缓冲索引的临界区锁。
 *
 *  ⚠️ 这里**不能**用 Cortex-M 那套 `__get_PRIMASK()/__disable_irq()`：
 *  那是 ARM CMSIS 的内建函数，ESP32-S3 是 **Xtensa** 内核，编译期直接报
 *  "implicit declaration of function '__get_PRIMASK'"。
 *  跨平台要按各自的内核 API 走，此处用 FreeRTOS 的 portMUX。 */
static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;

bool can_recv(proto_can_frame_t *out)
{
    bool got = false;

    if (NULL == out || !s_ready) {
        return false;
    }

    /* 临界区保护 tail/head 的一致性。区里只做一次结构体拷贝（12 字节），
     * 对中断延迟影响可忽略。 */
    portENTER_CRITICAL(&s_rx_mux);
    if (s_rx_head != s_rx_tail) {
        *out      = s_rx_ring[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1u) & CAN_RX_RING_MASK;
        got       = true;
    }
    portEXIT_CRITICAL(&s_rx_mux);

    if (got) {
        s_rx_frames++;
    }
    return got;
}

bool can_tx_idle(void)
{
    twai_node_status_t st;

    if (!s_ready) {
        return false;
    }
    if (twai_node_get_info(s_node, &st, NULL) != ESP_OK) {
        return false;   /* 取不到状态就当作"忙"，宁可少发不要乱发 */
    }
    /* tx_queue_depth 是我们配置的固定值，剩余量等于它即说明队列已空。
     * 用 >= 而不是 == ：驱动若改变"剩余"的计数口径也不会误判。 */
    return (st.tx_queue_remaining >= (uint32_t)CAN_TWAI_TX_QUEUE_LEN);
}

void can_get_stats(can_stats_t *out)
{
    twai_node_status_t st;

    if (NULL == out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->installed = s_ready;
    out->tx_frames = s_tx_frames;
    out->rx_frames = s_rx_frames;
    out->tx_failed = s_tx_failed;
    out->rx_missed = s_rx_missed;
    out->bus_errors = s_bus_errors;
    out->arb_lost   = s_arb_lost;
    out->ack_errors = s_ack_errors;
    out->bit_errors   = s_bit_errors;
    out->stuff_errors = s_stuff_errors;
    out->form_errors  = s_form_errors;
    out->arb_lost   = s_arb_lost;

    if (!s_ready) {
        return;
    }
    /* TEC/REC/状态只有驱动知道，必须现取（这正是判断总线健康的关键证据）。
     * 取不到时保留上面的计数，不把界面清零。 */
    if (twai_node_get_info(s_node, &st, NULL) != ESP_OK) {
        return;
    }
    out->state    = (uint8_t)st.state;
    out->tx_error = st.tx_error_count;
    out->rx_error = st.rx_error_count;
}

#else  /* 非 ESP-IDF：占位，保证 PC 侧静态检查能过 */

int  can_init(void) { return -1; }
bool can_ready(void) { return false; }
int  can_last_error(void) { return -1; }
const char *can_state_str(uint8_t s) { (void)s; return "?"; }
int  can_send(const proto_can_frame_t *f) { (void)f; return -1; }
bool can_recv(proto_can_frame_t *o) { (void)o; return false; }
bool can_tx_idle(void) { return false; }
void can_get_stats(can_stats_t *o) { if (o) { memset(o, 0, sizeof(*o)); } }

#endif /* ESP_PLATFORM */
