/*****************************************************************************
** File: sgui_demo.c
** Description: 核心机（ESP32-S3）的 SimpleGUI 界面层。
**
** 只有两屏 —— 已清理掉移植验证用的"基础绘图"和"组件演示"两屏：
**   Screen 0  DASHBOARD : 上电默认屏（陀螺仪 + 总线健康度 + 运行信息）
**   Screen 1  IMU       : 陀螺仪专页（三轴角速度 + 温度）
**
** ⚠️ 屏幕 ID 必须与 s_arrpstScreens[] 的下标一致：HMI_GoToScreen(i) 是按
**    下标直接索引数组的，ID 与下标错位会切到错误的屏（且不报错）。
**
** 分层约定：本文件**不直接调用任何 esp_* API**。平台数值（总线状态、堆量、
** WHO_AM_I）一律由主循环经 SGUI_DEMO_Set*() 灌入 —— 这样界面层保持零平台依赖，
** 与 firmware/node1 的 HAL 分层原则一致。
*****************************************************************************/
#include "sgui_demo.h"
#include "SGUI_Basic.h"
#include "SGUI_Text.h"
#include "SGUI_Common.h"
#include "SGUI_FontResource.h"
#include "sgui_font_gb2312.h"
#include "screen.h"
#include "can_twai.h"
#include "proto_id.h"

#include <stdint.h>

/*------------------------------------------------------------ Private ---*/
/* --- 陀螺仪数据的状态 -----------------------------------------------------
 * 角速度用"毫度每秒"(0.001 °/s) 存整数：0.001 的分辨率对 °/s 量级足够（可显示
 * 到 0.01 °/s），且避免浮点、便于定点显示。下标 0/1/2 = X/Y/Z。 */
static int32_t               s_aiGyroMdps[3] = {0, 0, 0};
static int32_t               s_iTempMdegC    = 0;
/* 是否已收到过有效数据。没数据时显示 "----" 而不是 0.00 —— 0.00 会让人以为
 * "陀螺仪正常且静止"，而实际可能根本没读到（I2C 失败/未接线），这是两种
 * 完全不同的故障，界面必须能区分。 */
static SGUI_BOOL             s_bImuValid     = 0;
/* 最近一次 IMU 读取的错误码（0 = 无错），显示在屏幕上便于现场排错。 */
static int32_t               s_iImuErr       = 0;

/* --- 仪表盘用的平台状态 --------------------------------------------------- */
static SGUI_INT              s_iOledOk       = -1;   /* -1 = 未知（还没探测） */
static SGUI_INT              s_iImuOk        = -1;
static uint32_t              s_uiUptimeSec   = 0;
static uint32_t              s_uiFreeHeapKB  = 0;
static SGUI_INT              s_iImuId        = 0;    /* 实测 WHO_AM_I，0 = 未知 */

/* --- CAN 屏的状态 ---------------------------------------------------------
 * 由主循环通过 SGUI_DEMO_SetCanStats() / SetNode1Status() 灌入。 */
static can_stats_t           s_stCan         = {0};
static SGUI_INT              s_iN1Online     = 0;
static int32_t               s_iN1TempMdegC  = 0;
static SGUI_INT              s_iN1Status     = 0;
static uint32_t              s_uiN1Frames    = 0;
/* CAN 屏节流：CAN 计数变化很快，但 128x64 的 I2C 刷新不便宜。
 * 500 ms 一次既看得出"是否在通信"，又不会把 I2C 占满影响其它屏。 */
#define SGUI_DEMO_CAN_REFRESH_TICKS  (25)   /* 25 x 20 ms = 500 ms */
static SGUI_INT              s_iCanTicks     = 0;

/* 空闲计时：SGUI_DEMO_Process() 每被调用一次记一拍（主循环 20 ms 一拍）。
 * 超过 SGUI_DEMO_IDLE_RETURN_TICKS 没收到任何按键就自动回默认屏。 */
#define SGUI_DEMO_IDLE_RETURN_TICKS   (1500)   /* 1500 x 20 ms = 30 s */
static SGUI_INT              s_iIdleTicks   = 0;

/* Character bitmap scratch buffer.
 * SimpleGUI reads one glyph bitmap at a time out of the font resource into
 * this buffer, then blits it to the screen. It MUST be a real buffer: with a
 * NULL pointer SGUI_Text_GetCharacterData returns 0 bytes and every glyph
 * comes out blank. Largest case is the 16px font:
 *   SGUI_USED_BYTE(16) * 16 = 2 * 16 = 32 bytes.
 * 64 bytes leaves comfortable headroom. */
#define SGUI_DEMO_CHAR_BUF_SIZE   (64)
static SGUI_BYTE          s_aucCharBitmap[SGUI_DEMO_CHAR_BUF_SIZE];

/*-------------------------------------------------- Forward declarations --*/
static HMI_ENGINE_RESULT SGUI_DemoDash_Initialize  (SGUI_SCR_DEV* pstDeviceIF);
static HMI_ENGINE_RESULT SGUI_DemoDash_Prepare     (SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters);
static HMI_ENGINE_RESULT SGUI_DemoDash_Repaint     (SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters);
static HMI_ENGINE_RESULT SGUI_DemoDash_PostProcess (SGUI_SCR_DEV* pstDeviceIF, HMI_ENGINE_RESULT eProcResult, SGUI_INT iActionID);

static HMI_ENGINE_RESULT SGUI_DemoImu_Initialize   (SGUI_SCR_DEV* pstDeviceIF);
static HMI_ENGINE_RESULT SGUI_DemoImu_Prepare      (SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters);
static HMI_ENGINE_RESULT SGUI_DemoImu_Repaint      (SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters);
static HMI_ENGINE_RESULT SGUI_DemoImu_ProcessEvent (SGUI_SCR_DEV* pstDeviceIF, const HMI_EVENT_BASE* pstEvent, SGUI_INT* piActionID);
static HMI_ENGINE_RESULT SGUI_DemoImu_PostProcess  (SGUI_SCR_DEV* pstDeviceIF, HMI_ENGINE_RESULT eProcResult, SGUI_INT iActionID);

static HMI_ENGINE_RESULT SGUI_DemoCan_Initialize   (SGUI_SCR_DEV* pstDeviceIF);
static HMI_ENGINE_RESULT SGUI_DemoCan_Prepare      (SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters);
static HMI_ENGINE_RESULT SGUI_DemoCan_Repaint      (SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters);
static HMI_ENGINE_RESULT SGUI_DemoCan_ProcessEvent (SGUI_SCR_DEV* pstDeviceIF, const HMI_EVENT_BASE* pstEvent, SGUI_INT* piActionID);
static HMI_ENGINE_RESULT SGUI_DemoCan_PostProcess  (SGUI_SCR_DEV* pstDeviceIF, HMI_ENGINE_RESULT eProcResult, SGUI_INT iActionID);

/*------------------------------------------------------ Screen table -----*/
/* Screen 0：仪表盘（上电默认屏）。无屏内交互，故 ProcessEvent 为 NULL。 */
static HMI_SCREEN_ACTION  s_stDashActions  =
{
    SGUI_DemoDash_Initialize,
    SGUI_DemoDash_Prepare,
    SGUI_DemoDash_Repaint,
    NULL,
    SGUI_DemoDash_PostProcess
};
static HMI_SCREEN_OBJECT  s_stDashScreen   = {SGUI_DEMO_SCR_DASHBOARD, &s_stDashActions, NULL};

/* Screen 1：陀螺仪专页 */
static HMI_SCREEN_ACTION  s_stImuActions   =
{
    SGUI_DemoImu_Initialize,
    SGUI_DemoImu_Prepare,
    SGUI_DemoImu_Repaint,
    SGUI_DemoImu_ProcessEvent,
    SGUI_DemoImu_PostProcess
};
static HMI_SCREEN_OBJECT  s_stImuScreen    = {SGUI_DEMO_SCR_IMU, &s_stImuActions, NULL};

/* Screen 2：CAN 通信参数 + 节点一状态 */
static HMI_SCREEN_ACTION  s_stCanActions   =
{
    SGUI_DemoCan_Initialize,
    SGUI_DemoCan_Prepare,
    SGUI_DemoCan_Repaint,
    SGUI_DemoCan_ProcessEvent,
    SGUI_DemoCan_PostProcess
};
static HMI_SCREEN_OBJECT  s_stCanScreen    = {SGUI_DEMO_SCR_CAN, &s_stCanActions, NULL};

/* ⚠️ 下标必须等于 SGUI_DEMO_SCR_xxx 的值（见文件头说明） */
static HMI_SCREEN_OBJECT* s_arrpstScreens[] =
{
    &s_stDashScreen,     /* 下标 0 = SGUI_DEMO_SCR_DASHBOARD */
    &s_stImuScreen,      /* 下标 1 = SGUI_DEMO_SCR_IMU       */
    &s_stCanScreen       /* 下标 2 = SGUI_DEMO_SCR_CAN       */
};
static HMI_ENGINE_OBJECT  s_stEngine       = {0};

/*********************************************************** Screen 0 ====*/
/* 仪表盘（上电默认屏）
 *
 * 布局（128x64，字库 6x12）：
 *   y=0..12   标题栏（反白）："OverCAN Core"
 *   ┌ 边框 y=14..63，圆角 3 ┐
 *      y=16/28/40  陀螺仪 X/Y/Z 角速度 + 单位 d/s
 *      分隔线 y=51
 *      y=53  I2C 状态 / 温度（或 IDxx / no IMU） / 剩余堆内存
 *   └───────────────────────┘
 *
 * 为什么把陀螺仪放主位：这是当前唯一**真实可用**的传感器数据。
 * I2C 状态那行是"现场排错的第一手信息"——一块黑屏或一个错的读数，
 * 先看这行就能分清"总线没通"还是"界面画错了"。
 * ------------------------------------------------------------------------ */
static HMI_ENGINE_RESULT SGUI_DemoDash_Initialize(SGUI_SCR_DEV* pstDeviceIF)
{
    (void)pstDeviceIF;
    return HMI_RET_NORMAL;
}

static HMI_ENGINE_RESULT SGUI_DemoDash_Prepare(SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters)
{
    (void)pstParameters;
    SGUI_Basic_ResetMask(pstDeviceIF);
    return HMI_RET_NORMAL;
}

/* 把"陀螺仪三轴 + 状态"画一遍。供 Repaint 与主循环刷新共用。 */
static void SGUI_DemoDash_Paint(SGUI_SCR_DEV* pstDeviceIF)
{
    SGUI_CHAR szValue[16];
    SGUI_INT  i;
    static const char* const s_szAxis[3] = { "X", "Y", "Z" };

    SGUI_Basic_ResetMask(pstDeviceIF);

    /* 标题栏（反白）。全 ASCII —— 不依赖中文字库。
     * 注意 SGUI_DRAW_REVERSE 是把**已有像素取反**，所以要先铺一块前景色矩形。 */
    SGUI_Basic_DrawRectangle1(pstDeviceIF, 0, 0, 128, 13,
                SGUI_COLOR_FRGCLR, SGUI_COLOR_FRGCLR);
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"OverCAN Core", &SGUI_FONT_GB2312,
                2, 1, SGUI_DRAW_REVERSE);

    /* 内容边框：给界面一个"仪表"的视觉边界，同时把标题与内容分开 */
    SGUI_Basic_DrawRoundedRectangle(pstDeviceIF, 0, 14, 128, 50, 3,
                SGUI_COLOR_FRGCLR, SGUI_COLOR_TRANS);

    /* --- 陀螺仪三轴（主信息）---
     * 宽度核算（6x12 字库）：轴标签 x=4..9、数值起 x=24（8 字符 → 24..71）、
     * 单位 "d/s" 起 x=101（→ 101..118）。总宽 118 < 124（边框内），不重叠。
     * 数值字段刻意留 8 字符：负号 + 三位整数 + 小数点 + 两位小数是极限，
     * 留窄了负值会把数字推到单位上（这是最容易出现的一类"看着像没数值"）。 */
    for(i = 0; i < 3; i++)
    {
        const SGUI_INT iY = 16 + i * 12;
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)s_szAxis[i],
                    &SGUI_FONT_GB2312, 4, iY, SGUI_DRAW_NORMAL);
        if(0 == s_bImuValid)
        {
            SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"    ----",
                        &SGUI_FONT_GB2312, 24, iY, SGUI_DRAW_NORMAL);
            continue;
        }
        /* 0.001 °/s 存整数 → 按 2 位小数显示（/10 再交给定点格式化） */
        SGUI_Common_IntegerToStringWithDecimalPoint(
                    (SGUI_INT)(s_aiGyroMdps[i] / 10), 2, szValue, 8, ' ');
        SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312,
                    24, iY, SGUI_DRAW_NORMAL);
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"d/s", &SGUI_FONT_GB2312,
                    101, iY, SGUI_DRAW_NORMAL);
    }

    /* 分隔线 */
    SGUI_Basic_DrawLine(pstDeviceIF, 3, 51, 124, 51, SGUI_COLOR_FRGCLR);

    /* --- 状态行：I2C 健康度 + 温度 + 剩余堆 ---
     * 行宽核算：状态 x=4（最多 8 字符 → 4..51）、温度 x=57（5 字符 → 57..86）、
     * "C" x=87、剩余堆 x=99（3 字符 → 99..116）、"K" x=120。
     * 剩余堆压到 3 字符是因为再宽就越过 128px 边界。 */
    if(s_iOledOk < 0)
    {
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"I2C --",
                    &SGUI_FONT_GB2312, 4, 53, SGUI_DRAW_NORMAL);
    }
    else if((0 == s_iOledOk) || (0 == s_iImuOk))
    {
        /* 任一设备没应答都算异常，并且把"哪一路"用符号区分开：
         * 只写 "ERR" 会让人分不清是屏的问题还是陀螺仪的问题。 */
        SGUI_Text_DrawText(pstDeviceIF,
                    (SGUI_CSZSTR)((0 == s_iOledOk) ? "I2C !OLED" : "I2C !IMU"),
                    &SGUI_FONT_GB2312, 4, 53, SGUI_DRAW_NORMAL);
    }
    else
    {
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"I2C OK",
                    &SGUI_FONT_GB2312, 4, 53, SGUI_DRAW_NORMAL);
    }

    if(0 != s_bImuValid)
    {
        SGUI_Common_IntegerToStringWithDecimalPoint(
                    (SGUI_INT)(s_iTempMdegC / 100), 1, szValue, 5, ' ');
        SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312,
                    57, 53, SGUI_DRAW_NORMAL);
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"C", &SGUI_FONT_GB2312,
                    87, 53, SGUI_DRAW_NORMAL);
    }
    else if(0 != s_iImuId)
    {
        /* IMU 在总线上（有 ID）但数据读不出来 → 把 WHO_AM_I 显示出来。
         * 这能立刻区分"没接上/地址错"（下面那支）与"接上了但兼容片"（这支）。 */
        static const char HEX[] = "0123456789ABCDEF";
        SGUI_CHAR szId[5];
        szId[0] = 'I';
        szId[1] = 'D';
        szId[2] = HEX[(s_iImuId >> 4) & 0x0F];
        szId[3] = HEX[s_iImuId & 0x0F];
        szId[4] = '\0';
        SGUI_Text_DrawText(pstDeviceIF, szId, &SGUI_FONT_GB2312,
                    57, 53, SGUI_DRAW_NORMAL);
    }
    else
    {
        /* 总线上根本没应答：问题在接线/上拉/地址，不在寄存器 */
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"no IMU", &SGUI_FONT_GB2312,
                    57, 53, SGUI_DRAW_NORMAL);
    }

    /* 剩余堆内存（KB）：核心机后续要跑 CAN + 显示 + 音频，这个数值得一直看着 */
    SGUI_Common_IntegerToStringWithDecimalPoint(
                (SGUI_INT)s_uiFreeHeapKB, 0, szValue, 3, ' ');
    SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312,
                99, 53, SGUI_DRAW_NORMAL);
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"K", &SGUI_FONT_GB2312,
                120, 53, SGUI_DRAW_NORMAL);
    (void)s_uiUptimeSec;   /* 预留：接入 CAN 后显示运行时长与总线负载 */
}

static HMI_ENGINE_RESULT SGUI_DemoDash_Repaint(SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters)
{
    (void)pstParameters;
    SGUI_DemoDash_Paint(pstDeviceIF);
    return HMI_RET_NORMAL;
}

static HMI_ENGINE_RESULT SGUI_DemoDash_PostProcess(SGUI_SCR_DEV* pstDeviceIF,
        HMI_ENGINE_RESULT eProcResult, SGUI_INT iActionID)
{
    (void)pstDeviceIF;
    (void)iActionID;
    SCREEN_RefreshScreen();
    return eProcResult;
}

/*********************************************************** Screen 1 ====*/
/* 陀螺仪专页：MPU6050 三轴角速度 + 芯片温度
 *
 * 布局（128x64，字库 6x12）：
 *   y=0..12   标题栏（反白）："MPU6050 Gyro"
 *   分隔线 y=14
 *   y=16/30/44  X / Y / Z 角速度，数值 8 字符右对齐，单位 d/s 在行尾
 *   y=58        温度
 *
 * 两个"必须能区分"的状态（这是现场排错的关键）：
 *   - 从未收到数据 → 数值位置显示 "----"（而不是 0.00）。0.00 会让人误以为
 *     "陀螺仪正常且静止"，而实际可能压根没读到 —— 两种故障现象完全不同。
 *   - 读取失败     → 标题栏右侧显示 "!"，具体错误码走串口日志
 *     （12px 高的标题栏塞不下 "E-258" 这类内容而不与标题打架）。
 * ------------------------------------------------------------------------ */
#define IMU_VALUE_FIELD     (8)     /* 数值字段宽度（字符），不足前补空格 */

static HMI_ENGINE_RESULT SGUI_DemoImu_Initialize(SGUI_SCR_DEV* pstDeviceIF)
{
    (void)pstDeviceIF;
    /* 值本身由 SGUI_DEMO_SetIMUData() 从外部灌入，这里无需初始化。 */
    return HMI_RET_NORMAL;
}

static HMI_ENGINE_RESULT SGUI_DemoImu_Prepare(SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters)
{
    (void)pstParameters;
    SGUI_Basic_ResetMask(pstDeviceIF);
    return HMI_RET_NORMAL;
}

static HMI_ENGINE_RESULT SGUI_DemoImu_Repaint(SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters)
{
    SGUI_CHAR szValue[16];
    SGUI_INT  iY;
    SGUI_INT  i;

    (void)pstParameters;
    SGUI_Basic_ResetMask(pstDeviceIF);

    /* 标题栏：反白 "MPU6050 Gyro"（全 ASCII） */
    SGUI_Basic_DrawRectangle1(pstDeviceIF, 0, 0, 128, 13,
                SGUI_COLOR_FRGCLR, SGUI_COLOR_FRGCLR);
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"MPU6050 Gyro", &SGUI_FONT_GB2312,
                2, 1, SGUI_DRAW_REVERSE);

    if(0 != s_iImuErr)
    {
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"!", &SGUI_FONT_GB2312,
                    122, 1, SGUI_DRAW_REVERSE);
    }

    SGUI_Basic_DrawLine(pstDeviceIF, 0, 14, 127, 14, SGUI_COLOR_FRGCLR);

    /* 三轴角速度：每行 "<轴><右对齐8字符数值>"，单位 d/s 画在行尾。
     * 宽度核算：轴标签 x=0..5，数值起 x=12（8 字符 → 12..59），
     * 单位 "d/s" 起 x=101（3 字符 → 101..118）。总宽 118 < 128，不重叠。 */
    for(i = 0; i < 3; i++)
    {
        iY = 16 + (SGUI_INT)i * 14;
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)((0 == i) ? "X" : ((1 == i) ? "Y" : "Z")),
                    &SGUI_FONT_GB2312, 0, iY, SGUI_DRAW_NORMAL);

        if(0 == s_bImuValid)
        {
            SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"    ----",
                        &SGUI_FONT_GB2312, 12, iY, SGUI_DRAW_NORMAL);
            continue;
        }
        /* 0.001 °/s 存的是整数毫度：按 2 位小数显示，即 (mdps/10) 作为
         * "两位小数的定点整数"，配合 iDecimalPoint=2。 */
        SGUI_Common_IntegerToStringWithDecimalPoint(
                    (SGUI_INT)(s_aiGyroMdps[i] / 10), 2, szValue,
                    IMU_VALUE_FIELD, ' ');
        SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312,
                    12, iY, SGUI_DRAW_NORMAL);
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"d/s", &SGUI_FONT_GB2312,
                    101, iY, SGUI_DRAW_NORMAL);
    }

    /* 温度行：单位直接用 ASCII "C"（不依赖字库里的 "℃" 全角符号） */
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"T", &SGUI_FONT_GB2312,
                0, 58, SGUI_DRAW_NORMAL);
    if(0 == s_bImuValid)
    {
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"    ----",
                    &SGUI_FONT_GB2312, 12, 58, SGUI_DRAW_NORMAL);
    }
    else
    {
        SGUI_Common_IntegerToStringWithDecimalPoint(
                    (SGUI_INT)(s_iTempMdegC / 10), 2, szValue,
                    IMU_VALUE_FIELD, ' ');
        SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312,
                    12, 58, SGUI_DRAW_NORMAL);
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"C", &SGUI_FONT_GB2312,
                    101, 58, SGUI_DRAW_NORMAL);
    }
    return HMI_RET_NORMAL;
}

static HMI_ENGINE_RESULT SGUI_DemoImu_ProcessEvent(SGUI_SCR_DEV* pstDeviceIF,
        const HMI_EVENT_BASE* pstEvent, SGUI_INT* piActionID)
{
    const SGUI_DEMO_KEY_EVENT* pstKeyEvent = (const SGUI_DEMO_KEY_EVENT*)pstEvent;

    (void)piActionID;
    if(NULL == pstKeyEvent)
    {
        return HMI_RET_INVALID_DATA;
    }
    if(SGUI_DEMO_EVENT_KEY != pstKeyEvent->Head.iID)
    {
        return HMI_RET_NORMAL;
    }
    /* 本页内容由外部数据驱动，按键只触发一次重绘（便于手动核对读数）。 */
    SGUI_DemoImu_Repaint(pstDeviceIF, NULL);
    return HMI_RET_NORMAL;
}

static HMI_ENGINE_RESULT SGUI_DemoImu_PostProcess(SGUI_SCR_DEV* pstDeviceIF,
        HMI_ENGINE_RESULT eProcResult, SGUI_INT iActionID)
{
    (void)pstDeviceIF;
    (void)iActionID;
    SCREEN_RefreshScreen();
    return eProcResult;
}

/*********************************************************** Screen 2 ====*/
/* CAN 通信参数 + 节点一状态
 *
 * 布局（128x64，字库 6x12）：
 *   y=0..12   标题栏（反白）："CAN 500k" + 右侧总线状态（RUN/BUSOFF/...）
 *   y=14/26/38   TX / RX / ERR 三行参数
 *   ── 分隔线 y=50 ──
 *   y=51..63   节点一：在线标记 + 温度 + 状态位
 *
 * 为什么把"状态"放标题栏而不是单独一行：
 *   RUN / BUSOFF 是**一眼就要看到**的信息（Bus-Off 意味着节点已经脱离总线），
 *   塞在第三行会和计数混在一起被忽略。
 * ------------------------------------------------------------------------ */
static HMI_ENGINE_RESULT SGUI_DemoCan_Initialize(SGUI_SCR_DEV* pstDeviceIF)
{
    (void)pstDeviceIF;
    return HMI_RET_NORMAL;
}

static HMI_ENGINE_RESULT SGUI_DemoCan_Prepare(SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters)
{
    (void)pstParameters;
    SGUI_Basic_ResetMask(pstDeviceIF);
    return HMI_RET_NORMAL;
}

/* 把 "CAN 参数 + 节点一" 画一遍。供 Repaint 与周期性刷新共用。 */
static void SGUI_DemoCan_Paint(SGUI_SCR_DEV* pstDeviceIF)
{
    SGUI_CHAR szValue[16];

    SGUI_Basic_ResetMask(pstDeviceIF);

    /* --- 标题栏：速率 + 总线状态 --- */
    SGUI_Basic_DrawRectangle1(pstDeviceIF, 0, 0, 128, 13,
                SGUI_COLOR_FRGCLR, SGUI_COLOR_FRGCLR);
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"CAN 500k", &SGUI_FONT_GB2312,
                2, 1, SGUI_DRAW_REVERSE);
    /* 右侧状态。驱动没装上时显示 "NOCAN"，与"装上了但 Bus-Off"区分开 ——
     * 这两种故障的处置完全不同（前者查接线/引脚，后者查总线/终端电阻）。 */
    SGUI_Text_DrawText(pstDeviceIF,
                (SGUI_CSZSTR)(s_stCan.installed
                              ? can_state_str(s_stCan.state)
                              : "NOCAN"),
                &SGUI_FONT_GB2312, 74, 1, SGUI_DRAW_REVERSE);

    /* --- 参数三行 --- */
    /* TX：成功发送帧数 + 失败次数（失败数才是诊断价值所在） */
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"TX", &SGUI_FONT_GB2312,
                0, 14, SGUI_DRAW_NORMAL);
    SGUI_Common_IntegerToStringWithDecimalPoint(
                (SGUI_INT)s_stCan.tx_frames, 0, szValue, 6, ' ');
    SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312, 14, 14, SGUI_DRAW_NORMAL);
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"F", &SGUI_FONT_GB2312, 50, 14, SGUI_DRAW_NORMAL);
    SGUI_Common_IntegerToStringWithDecimalPoint(
                (SGUI_INT)s_stCan.tx_failed, 0, szValue, 4, ' ');
    SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312, 62, 14, SGUI_DRAW_NORMAL);
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"!", &SGUI_FONT_GB2312, 86, 14, SGUI_DRAW_NORMAL);

    /* RX：成功接收帧数 + 丢弃数 */
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"RX", &SGUI_FONT_GB2312,
                0, 26, SGUI_DRAW_NORMAL);
    SGUI_Common_IntegerToStringWithDecimalPoint(
                (SGUI_INT)s_stCan.rx_frames, 0, szValue, 6, ' ');
    SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312, 14, 26, SGUI_DRAW_NORMAL);
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"F", &SGUI_FONT_GB2312, 50, 26, SGUI_DRAW_NORMAL);
    SGUI_Common_IntegerToStringWithDecimalPoint(
                (SGUI_INT)s_stCan.rx_missed, 0, szValue, 4, ' ');
    SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312, 62, 26, SGUI_DRAW_NORMAL);
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"D", &SGUI_FONT_GB2312, 86, 26, SGUI_DRAW_NORMAL);

    /* ERR：TEC/REC 错误计数器 + 无应答次数。
     * TEC >= 256 就是 Bus-Off（节点已脱离总线），这是最早能看到的征兆；
     * ACK 错误单独显示是因为它指向"对端不在或终端电阻缺失"这一类接线问题，
     * 与"总线受干扰"的处置完全不同。 */
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"TE", &SGUI_FONT_GB2312,
                0, 38, SGUI_DRAW_NORMAL);
    SGUI_Common_IntegerToStringWithDecimalPoint(
                (SGUI_INT)s_stCan.tx_error, 0, szValue, 4, ' ');
    SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312, 14, 38, SGUI_DRAW_NORMAL);
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"RE", &SGUI_FONT_GB2312,
                42, 38, SGUI_DRAW_NORMAL);
    SGUI_Common_IntegerToStringWithDecimalPoint(
                (SGUI_INT)s_stCan.rx_error, 0, szValue, 4, ' ');
    SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312, 56, 38, SGUI_DRAW_NORMAL);
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"AE", &SGUI_FONT_GB2312,
                84, 38, SGUI_DRAW_NORMAL);
    SGUI_Common_IntegerToStringWithDecimalPoint(
                (SGUI_INT)s_stCan.ack_errors, 0, szValue, 4, ' ');
    SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312, 98, 38, SGUI_DRAW_NORMAL);

    SGUI_Basic_DrawLine(pstDeviceIF, 0, 50, 127, 50, SGUI_COLOR_FRGCLR);

    /* --- 节点一一行：在线 + 温度 + 状态 + 累计帧数 --- */
    SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"N1", &SGUI_FONT_GB2312,
                0, 51, SGUI_DRAW_NORMAL);
    if(0 == s_iN1Online)
    {
        /* 离线时把"收到过多少帧"一起显示：
         *   0 帧  → 从来没通过（接线/终端电阻/位速率）
         *   >0 帧 → 曾经通过，现在掉了（节点复位或总线故障）
         * 这两种情况的排查方向完全不同，界面必须能区分。 */
        if(0u == s_uiN1Frames)
        {
            SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"no link", &SGUI_FONT_GB2312,
                        16, 51, SGUI_DRAW_NORMAL);
        }
        else
        {
            SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"lost", &SGUI_FONT_GB2312,
                        16, 51, SGUI_DRAW_NORMAL);
            SGUI_Common_IntegerToStringWithDecimalPoint(
                        (SGUI_INT)s_uiN1Frames, 0, szValue, 5, ' ');
            SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312,
                        46, 51, SGUI_DRAW_NORMAL);
        }
    }
    else
    {
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)">", &SGUI_FONT_GB2312,
                    16, 51, SGUI_DRAW_NORMAL);
        /* 温度：0.001 ℃ 定点 → 一位小数显示 */
        SGUI_Common_IntegerToStringWithDecimalPoint(
                    (SGUI_INT)(s_iN1TempMdegC / 100), 1, szValue, 5, ' ');
        SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312,
                    24, 51, SGUI_DRAW_NORMAL);
        SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"C", &SGUI_FONT_GB2312,
                    55, 51, SGUI_DRAW_NORMAL);
        /* 状态位：bit1=过温 bit2=传感器故障（见 proto_telemetry_node1_t.status）*/
        if(0 != (s_iN1Status & 0x02))
        {
            SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"OVERHEAT", &SGUI_FONT_GB2312,
                        64, 51, SGUI_DRAW_NORMAL);
        }
        else if(0 != (s_iN1Status & 0x04))
        {
            SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"SENFAIL", &SGUI_FONT_GB2312,
                        64, 51, SGUI_DRAW_NORMAL);
        }
        else
        {
            SGUI_Text_DrawText(pstDeviceIF, (SGUI_CSZSTR)"ok", &SGUI_FONT_GB2312,
                        64, 51, SGUI_DRAW_NORMAL);
        }
        /* 累计帧数放行尾，用于确认"是在持续通信"而不是一次性的残留值 */
        SGUI_Common_IntegerToStringWithDecimalPoint(
                    (SGUI_INT)s_uiN1Frames, 0, szValue, 5, ' ');
        SGUI_Text_DrawText(pstDeviceIF, szValue, &SGUI_FONT_GB2312,
                    96, 51, SGUI_DRAW_NORMAL);
    }
    (void)szValue;
}

static HMI_ENGINE_RESULT SGUI_DemoCan_Repaint(SGUI_SCR_DEV* pstDeviceIF, const void* pstParameters)
{
    (void)pstParameters;
    SGUI_DemoCan_Paint(pstDeviceIF);
    return HMI_RET_NORMAL;
}

static HMI_ENGINE_RESULT SGUI_DemoCan_ProcessEvent(SGUI_SCR_DEV* pstDeviceIF,
        const HMI_EVENT_BASE* pstEvent, SGUI_INT* piActionID)
{
    const SGUI_DEMO_KEY_EVENT* pstKeyEvent = (const SGUI_DEMO_KEY_EVENT*)pstEvent;

    (void)piActionID;
    if(NULL == pstKeyEvent)
    {
        return HMI_RET_INVALID_DATA;
    }
    if(SGUI_DEMO_EVENT_KEY != pstKeyEvent->Head.iID)
    {
        return HMI_RET_NORMAL;
    }
    SGUI_DemoCan_Repaint(pstDeviceIF, NULL);
    return HMI_RET_NORMAL;
}

static HMI_ENGINE_RESULT SGUI_DemoCan_PostProcess(SGUI_SCR_DEV* pstDeviceIF,
        HMI_ENGINE_RESULT eProcResult, SGUI_INT iActionID)
{
    (void)pstDeviceIF;
    (void)iActionID;
    SCREEN_RefreshScreen();
    return eProcResult;
}

/************************************************************ Public ====*/
/* 供主循环灌入 CAN 统计（见 sgui_demo.h 的说明） */
void SGUI_DEMO_SetCanStats(const can_stats_t *stats)
{
    if(NULL == stats)
    {
        /* 驱动未就绪：保留 installed=false，让界面显示 "NOCAN" 而不是陈旧计数 */
        s_stCan.installed = false;
        return;
    }
    s_stCan = *stats;
}

void SGUI_DEMO_SetNode1Status(SGUI_INT iOnline, int32_t iTempMdegC,
                              SGUI_INT iStatus, uint32_t uiFrames)
{
    s_iN1Online    = iOnline;
    s_iN1TempMdegC = iTempMdegC;
    s_iN1Status    = iStatus;
    s_uiN1Frames   = uiFrames;
}

/* 供主循环灌入采样值（见 sgui_demo.h 的说明） */
void SGUI_DEMO_SetIMUData(const int32_t gyro_mdps[3], int32_t temp_mdeg_c)
{
    if(NULL != gyro_mdps)
    {
        s_aiGyroMdps[0] = gyro_mdps[0];
        s_aiGyroMdps[1] = gyro_mdps[1];
        s_aiGyroMdps[2] = gyro_mdps[2];
        s_bImuValid     = SGUI_TRUE;
    }
    s_iTempMdegC = temp_mdeg_c;
}

void SGUI_DEMO_SetIMUError(int err_code)
{
    s_iImuErr   = err_code;
    s_bImuValid = SGUI_FALSE;   /* 读失败就不能再显示旧值当"当前值" */
}

/* 主循环每次采到新数据后调用；非当前页直接返回，不做任何 I2C 输出 */
void SGUI_DEMO_RefreshIMU(void)
{
    SGUI_INT iScreen;

    if(NULL == s_stEngine.CurrentScreenObject)
    {
        return;
    }
    iScreen = s_stEngine.CurrentScreenObject->iScreenID;
    /* 陀螺仪数据在**两屏**上都有显示（陀螺仪页 + 仪表盘）。
     * 只认一屏的话，切到另一屏后读数就不会再更新了（很容易漏的一点）。 */
    if((SGUI_DEMO_SCR_IMU == iScreen) || (SGUI_DEMO_SCR_DASHBOARD == iScreen))
    {
        s_stEngine.CurrentScreenObject->pstActions->Repaint(
                    &g_stDeviceInterface, NULL);
        SCREEN_RefreshScreen();
    }
}

/* CAN 屏刷新（带节流，见 s_iCanTicks 的说明） */
void SGUI_DEMO_RefreshCan(void)
{
    if(NULL == s_stEngine.CurrentScreenObject)
    {
        return;
    }
    if(SGUI_DEMO_SCR_CAN != s_stEngine.CurrentScreenObject->iScreenID)
    {
        return;
    }
    if(s_iCanTicks < SGUI_DEMO_CAN_REFRESH_TICKS)
    {
        s_iCanTicks++;
        return;
    }
    s_iCanTicks = 0;
    SGUI_DemoCan_Paint(&g_stDeviceInterface);
    SCREEN_RefreshScreen();
}

/* 供主循环灌入平台状态（见 sgui_demo.h 的说明） */
void SGUI_DEMO_SetBusStatus(SGUI_INT iOledOk, SGUI_INT iImuOk)
{
    s_iOledOk = iOledOk;
    s_iImuOk  = iImuOk;
}

void SGUI_DEMO_SetSystemInfo(uint32_t uiUptimeSec, uint32_t uiFreeHeapKB)
{
    s_uiUptimeSec  = uiUptimeSec;
    s_uiFreeHeapKB = uiFreeHeapKB;
}

/* 上报实测的 WHO_AM_I：初始化失败时界面能显示 "IDxx"，
 * 从而区分"芯片在但读不出数据"与"总线根本没应答"。 */
void SGUI_DEMO_SetImuId(SGUI_INT iWhoAmI)
{
    s_iImuId = iWhoAmI;
}

/*****************************************************************************
** Function Name: SGUI_DEMO_Initialize
** Purpose:       Bring up the screen device, the two screens and the engine.
** Return:        HMI_ENGINE_RESULT
*****************************************************************************/
HMI_ENGINE_RESULT SGUI_DEMO_Initialize(void)
{
    HMI_ENGINE_RESULT   eResult;
    SGUI_INT            iIndex;

    /* 1. Bring up the panel and the 1KB frame buffer. */
    SCREEN_Initialize();

    /* 2. Bind the SimpleGUI device object to our screen callbacks. */
    SGUI_SystemIF_MemorySet(&g_stDeviceInterface, 0x00, sizeof(SGUI_SCR_DEV));
    g_stDeviceInterface.stSize.iWidth          = 128;
    g_stDeviceInterface.stSize.iHeight         = 64;
    g_stDeviceInterface.fnSetPixel             = SCREEN_SetPixel;
    g_stDeviceInterface.fnFillRect             = SCREEN_FillRectangle;
    g_stDeviceInterface.fnClear                = SCREEN_ClearDisplay;
    g_stDeviceInterface.fnSyncBuffer           = SCREEN_RefreshScreen;
    /* [ESP32 移植] 回读像素。本工程在顶层编译选项里开了
     * SGUI_GET_POINT_FUNC_EN，因此 SGUI_SCR_DEV 多出 fnGetPixel 成员。
     * 开启该宏后**必须**赋非空实现：库在 SGUI_Basic.c 里对 NULL 是
     * "静默返回背景色"而不是报错，很容易让人以为是显存写错了。
     * SSD1306 没有硬件回读，实现是从显存（权威副本）读，见 screen.c。 */
#ifdef SGUI_GET_POINT_FUNC_EN
    g_stDeviceInterface.fnGetPixel             = SCREEN_GetPixel;
#endif
    /* Glyph scratch buffer used by the text drawing functions. */
    g_stDeviceInterface.stBuffer.pBuffer       = s_aucCharBitmap;
    g_stDeviceInterface.stBuffer.sSize         = SGUI_DEMO_CHAR_BUF_SIZE;
    SGUI_Basic_ResetMask(&g_stDeviceInterface);

    /* 3. Prepare the HMI engine. */
    SGUI_SystemIF_MemorySet(&s_stEngine, 0x00, sizeof(HMI_ENGINE_OBJECT));
    s_stEngine.ScreenCount      = sizeof(s_arrpstScreens) / sizeof(*s_arrpstScreens);
    s_stEngine.ScreenObjPtr     = s_arrpstScreens;
    s_stEngine.Interface        = &g_stDeviceInterface;

    /* 4. Initialize every screen object. */
    for(iIndex = 0; iIndex < s_stEngine.ScreenCount; iIndex++)
    {
        if((NULL != s_arrpstScreens[iIndex]) &&
           (NULL != s_arrpstScreens[iIndex]->pstActions) &&
           (NULL != s_arrpstScreens[iIndex]->pstActions->Initialize))
        {
            s_arrpstScreens[iIndex]->pstActions->Initialize(&g_stDeviceInterface);
            s_arrpstScreens[iIndex]->pstPrevious = NULL;
        }
    }

    /* 5. Activate the engine on the **dashboard** (boot screen) and paint it. */
    eResult = HMI_ActiveEngine(&s_stEngine, SGUI_DEMO_SCR_DASHBOARD);
    if(HMI_PROCESS_FAILED(eResult))
    {
        return eResult;
    }
    eResult = HMI_StartEngine(NULL);
    if(HMI_PROCESS_FAILED(eResult))
    {
        return eResult;
    }
    /* Paint the first screen explicitly, StartEngine only calls Prepare. */
    SGUI_DemoDash_Repaint(&g_stDeviceInterface, NULL);
    SCREEN_RefreshScreen();
    return HMI_RET_NORMAL;
}

/*****************************************************************************
** Function Name: SGUI_DEMO_Process
** Purpose:       Periodic work. Only does "self-advancing" housekeeping now
**                that the demo screens are gone: it implements the idle
**                auto-return to the default screen.
** Return:        None
*****************************************************************************/
void SGUI_DEMO_Process(void)
{
    /* --- 空闲自动回默认屏 ------------------------------------------------
     * 陀螺仪数据是这台设备的主信息，不该因为误碰按键永远停在别的屏上。
     * 30 秒无输入就回仪表盘（SGUI_DEMO_ProcessKey() 里会把计时清零）。 */
    if(s_iIdleTicks < SGUI_DEMO_IDLE_RETURN_TICKS)
    {
        s_iIdleTicks++;
        if(s_iIdleTicks >= SGUI_DEMO_IDLE_RETURN_TICKS)
        {
            SGUI_DEMO_ReturnToDefault();
        }
    }

    /* CAN 屏按自己的节奏刷新（内部判断是否可见 + 节流） */
    SGUI_DEMO_RefreshCan();
}

/*****************************************************************************
** Function Name: SGUI_DEMO_ReturnToDefault
** Purpose:       回到默认屏（仪表盘）。
** Note:          仪表盘在上电时由 SGUI_DEMO_Initialize() 直接激活
**                （HMI_ActiveEngine(..., SGUI_DEMO_SCR_DASHBOARD)），
**                因此不需要额外的"显示仪表盘"入口 —— 只保留这个
**                "无人操作后自动回落"的入口。
** Return:        None
*****************************************************************************/
void SGUI_DEMO_ReturnToDefault(void)
{
    if(SGUI_DEMO_CurrentScreenID() != SGUI_DEMO_SCR_DASHBOARD)
    {
        (void)SGUI_DEMO_GoToScreen(SGUI_DEMO_SCR_DASHBOARD);
    }
}

/*****************************************************************************
** Function Name: SGUI_DEMO_CurrentScreenID
** Purpose:       返回当前活动屏幕 ID（无活动屏时返回 -1）。
**                主循环用它检测"屏幕是否被换掉了"，从而决定要不要整屏重绘。
** Return:        SGUI_INT
*****************************************************************************/
SGUI_INT SGUI_DEMO_CurrentScreenID(void)
{
    if(NULL == s_stEngine.CurrentScreenObject)
    {
        return -1;
    }
    return s_stEngine.CurrentScreenObject->iScreenID;
}

/*****************************************************************************
** Function Name: SGUI_DEMO_GoToScreen
** Purpose:       切换到指定屏幕并整屏重绘（清显存，避免残留上一屏内容）。
**                走 HMI 引擎的 HMI_GoToScreen()，而不是自己改 CurrentScreenObject
**                —— 引擎会在切换时调用目标屏的 Prepare，跳过它会让目标屏状态没初始化。
** Return:        HMI 引擎结果
*****************************************************************************/
HMI_ENGINE_RESULT SGUI_DEMO_GoToScreen(SGUI_INT iScreenID)
{
    HMI_ENGINE_RESULT eResult;

    eResult = HMI_GoToScreen(iScreenID, NULL);
    if(HMI_PROCESS_FAILED(eResult))
    {
        return eResult;
    }

    /* 整屏重绘：先清显存（含脏区记录），再调目标屏的 Repaint。
     * 只清不清脏区记录的话，上一次的脏区会把旧内容又推回屏幕。 */
    SCREEN_ClearCache();
    if(NULL != s_stEngine.CurrentScreenObject)
    {
        if(NULL != s_stEngine.CurrentScreenObject->pstActions->Repaint)
        {
            s_stEngine.CurrentScreenObject->pstActions->Repaint(
                        &g_stDeviceInterface, NULL);
        }
    }
    SCREEN_RefreshScreen();
    return HMI_RET_NORMAL;
}

/*****************************************************************************
** Function Name: SGUI_DEMO_ProcessKey
** Purpose:       把一个按键码交给当前屏幕处理，并负责屏幕之间的切换。
**                切屏放在这里而不是各屏的 ProcessEvent 里：引擎的
**                HMI_ProcessEvent() 不会替你换屏（它只调 ProcessEvent +
**                PostProcess），跨屏必须由调用方显式调 HMI_GoToScreen()。
** Params:        iKeyCode - SGUI_DEMO_KEY_xxx
** Return:        None
*****************************************************************************/
void SGUI_DEMO_ProcessKey(SGUI_INT iKeyCode)
{
    SGUI_INT iCurrent = SGUI_DEMO_CurrentScreenID();

    if(SGUI_DEMO_KEY_NONE == iKeyCode)
    {
        return;
    }

    /* 有输入就重置"空闲回落"计时（见 sgui_demo.h 的说明） */
    s_iIdleTicks = 0;

    /* --- 屏幕切换：用 HMI_GoToScreen() 真正换屏（含目标屏 Prepare） ---
     * 只有两屏，因此 UP/DOWN 都是"在仪表盘与陀螺仪页之间往返"（循环切换，
     * 不会出现"到头卡住"）。 */
    if(SGUI_DEMO_KEY_UP == iKeyCode)
    {
        (void)SGUI_DEMO_GoToScreen((iCurrent <= 0)
                    ? ((SGUI_INT)s_stEngine.ScreenCount - 1)
                    : (iCurrent - 1));
        return;
    }
    else if(SGUI_DEMO_KEY_DOWN == iKeyCode)
    {
        (void)SGUI_DEMO_GoToScreen((iCurrent >= ((SGUI_INT)s_stEngine.ScreenCount - 1))
                    ? 0
                    : (iCurrent + 1));
        return;
    }
    else if(SGUI_DEMO_KEY_ESC == iKeyCode)
    {
        /* 返回默认屏（仪表盘） */
        SGUI_DEMO_ReturnToDefault();
        return;
    }

    /* --- 其余按键（ENTER 等）交给当前屏幕自己处理 --- */
    SGUI_DEMO_PostKeyEvent(iKeyCode);
}

/*****************************************************************************
** Function Name: SGUI_DEMO_PostKeyEvent
** Purpose:       Post one key event into the engine.
** Params:       iKeyCode - SGUI_DEMO_KEY_xxx
** Return:        None
*****************************************************************************/
void SGUI_DEMO_PostKeyEvent(SGUI_INT iKeyCode)
{
    SGUI_DEMO_KEY_EVENT   stEvent;

    if(SGUI_DEMO_KEY_NONE == iKeyCode)
    {
        return;
    }
    HMI_EVENT_INIT(stEvent);
    stEvent.Head.iID        = SGUI_DEMO_EVENT_KEY;
    stEvent.iKeyValue       = iKeyCode;
    HMI_ProcessEvent((HMI_EVENT_BASE*)&stEvent);
}
