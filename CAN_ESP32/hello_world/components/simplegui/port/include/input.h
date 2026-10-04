/*****************************************************************************
** File: input.h
** Description: 按键（3 个）与 EC11 旋转编码器的输入驱动
**      平台：ESP32-S3 / ESP-IDF v6.x
**      接线（docs/引脚分配.md §2.1）：
**        按键（**低有效**，按下接地）：
**          BTN1 = GPIO8    → 上一项 / 上一个
**          BTN2 = GPIO12   → 下一项 / 下一个
**          BTN3 = GPIO47   → 确认（长按 = 返回/取消）
**        EC11 旋转编码器：
**          A 相 = GPIO6
**          B 相 = GPIO7
**          （编码器中间按键本项目未使用；如需使用需另占一个 GPIO）
**
** 两个设计决定（都有踩坑背景）：
**
** 1) 编码器用 **PCNT 硬件计数** 而不是 GPIO 中断/软件状态机。
**    EC11 每转过一格会产生多次电平跳变（机械触点抖动 + 四分之一周期），
**    20 ms 的轮询粒度一定会漏计数；而 PCNT 由硬件计数，我们只需定期"取走"
**    累计值即可，既不丢步也不需要消抖延时。
**
** 2) 按键**全部使能内部上拉**。GPIO8/12/47 复位后都是"输入使能、无上下拉"
**    的悬空状态（见 docs/引脚分配.md R-2），不加上拉会读到随机电平。
**    按键按下接地 → 低电平；内部上拉约 45k，配合软件消抖足够。
*****************************************************************************/
#ifndef __INPUT_H__
#define __INPUT_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 按键编号 */
typedef enum {
    INPUT_BTN_1 = 0,   /* GPIO8  — 上一项 */
    INPUT_BTN_2,       /* GPIO12 — 下一项 */
    INPUT_BTN_3        /* GPIO47 — 确认（长按 = 返回） */
} input_button_t;

/* 一个采样周期内发生的事件。
 * 一次可能出现多个事件（如同时按两个键 + 转一格），因此数量用计数表示。 */
typedef struct {
    uint8_t btn1_pressed;   /* 1 = BTN1 本次被按下（上升沿，已消抖） */
    uint8_t btn2_pressed;
    uint8_t btn3_pressed;
    uint8_t btn3_long;      /* 1 = BTN3 长按判定成立（见 INPUT_LONG_PRESS_TICKS） */
    int8_t  encoder_delta;  /* 编码器净步数：正 = 一个方向，负 = 另一个；0 = 未转动 */
} input_event_t;

/* 引脚定义（与 docs/引脚分配.md 一一对应；若改板改这里） */
#define INPUT_BTN1_GPIO      (8)
#define INPUT_BTN2_GPIO      (12)
#define INPUT_BTN3_GPIO      (47)
#define INPUT_ENC_A_GPIO     (6)
#define INPUT_ENC_B_GPIO     (7)

/* 长按判定：连续 N 个采样周期都是按下状态即算长按。
 * 采样周期见 main 的 INPUT_TICK_MS（当前 20 ms）→ 40 × 20 ms = 800 ms。 */
#define INPUT_LONG_PRESS_TICKS   (40)

/**
 * @brief 初始化按键 GPIO（输入 + 内部上拉）与编码器 PCNT 单元。
 * @return 0 成功；非 0 为 ESP-IDF esp_err_t
 */
int  input_init(void);

/**
 * @brief 取走自上次调用以来累积的输入事件。
 *
 * 设计成"拉取"而不是回调：调用方（主循环）本来就有固定节奏，
 * 回调会把 GUI 调用散落到多个上下文里，反而更难推理。
 * 内部读走 PCNT 计数并清零，因此不会重复报告同一次转动。
 *
 * @param out 输出；不可为 NULL（会先清零）
 * @return true 表示 out 中有事件，false 表示本周期无输入
 */
bool input_poll(input_event_t *out);

/** @return 最近一次底层调用失败的错误码（ESP-IDF esp_err_t；0 = 无错） */
int  input_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* __INPUT_H__ */
