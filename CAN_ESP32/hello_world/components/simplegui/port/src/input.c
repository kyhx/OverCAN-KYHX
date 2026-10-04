/*****************************************************************************
** File: input.c
** Description: 按键 + EC11 编码器驱动实现（ESP-IDF v6.x）
**
** 为什么按键与编码器用两套完全不同的机制：
**   按键是人手按的，抖动频率低、动作是一次性的 → **软件消抖 + 边沿检测** 足够；
**   编码器是机械旋转，每格产生 2~4 次电平跳变 → 必须 **PCNT 硬件计数**，
**   否则 20 ms 轮询必然漏步（这是编码器最典型的读数跳变来源）。
**
** PCNT 计数与"每格一步"的换算：
**   EC11 典型每格 = 1 个完整正交周期 = **4 个计数**。
**   因此把 PCNT 的高/低限设成 ±2：累计到 2 就产生一次高限事件（+1 步），
**   到 -2 产生一次低限事件（-1 步）。这样正好"每格一步"，
**   既不过灵（半格就动）也不过钝（两格才动）。
**   ⚠️ 若你的编码器是每格 2 计数（部分型号），把 INPUT_ENC_STEP_COUNTS 改成 1。
*****************************************************************************/
#include "input.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "input";

/* 一个"格"对应的 PCNT 计数。
 * EC11 每转过一格 = **1 个完整正交周期 = 4 个计数**（A/B 两相的 4 条边沿）。
 * 因此高/低限设 ±4：正好"每格一步"。
 * ⚠️ 设成 2 会让半格就触发一次 → 手感变成"转一格走两屏"，
 *    而且机械抖动产生的半格会被误判成真实转动。 */
#define INPUT_ENC_STEP_COUNTS   (4)

/* 毛刺滤波门限（纳秒）。
 * ⚠️ 有硬件上限！阈值 = APB_MHz(80) x ns / 1000，且必须 <= PCNT_LL_MAX_GLITCH_WIDTH(1023)，
 *    即**最大约 12.7 µs**。最初写成 1 ms（1,000,000 ns）→ 阈值 80000 → 越界报
 *    "glitch width out of range"（ESP_ERR_INVALID_ARG），滤波完全没生效。
 * 这里取 1 µs：能把触点抖动的窄脉冲滤掉，又远小于人手转动的边沿间隔。 */
#define INPUT_ENC_GLITCH_NS     (1000)

typedef struct {
    int             gpio;
    bool            raw_last;      /* 上一次采样电平（true = 按下） */
    uint8_t         stable_cnt;    /* 连续相同采样的次数，用于消抖 */
    bool            stable;        /* 消抖后的稳定状态 */
    uint16_t        hold_ticks;    /* 稳定按下持续的采样周期数 */
    bool            long_reported; /* 本轮长按是否已上报，避免连发 */
} btn_state_t;

static btn_state_t          s_btns[3];
static pcnt_unit_handle_t   s_pcnt_unit = NULL;
static int                  s_enc_accum  = 0;   /* 由 PCNT 事件累加的净步数 */
static int                  s_last_err   = 0;

/* ---------------------------- PCNT 事件回调 ------------------------------ */

/* 高/低限事件在中断上下文里累加"步数"，主循环再一次性取走。
 * 这样即使主循环某一拍卡顿，也不会丢掉已经发生的转动。 */
static bool IRAM_ATTR pcnt_on_reach(pcnt_unit_handle_t unit,
                                    const pcnt_watch_event_data_t *edata,
                                    void *user_ctx)
{
    (void)unit;
    (void)user_ctx;
    s_enc_accum += (edata->watch_point_value > 0) ? 1 : -1;
    return false;   /* false = 不触发任务切换，保持中断最短 */
}

int input_last_error(void)
{
    return s_last_err;
}

/* ------------------------------- 按键 ------------------------------------ */

static void btn_config(int idx, int gpio)
{
    gpio_config_t cfg = {0};
    cfg.pin_bit_mask = (1ULL << gpio);
    cfg.mode         = GPIO_MODE_INPUT;
    /* ⚠️ 三个按键引脚复位后都是悬空输入，**必须**上拉，否则读数为随机值 */
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;   /* 用轮询 + 软件消抖，不起中断 */
    (void)gpio_config(&cfg);

    s_btns[idx].gpio          = gpio;
    s_btns[idx].raw_last      = false;
    s_btns[idx].stable_cnt    = 0;
    s_btns[idx].stable        = false;
    s_btns[idx].hold_ticks    = 0;
    s_btns[idx].long_reported = false;
}

static int btn_poll(int idx, bool *out_pressed, bool *out_long)
{
    /* 低电平 = 按下（按键一端接地、另一端 GPIO 且内部上拉） */
    const bool raw = (gpio_get_level(s_btns[idx].gpio) == 0);

    *out_pressed = false;
    *out_long    = false;

    /* 连续 2 次采样一致才认账 → 20 ms 周期下约 40 ms 消抖窗口。
     * 机械按键抖动典型 5~10 ms，40 ms 足够且不影响手感。 */
    if (raw == s_btns[idx].raw_last) {
        if (s_btns[idx].stable_cnt < 255) {
            s_btns[idx].stable_cnt++;
        }
    } else {
        s_btns[idx].raw_last   = raw;
        s_btns[idx].stable_cnt = 1;
    }

    if ((s_btns[idx].stable_cnt >= 2) && (s_btns[idx].stable != raw)) {
        s_btns[idx].stable = raw;
        if (raw) {
            /* 按下沿：立即上报一次；长按计时从这里开始 */
            *out_pressed              = true;
            s_btns[idx].hold_ticks    = 0;
            s_btns[idx].long_reported = false;
        } else {
            s_btns[idx].hold_ticks = 0;
        }
    }

    if (s_btns[idx].stable && !s_btns[idx].long_reported) {
        if (s_btns[idx].hold_ticks < 0xFFFF) {
            s_btns[idx].hold_ticks++;
        }
        if (s_btns[idx].hold_ticks >= INPUT_LONG_PRESS_TICKS) {
            *out_long                 = true;
            s_btns[idx].long_reported = true;   /* 只报一次，不连发 */
        }
    }
    return 0;
}

/* ------------------------------ 编码器 ----------------------------------- */

static int encoder_init(void)
{
    pcnt_unit_config_t unit_cfg = {0};
    pcnt_chan_config_t chan_cfg = {0};
    pcnt_channel_handle_t chan_a = NULL;
    pcnt_event_callbacks_t cbs = {0};
    esp_err_t err;

    /* 高/低限设 ±2 → 每格一步（见文件头换算说明） */
    unit_cfg.high_limit = INPUT_ENC_STEP_COUNTS;
    unit_cfg.low_limit  = -INPUT_ENC_STEP_COUNTS;

    err = pcnt_new_unit(&unit_cfg, &s_pcnt_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_unit failed: %s", esp_err_to_name(err));
        s_last_err = (int)err;
        return (int)err;
    }

    /* 单通道同时接 A/B 两相：edge/level 组合即正交解码。
     * 这是 ESP-IDF PCNT 解码旋转编码器的标准做法。 */
    chan_cfg.edge_gpio_num  = INPUT_ENC_A_GPIO;
    chan_cfg.level_gpio_num = INPUT_ENC_B_GPIO;
    err = pcnt_new_channel(s_pcnt_unit, &chan_cfg, &chan_a);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_channel failed: %s", esp_err_to_name(err));
        s_last_err = (int)err;
        return (int)err;
    }

    /* 只用一个通道：A 相做边沿计数、B 相做方向判定（PCNT 的标准正交接法）。
     * 想再提高分辨率（4 倍频）可以给 B 相也建一个通道，本项目不需要。 */
    (void)pcnt_channel_set_edge_action(chan_a,
                PCNT_CHANNEL_EDGE_ACTION_DECREASE,   /* 正沿 */
                PCNT_CHANNEL_EDGE_ACTION_INCREASE);  /* 负沿 */
    (void)pcnt_channel_set_level_action(chan_a,
                PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    /* 毛刺滤波：窄于阈值的脉冲（触点抖动）不计入。
     * ⚠️ 新版 IDF 的这个 API 收的是**结构体指针**（`max_glitch_ns`，单位纳秒），
     * 不是旧版的裸整数；写成整数会报 "makes pointer from integer without a cast"。
     * 门限上限见 INPUT_ENC_GLITCH_NS 的说明（约 12.7 µs），超了会 ESP_ERR_INVALID_ARG。 */
    {
        pcnt_glitch_filter_config_t filter_cfg = {0};
        filter_cfg.max_glitch_ns = INPUT_ENC_GLITCH_NS;
        err = pcnt_unit_set_glitch_filter(s_pcnt_unit, &filter_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "glitch filter failed: %s (max_glitch_ns=%u)",
                     esp_err_to_name(err), (unsigned)INPUT_ENC_GLITCH_NS);
        }
    }

    cbs.on_reach = pcnt_on_reach;
    err = pcnt_unit_register_event_callbacks(s_pcnt_unit, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcnt register callback failed: %s", esp_err_to_name(err));
        s_last_err = (int)err;
        return (int)err;
    }

    (void)pcnt_unit_add_watch_point(s_pcnt_unit, INPUT_ENC_STEP_COUNTS);
    (void)pcnt_unit_add_watch_point(s_pcnt_unit, -INPUT_ENC_STEP_COUNTS);

    err = pcnt_unit_enable(s_pcnt_unit);
    if (err == ESP_OK) {
        err = pcnt_unit_start(s_pcnt_unit);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcnt enable/start failed: %s", esp_err_to_name(err));
        s_last_err = (int)err;
        return (int)err;
    }

    ESP_LOGI(TAG, "encoder PCNT ready: A=GPIO%d B=GPIO%d, %d counts/step",
             INPUT_ENC_A_GPIO, INPUT_ENC_B_GPIO, INPUT_ENC_STEP_COUNTS);
    return 0;
}

/* ------------------------------- 对外接口 -------------------------------- */

int input_init(void)
{
    btn_config(INPUT_BTN_1, INPUT_BTN1_GPIO);
    btn_config(INPUT_BTN_2, INPUT_BTN2_GPIO);
    btn_config(INPUT_BTN_3, INPUT_BTN3_GPIO);

    ESP_LOGI(TAG, "buttons ready: BTN1=GPIO%d BTN2=GPIO%d BTN3=GPIO%d (active low)",
             INPUT_BTN1_GPIO, INPUT_BTN2_GPIO, INPUT_BTN3_GPIO);

    return encoder_init();
}

bool input_poll(input_event_t *out)
{
    bool any = false;
    bool p1 = false, p2 = false, p3 = false;
    bool l1 = false, l2 = false, l3 = false;

    if (NULL == out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    /* 每个按键用**各自独立**的输出变量：共用一个变量时，后一次调用会覆盖
     * 前一次的结果，一旦将来给 BTN1/BTN2 也加长按就会静默出错。 */
    (void)btn_poll(INPUT_BTN_1, &p1, &l1);
    (void)btn_poll(INPUT_BTN_2, &p2, &l2);
    (void)btn_poll(INPUT_BTN_3, &p3, &l3);
    (void)l1;
    (void)l2;

    if (p1) { out->btn1_pressed = 1; any = true; }
    if (p2) { out->btn2_pressed = 1; any = true; }
    if (p3) { out->btn3_pressed = 1; any = true; }
    if (l3) { out->btn3_long    = 1; any = true; }

    /* 取走累积的编码器步数（PCNT 回调里累加，这里原子取走） */
    if (0 != s_enc_accum) {
        out->encoder_delta = (int8_t)((s_enc_accum > 127) ? 127
                                    : ((s_enc_accum < -128) ? -128 : s_enc_accum));
        s_enc_accum = 0;
        any = true;
    }
    return any;
}

#else  /* 非 ESP-IDF：占位，保证 PC 侧静态检查能过 */

int  input_init(void) { return -1; }
bool input_poll(input_event_t *o) { if (o) { memset(o, 0, sizeof(*o)); } return false; }
int  input_last_error(void) { return -1; }

#endif /* ESP_PLATFORM */
