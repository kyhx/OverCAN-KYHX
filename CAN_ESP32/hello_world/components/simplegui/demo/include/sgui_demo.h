/*****************************************************************************
** File: sgui_demo.h
** Description: 核心机（ESP32-S3）的 SimpleGUI 界面层接口。
**
** 三屏（移植验证用的"基础绘图 / 组件演示"两屏已清理）：
**   SGUI_DEMO_SCR_DASHBOARD (0) —— 上电默认屏：陀螺仪 + 总线健康度 + 运行信息
**   SGUI_DEMO_SCR_IMU       (1) —— 陀螺仪专页：三轴角速度 + 温度
**   SGUI_DEMO_SCR_CAN       (2) —— CAN 通信参数 + 节点一状态
**
** ⚠️ 这些值同时也是 HMI 引擎里 s_arrpstScreens[] 的**数组下标**
**    （HMI_GoToScreen(i) 按下标索引）。改这里必须同步改那张表。
*****************************************************************************/
#ifndef __SGUI_DEMO_H__
#define __SGUI_DEMO_H__

#include <stdint.h>

#include "SGUI_Typedef.h"
#include "HMI_Engine.h"
#include "can_twai.h"   /* can_stats_t：界面要显示 CAN 通信参数 */

/* Screen IDs of this screen set (must match s_arrpstScreens[] indices) */
#define SGUI_DEMO_SCR_DASHBOARD     (0)    /* Dashboard (boot screen)     */
#define SGUI_DEMO_SCR_IMU           (1)    /* MPU6050 gyro readout        */
#define SGUI_DEMO_SCR_CAN           (2)    /* CAN bus + node1 status      */

/* Key codes, platform independent. Wired to buttons in app_main.c. */
#define SGUI_DEMO_KEY_NONE          (0)
#define SGUI_DEMO_KEY_UP            (1)
#define SGUI_DEMO_KEY_DOWN          (2)
#define SGUI_DEMO_KEY_ENTER         (3)
#define SGUI_DEMO_KEY_ESC           (4)

/* Event IDs posted to the HMI engine. */
#define SGUI_DEMO_EVENT_KEY         (0x1000)

/* Event payload: one key code. */
typedef struct
{
    HMI_EVENT_BASE  Head;
    SGUI_INT        iKeyValue;
}SGUI_DEMO_KEY_EVENT;

/* Initialize the device interface, the demo screens and the HMI engine. */
HMI_ENGINE_RESULT SGUI_DEMO_Initialize(void);

/* Periodic process, called from the main loop. */
void SGUI_DEMO_Process(void);

/* Post one key event into the engine (SGUI_DEMO_KEY_xxx). */
void SGUI_DEMO_PostKeyEvent(SGUI_INT iKeyCode);

/**
 * @brief 刷新"陀螺仪"页（仅当该页当前可见时才重绘）。
 *
 * 与 SGUI_DEMO_Process() 的分工：
 *   - SGUI_DEMO_Process() 由主循环按显示节奏调用，负责**自行推进**的动画
 *     （演示用的进度条）；
 *   - 本函数用于**外部数据驱动**的页面：主循环采到新的 MPU6050 数据后调用它，
 *     由 demo 层决定"要不要重绘、重绘哪一块"。
 *
 * 之所以把判断放在 demo 层：只有它知道当前是第几屏、以及哪些区域需要重绘。
 * 主循环不该知道界面细节。
 *
 * @note 非当前页时立即返回（不做任何 I2C 输出）。
 */
void SGUI_DEMO_RefreshIMU(void);

/**
 * @brief 把一份 MPU6050 采样交给陀螺仪页显示。
 * @param gyro_mdps   角速度，单位 0.001 °/s，下标 0/1/2 = X/Y/Z
 * @param temp_mdeg_c 温度，单位 0.001 ℃
 */
void SGUI_DEMO_SetIMUData(const int32_t gyro_mdps[3], int32_t temp_mdeg_c);

/**
 * @brief 报告一次 IMU 读取失败。
 * @param err_code 错误码（本项目用 ESP-IDF esp_err_t 的整数值；0 表示无错）
 *
 * 语义上会**清掉"数据有效"标志**：读到失败就必须停止显示旧值，
 * 否则界面会拿上一次的成功读数冒充当前值 —— 这是最危险的一类显示错误。
 */
void SGUI_DEMO_SetIMUError(int err_code);

/* --- 按键 / 编码器接入（见 port/src/input.c 的映射说明）-------------------
 * 输入映射（只有两屏）：
 *   编码器旋转        → 在"仪表盘 / 陀螺仪页"之间切换
 *   BTN1 (UP) / BTN2 (DOWN) → 同上（切换屏幕）
 *   BTN3 长按 (ESC)   → 回到默认屏（仪表盘）
 *   30 秒无输入        → 自动回默认屏
 * ------------------------------------------------------------------------ */

/** @return 当前活动屏 ID；无活动屏返回 -1 */
SGUI_INT SGUI_DEMO_CurrentScreenID(void);

/**
 * @brief 切换到指定屏并整屏重绘（清显存 + 调目标屏 Repaint）。
 *
 * 内部走 HMI 引擎的 HMI_GoToScreen()（会调用目标屏 Prepare），
 * 因此不要自己改 engine.CurrentScreenObject —— 那样会跳过 Prepare。
 */
HMI_ENGINE_RESULT SGUI_DEMO_GoToScreen(SGUI_INT iScreenID);

/**
 * @brief 处理一个按键码：先做屏幕切换，其余交给当前屏。
 * @param iKeyCode SGUI_DEMO_KEY_UP / DOWN / ENTER / ESC
 */
void SGUI_DEMO_ProcessKey(SGUI_INT iKeyCode);

/* --- 全局状态（给仪表盘显示用）------------------------------------------
 * 为什么用显式 setter 而不是让界面层去调 esp_* API：
 *   demo 层要保持"零平台依赖"，否则它就不能在 PC 上单测 —— 这与
 *   firmware/node1 的分层原则一致。主循环负责把平台数值喂进来。 */

/** @brief 上报 I2C 总线状态（OLED 是否应答、MPU6050 是否在位）。 */
void SGUI_DEMO_SetBusStatus(SGUI_INT iOledOk, SGUI_INT iImuOk);

/** @brief 上报运行统计（供仪表盘显示）。 */
void SGUI_DEMO_SetSystemInfo(uint32_t uiUptimeSec, uint32_t uiFreeHeapKB);

/**
 * @brief 上报实测的 WHO_AM_I(0x75)。
 *
 * 初始化失败时界面会把它显示成 "IDxx"，用来区分两种完全不同的故障：
 *   - 显示 "IDxx"    → 芯片在总线上、能应答，问题在寄存器/兼容性
 *   - 显示 "no IMU"  → 总线上根本没应答，问题在接线/上拉/地址
 * 没有这个区分，两种故障在屏幕上长得一模一样。
 */
void SGUI_DEMO_SetImuId(SGUI_INT iWhoAmI);

/* --- CAN / 节点一状态（给 CAN 屏显示用）---------------------------------
 * 与陀螺仪同样的分层原则：主循环拿到 can_stats_t 与解析后的节点一遥测后
 * 灌进来，demo 层不碰 TWAI、也不碰协议解码。 */

/**
 * @brief 上报 CAN 端口层统计（波特率外的全部"通信参数"）。
 * @param stats 由 can_get_stats() 得到的快照；NULL 表示驱动未就绪
 */
void SGUI_DEMO_SetCanStats(const can_stats_t *stats);

/**
 * @brief 上报节点一状态（从遥测/心跳解析得到）。
 * @param online      1 = 在线（在离线超时内收到过帧）
 * @param temperature 温度，单位 0.001 ℃（与陀螺仪同一套定点约定）
 * @param status      遥测 status 位域（bit1=过温 bit2=传感器故障）
 * @param frames      累计收到的节点一帧数（0 且 offline 时显示 "no link"）
 */
void SGUI_DEMO_SetNode1Status(SGUI_INT iOnline, int32_t iTempMdegC,
                              SGUI_INT iStatus, uint32_t uiFrames);

/**
 * @brief 刷新 CAN 屏（仅当该屏可见时重绘）。
 * 主循环按自己的节奏调用；内部做了节流，不必担心调用过密。
 */
void SGUI_DEMO_RefreshCan(void);

/**
 * @brief 回到默认屏（仪表盘）。
 *
 * 仪表盘在上电时由 SGUI_DEMO_Initialize() 直接激活，所以没有单独的"去仪表盘"
 * 入口；主循环在一段时间没有输入时调用本函数把它切回来：
 * 陀螺仪数据是这台设备的主信息，不该因为误碰按键被留在演示页上。
 */
void SGUI_DEMO_ReturnToDefault(void);

#endif /* __SGUI_DEMO_H__ */
