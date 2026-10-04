/*
 * ESP32-S3 核心机 —— 启动入口
 *
 * 本文件负责：打印芯片信息（排错用）+ 拉起 SimpleGUI 显示 + MPU6050 陀螺仪采样
 *             + 主循环把采样值送到"陀螺仪"页。
 *
 * 显示栈（自下而上，全部在 components/simplegui/ 内）：
 *   i2c_master（ESP-IDF v6.x 新版驱动）
 *     ├─ port/src/ssd1306.c   SSD1306 控制器驱动（含从机探测与朝向配置）
 *     └─ port/src/mpu6050.c   MPU6050 六轴驱动（**复用同一条 I2C 总线**）
 *       → port/src/screen.c   1KB 显存 + 脏区刷新，实现 SGUI_SCR_DEV 回调
 *         → GUI/ + HMI/       SimpleGUI 核心库与交互引擎（上游原样）
 *           → demo/           演示界面（含陀螺仪页，后续替换为核心机状态页）
 *
 * 接线（docs/引脚分配.md §2.1 核心机 I²C0 —— OLED 与 MPU6050 共用一条总线）：
 *   SDA → GPIO1
 *   SCL → GPIO2
 *   SSD1306 地址 0x3C（部分模块 0x3D）
 *   MPU6050 地址 0x68（AD0 接地）
 *   ⚠️ 两个模块一般各自带 4.7k 上拉，并联后约 2.35k，仍在正常范围；
 *      若线较长导致通信不稳，可去掉其中一个模块的上拉。
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>     /* memset（回环自测构造测试帧时用） */

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"     /* esp_get_free_heap_size / esp_get_minimum_free_heap_size */
#include "esp_timer.h"      /* esp_timer_get_time（运行时长） */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* SimpleGUI 显示组件（components/simplegui/） */
#include "sgui_demo.h"
#include "ssd1306.h"
#include "mpu6050.h"
#include "input.h"
#include "can_twai.h"

/* 共用协议库（与节点一、PC 单测同一份源码，经组件的 INCLUDE_DIRS 暴露） */
#include "proto_id.h"
#include "proto_codec.h"

static const char *TAG = "core";

/* OLED 主循环节奏。20 ms（50 Hz）足够流畅，同时把 I2C 占用压到很低，
 * 给后续 CAN 收发与音频任务留出 CPU 与总线。
 * 输入也按这个节奏轮询：按键消抖窗口与长按判定都按"采样周期数"计
 * （见 input.h 的 INPUT_LONG_PRESS_TICKS），所以改这里会影响手感。 */
#define DISPLAY_TICK_MS   (20)

/* 陀螺仪采样间隔。MPU6050 配置为 100 Hz 采样率（见 mpu6050_init），
 * 因此 20 ms 取一次正好匹配，不会重复读同一份数据。 */
#define IMU_TICK_MS       (20)

/* 输入轮询周期。与显示同拍即可：按键是人手动作，20 ms 足够灵敏。 */
#define INPUT_TICK_MS     (20)

/* 运行信息（运行时长 / 剩余堆）刷新周期。1 s 足够，且不必每拍都算。 */
#define INFO_TICK_MS      (1000)

/* CAN 诊断日志周期。2 s 足够看清趋势，又不会把启动日志挤掉。
 * ⚠️ 最初设 5 s，结果**首条诊断都没机会打印**：实机发现主循环存活不到 5 s。
 * 缩短周期是为了让"循环有没有在跑"这件事尽快可见。 */
#define CAN_DIAG_TICK_MS  (2000)

/* 主站心跳周期。协议规定节点侧 3 s 收不到心跳即判主站掉线（安全态），
 * 因此必须**明显小于** 3 s：这里用 1 s，允许连续丢 2 帧仍不误判。 */
#define HEARTBEAT_TICK_MS (1000)

/* 节点离线判定：协议规定的 3 s（与节点一侧的 NODE1_MASTER_OFFLINE_MS 对应）。
 * 主站这边用同样的窗口判断节点是否还在线，两侧对称。 */
#define NODE_OFFLINE_MS   (3000)

static void log_chip_info(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "chip=%s cores=%d wifi=%d ble=%d",
             CONFIG_IDF_TARGET, chip_info.cores,
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? 1 : 0,
             (chip_info.features & CHIP_FEATURE_BLE) ? 1 : 0);

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        /* ⚠️ 这里打印的是**运行时探测到的真实 flash 容量**，不是编译期的
         * CONFIG_ESPTOOLPY_FLASHSIZE。
         * 教训（2026-10-04）：最初我写的就是 `CONFIG_ESPTOOLPY_FLASHSIZE`，
         * 而 sdkconfig 里被设成了 2MB，于是日志一直打 "flash=2MB"，
         * 让我误判了板子容量——**实际是 16MB**（用 esptool flash-id 查实）。
         * 一个把编译期配置伪装成硬件事实的日志，比没有日志更糟。
         * 同时打印编译期值，两者不一致就是配置错配的直接证据。 */
        ESP_LOGI(TAG, "flash=%" PRIu32 "MB (detected) / build config=%s",
                 flash_size / (1024u * 1024u), CONFIG_ESPTOOLPY_FLASHSIZE);
        ESP_LOGI(TAG, "free heap(min)=%" PRIu32 "B", esp_get_minimum_free_heap_size());
    } else {
        ESP_LOGW(TAG, "无法读取 flash 容量（esp_flash_get_size 失败）");
    }
}

/* 拉起 I2C 总线上的两片从机（OLED + 陀螺仪）。
 *
 * 顺序是**硬性**的：SSD1306_I2C_Init() 负责 i2c_new_master_bus()，
 * 陀螺仪必须复用这条总线 —— 各自去 new_master_bus 会因 I2C_NUM_0 已被占用而失败。
 *
 * 返回值用两个 out 参数把"谁通了"告诉调用方，界面层据此显示 I2C 健康度：
 * 现场排错时，这一行能立刻分清"总线没通"还是"界面画错了"。 */
static void sensors_init(int *out_oled_ok, int *out_imu_ok)
{
    int oled_ok = 0;
    int imu_ok  = 0;

    if (SSD1306_I2C_Init() != 0) {
        ESP_LOGW(TAG, "OLED I2C init failed (err=%d) — 检查 SDA=GPIO1/SCL=GPIO2 与上拉",
                 SSD1306_LastError());
    } else if (!SSD1306_Present()) {
        ESP_LOGW(TAG, "OLED 无应答 — 接线/上拉/地址(0x3C vs 0x3D)");
    } else {
        oled_ok = 1;
    }

    /* 陀螺仪挂在同一条总线上 */
    if (mpu6050_bus_add(SSD1306_GetBus()) != 0) {
        ESP_LOGE(TAG, "MPU6050 挂载失败 (err=%d)", mpu6050_last_error());
    } else if (mpu6050_init(MPU6050_GYRO_FS_500, MPU6050_ACCEL_FS_2G) != 0) {
        /* ±500 °/s / ±2g：手持与小车场景够用；量程越大分辨率越差，
         * 且陀螺仪灵敏度直接决定零偏与噪声的可观测性。 */
        ESP_LOGE(TAG, "MPU6050 初始化失败 (err=%d) — 检查 0x68 地址与 WHO_AM_I",
                 mpu6050_last_error());
    } else {
        imu_ok = 1;
    }

    if (out_oled_ok != NULL) { *out_oled_ok = oled_ok; }
    if (out_imu_ok  != NULL) { *out_imu_ok  = imu_ok;  }
}

/* ==========================================================================
 * CAN：主站侧的最小协议实现
 *
 * 分工（与 node1 对称）：
 *   本文件 = 主站业务：发心跳、收节点帧、维护在线位图、把结果喂给界面。
 *   port/src/can_twai.c = 物理收发与统计，不认识协议。
 *   firmware/common/protocol = 帧格式（与节点一、PC 单测同一份源码）。
 * ========================================================================== */

/* 节点一状态跟踪。全为 static：app_main 是唯一使用者，且不占栈。 */
static uint32_t s_n1_frames    = 0;   /* 累计收到的节点一帧数（遥测+ACK） */
static uint32_t s_n1_last_ms   = 0;   /* 最近一次收到的时刻（用于离线判定） */
static int32_t  s_n1_temp_mdeg = 0;   /* 遥测温度，0.001 ℃ */
static uint8_t  s_n1_status    = 0;   /* 遥测 status 位域 */
static uint8_t  s_hb_count     = 0;   /* 心跳计数（回绕） */

/* 陀螺仪错误日志限流：见采样处的注释。
 * 不这么做的话，一个持续故障会以 20 Hz 把日志刷满。 */
static bool     s_imu_err_logged = false;
static int      s_imu_err_last   = 0;

static void can_bringup(void)
{
    if (can_init() != 0) {
        ESP_LOGE(TAG, "CAN 初始化失败 (err=%d) — 检查 TX=GPIO%d / RX=GPIO%d 与收发器",
                 can_last_error(), CAN_TWAI_TX_GPIO, CAN_TWAI_RX_GPIO);
        SGUI_DEMO_SetCanStats(NULL);
        return;
    }
    /* 首帧统计先灌一次：否则 CAN 屏在第一次刷新前会显示全 0，
     * 看起来像"NOCAN"，与真实状态不符。 */
    {
        can_stats_t st;
        can_get_stats(&st);
        SGUI_DEMO_SetCanStats(&st);
    }
}

/* 发一帧主站心跳。
 *
 * online_bitmap 的 bit N 表示"节点 N 在线"——这是主站把**自己观察到的**
 * 拓扑通过总线广播回去，节点侧可据此判断"主站是否还看得见我"。
 * 本步只运维节点一，故只置 bit1。 */
static void can_send_heartbeat(uint32_t now_ms)
{
    proto_heartbeat_msg_t hb;
    proto_can_frame_t     f;
    int                   online = 0;

    if (!can_ready()) {
        return;
    }

    /* ⚠️ 队列里还有帧没发出去就不要再投新心跳。
     *
     * 实机踩坑（2026-10-04，COM4 实测，总线无 ACK）：
     *   最初每秒都投一帧 → 帧滞留在驱动队列里 → 队列填满 →
     *   驱动每秒打印 `E esp_twai: _node_queue_tx(617): tx queue full`，
     *   日志被刷屏，其它有用信息全被淹没。
     *
     * ⚠️ 判断依据必须是**队列余量**（can_tx_idle），不能是 can_send() 的返回值：
     *   那个返回值只表示"成功入队"，总线发不出去时它照样返回成功。
     *   （我最初用"上次是否成功"做标志位，结果一旦失败就**永远不会再发**——
     *    总线恢复后也要人工重启，与 CAN 的自动恢复理念相悖。）
     *   查队列余量则让发送/暂停完全由总线状态驱动：恢复后队列自然清空，
     *   下一拍就会自动继续发心跳，**无需人工干预**。 */
    if (!can_tx_idle()) {
        return;
    }

    if ((now_ms - s_n1_last_ms) <= NODE_OFFLINE_MS) {
        online = 1;
    }

    hb.heartbeat_count    = s_hb_count++;
    hb.system_mode        = 0;        /* 未定义，置 0 */
    hb.online_bitmap      = (uint16_t)(online ? (1u << 1) : 0u);
    hb.fw_version_uniform = 0;        /* 0 = 各节点固件版本一致（本步只有节点一）*/
    hb.protocol_version   = (uint8_t)PROTO_PROTOCOL_VERSION;

    if (proto_encode_heartbeat(&hb, &f) == PROTO_OK) {
        (void)can_send(&f);
    }
}

/* 取空接收队列并处理。**必须循环取干净**：
 * 每拍只取一帧的话，节点一 1 Hz 遥测 + 突发 ACK/事件会把队列积压到丢帧。 */
static void can_pump_rx(uint32_t now_ms)
{
    proto_can_frame_t f;
    int               budget = 16;   /* 单拍最多处理 16 帧，避免饿死显示与陀螺仪 */

    while (budget-- > 0 && can_recv(&f)) {
        /* 统一用协议库的分派器，而不是自己 switch ID：
         * 帧类型将来增加时这里不用跟着改，也避免"漏掉某一类帧"这类错误
         * （本步就曾漏掉节点一的事件帧 0x2x0）。 */
        switch (f.id) {
        case PROTO_ID_TELEMETRY_NODE1:
        case PROTO_ID_ACK_NODE1:
        case PROTO_ID_NODE1_EVENT: {
            proto_decoded_t d;
            if (proto_decode_dispatch(&f, (uint8_t)PROTO_NODE_MASTER, &d) != PROTO_OK) {
                break;   /* 内容非法：不计入"节点活着"的证据 */
            }
            /* 只要能解出上述任一帧，就说明节点一确实在总线上说话 ——
             * 协议里没有节点侧心跳帧，在线证据就来自这些上行帧。 */
            s_n1_frames++;
            s_n1_last_ms = now_ms;

            if (f.id == PROTO_ID_TELEMETRY_NODE1) {
                /* temperature 单位 0.1℃，界面用 0.001℃ 的定点约定 */
                s_n1_temp_mdeg = (int32_t)d.u.telem1.temperature * 100;
                s_n1_status    = d.u.telem1.status;
            }
            break;
        }
        default:
            /* 其它 ID：本步不处理。刻意不计数也不报错——
             * 总线上出现无关帧是正常的，刷日志只会掩盖真正的问题。 */
            break;
        }
    }
}

/* 把 CAN 与节点一状态推给界面层（demo 层不碰 TWAI、不碰协议解码） */
static void can_push_to_ui(uint32_t now_ms)
{
    can_stats_t st;
    int         online;

    can_get_stats(&st);
    SGUI_DEMO_SetCanStats(&st);

    online = ((now_ms - s_n1_last_ms) <= NODE_OFFLINE_MS) ? 1 : 0;
    SGUI_DEMO_SetNode1Status(online, s_n1_temp_mdeg, (SGUI_INT)s_n1_status,
                             s_n1_frames);
}

void app_main(void)
{
    HMI_ENGINE_RESULT eResult;
    mpu6050_sample_t  sample;
    input_event_t     input;
    TickType_t        last_imu_tick;
    TickType_t        last_input_tick;
    TickType_t        last_info_tick;
    uint32_t          now_ms      = 0;
    uint32_t          last_hb_ms  = 0;
    uint32_t          last_diag_ms = 0;
    uint32_t          last_alive_ms = 0;
    SGUI_INT          iScreenBefore;
    SGUI_INT          iScreenAfter;
    int               i_oled_ok = 0;
    int               i_imu_ok  = 0;

    log_chip_info();
    sensors_init(&i_oled_ok, &i_imu_ok);

    /* 按键与编码器（GPIO8/12/47 + 编码器 GPIO6/7，见 input.h 的接线说明） */
    if (input_init() != 0) {
        ESP_LOGE(TAG, "输入初始化失败 (err=%d)", input_last_error());
    }

    /* CAN：必须在界面起来之前或之后都行，但**先建总线**能让 CAN 屏首次
     * 刷新就显示真实状态而不是 "NOCAN"。 */
    can_bringup();

    /* 拉起 SimpleGUI：内部会做 SCREEN_Initialize()（面板初始化 + 清显存），
     * 装配 SGUI_SCR_DEV 设备对象，初始化四个页面并**激活仪表盘**（默认屏）。 */
    eResult = SGUI_DEMO_Initialize();
    if (HMI_PROCESS_FAILED(eResult)) {
        ESP_LOGE(TAG, "SimpleGUI init failed (result=%d)", (int)eResult);
    } else {
        ESP_LOGI(TAG, "SimpleGUI ready (SSD1306 128x64 @ I2C0)");
    }

    /* 把"谁通了"和运行信息交给界面层显示（demo 层不直接调 esp_* API）。
     * 另外把实测的 WHO_AM_I 也送进去：陀螺仪读不到数据时，界面显示 "IDxx"
     * 还是 "no IMU" 能直接区分"兼容片"与"根本没应答"。 */
    SGUI_DEMO_SetBusStatus(i_oled_ok, i_imu_ok);
    SGUI_DEMO_SetImuId((int)mpu6050_who_am_i());
    SGUI_DEMO_SetSystemInfo(0, esp_get_free_heap_size() / 1024u);

    last_imu_tick   = xTaskGetTickCount();
    last_input_tick = xTaskGetTickCount();
    last_info_tick  = xTaskGetTickCount();
    /* 用 0 初始化"最近收到节点一的时刻"，让 (now - 0) > 3s 成立 →
     * 上电即为"离线"，避免界面在真的收到帧之前谎报在线。 */
    s_n1_last_ms    = 0;

    /* 主循环：驱动 GUI + 周期采样陀螺仪 + 轮询按键/编码器。
     *
     * 后续要接入的部分（本步暂不做）：
     *   1) CAN（TWAI）收发与协议层：用 firmware/common/protocol/ 的同一份协议库，
     *      ESP-IDF 侧把 proto_can_frame_t 与 twai_message_t 互转即可。
     *   2) 姿态解算：现在只显示**角速度**，不是姿态角；
     *      要出角度需再用加速度计做互补滤波/卡尔曼并标定零偏。 */
    for (;;) {
        /* --- 按键 / 编码器 --- */
        if ((xTaskGetTickCount() - last_input_tick) >= pdMS_TO_TICKS(INPUT_TICK_MS)) {
            last_input_tick = xTaskGetTickCount();

            if (input_poll(&input)) {
                iScreenBefore = SGUI_DEMO_CurrentScreenID();

                /* 编码器：一格 = 一次屏幕切换（UP=上一屏, DOWN=下一屏） */
                if (input.encoder_delta != 0) {
                    SGUI_DEMO_ProcessKey((input.encoder_delta > 0)
                                             ? SGUI_DEMO_KEY_DOWN
                                             : SGUI_DEMO_KEY_UP);
                }
                if (input.btn1_pressed) { SGUI_DEMO_ProcessKey(SGUI_DEMO_KEY_UP); }
                if (input.btn2_pressed) { SGUI_DEMO_ProcessKey(SGUI_DEMO_KEY_DOWN); }
                if (input.btn3_pressed) { SGUI_DEMO_ProcessKey(SGUI_DEMO_KEY_ENTER); }
                if (input.btn3_long)    { SGUI_DEMO_ProcessKey(SGUI_DEMO_KEY_ESC); }

                /* 屏幕真的换了就整屏重绘（清显存后重画目标屏）。
                 * 若不重绘，新屏内容不会出现 —— HMI 引擎的
                 * HMI_ProcessEvent() 只调 ProcessEvent + PostProcess，
                 * 不负责画新屏。 */
                iScreenAfter = SGUI_DEMO_CurrentScreenID();
                if (iScreenAfter != iScreenBefore) {
                    (void)SGUI_DEMO_GoToScreen(iScreenAfter);
                }
            }
        }

        /* --- 陀螺仪采样 --- */
        if ((xTaskGetTickCount() - last_imu_tick) >= pdMS_TO_TICKS(IMU_TICK_MS)) {
            last_imu_tick = xTaskGetTickCount();
            if (mpu6050_present()) {
                if (mpu6050_read(&sample) == 0) {
                    SGUI_DEMO_SetIMUData(sample.gyro_mdps, sample.temp_mdeg_c);
                    s_imu_err_logged = false;   /* 恢复后允许下次故障再报一次 */
                } else {
                    /* 读失败：必须让界面显示"无数据"，不能拿旧值冒充当前值 */
                    const int err = mpu6050_last_error();
                    SGUI_DEMO_SetIMUError(err);
                    /* ⚠️ 日志必须限流：这里每 50 ms 就执行一次。
                     * 实机踩坑（2026-10-04，COM4 实测）：陀螺仪开始持续读失败后，
                     * 日志被 "MPU6050 read failed (err=264)" 刷屏（每 50ms 一条），
                     * 把 CAN 联调最需要的诊断信息全淹没了。
                     * 只在"错误码发生变化"时打印：既保留了首次故障的证据，
                     * 又不会持续输出；错误码变了说明是另一类问题，值得再报一次。 */
                    if (!s_imu_err_logged || s_imu_err_last != err) {
                        ESP_LOGW(TAG, "MPU6050 read failed (err=%d) — 后续同类错误不再重复打印",
                                 err);
                        s_imu_err_last   = err;
                        s_imu_err_logged = true;
                    }
                }
                SGUI_DEMO_RefreshIMU();
            }
        }

        /* --- CAN 收发 ---------------------------------------------------
         * **每一拍都取帧**（不按 tick 计数节流）：节点一的遥测是突发的，
         * 攒着不取会把 32 深的接收队列填满并开始丢帧。
         * 心跳按 1 s 发一次，远小于协议规定的 3 s 离线门限。
         *
         * 时间基准统一用 esp_timer（微秒转毫秒）：它与 FreeRTOS tick 不同源，
         * 但两者都单调递增，用于"过了多久"的判断等价；这里刻意统一成一个
         * 来源，避免在同一处逻辑里混用两个时钟。 */
        now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        can_pump_rx(now_ms);
#if CAN_LOOPBACK_TEST
        /* 回环自测：自己发一帧，应当被自己收回（见 can_twai.h 的说明）。
         * 用带递增序号的测试 ID，便于确认收到的是**新**帧而不是残留。 */
        if ((now_ms - last_hb_ms) >= HEARTBEAT_TICK_MS) {
            last_hb_ms = now_ms;
            {
                proto_can_frame_t tf;
                memset(&tf, 0, sizeof(tf));
                tf.id      = 0x7E0u;              /* 测试用 ID，不占用协议帧表 */
                tf.dlc     = 2u;
                tf.data[0] = (uint8_t)(s_hb_count++);
                tf.data[1] = 0xA5u;
                (void)can_send(&tf);
            }
        }
#else
        if ((now_ms - last_hb_ms) >= HEARTBEAT_TICK_MS) {
            last_hb_ms = now_ms;
            can_send_heartbeat(now_ms);
        }
#endif
        can_push_to_ui(now_ms);

        /* --- 存活心跳（每 2 s 一行）---------------------------------------
         * 目的是把"主循环还在不在跑"变成**可观测**的。
         * 实机踩坑（2026-10-04）：一度出现"启动日志打完就再无输出"，
         * 无法区分是①循环卡死、②日志被刷没、还是③串口断了。
         * 一条只依赖循环次数的日志能把这三者区分开。 */
        if ((now_ms - last_alive_ms) >= CAN_DIAG_TICK_MS) {
            last_alive_ms = now_ms;
            ESP_LOGI(TAG, "alive tick=%" PRIu32 " heap=%" PRIu32 "B",
                     (uint32_t)(now_ms / 1000u), esp_get_free_heap_size());
        }

        /* --- CAN 诊断日志 -------------------------------------------------
         * 为什么需要：CAN 屏在设备上，而联调时人常常只能看串口。
         * 这一行把"判断总线通不通"所需的全部数字集中起来，无需调试器：
         *   tx/rx  —— 我们发出/收到的帧数
         *   fail   —— 发送失败次数（队列满/驱动拒绝）
         *   te/re  —— TEC/REC 错误计数器（te 涨到 256 即 Bus-Off）
         *   st     —— 总线状态
         *   n1     —— 节点一的帧数与在线标志
         * 判读要点：
         *   tx 不涨          → 我们自己没发出去（或队列被卡住）
         *   tx 涨但 n1 一直是 0 → 我们发得出去，但收不到对端 → **单向不通**
         *   te 涨            → 总线无 ACK 或干扰
         * 频次刻意取 5 s：足够看清趋势，又不会把启动日志挤掉。 */
        if ((now_ms - last_diag_ms) >= CAN_DIAG_TICK_MS) {
            last_diag_ms = now_ms;
            {
                can_stats_t d;
                can_get_stats(&d);
                const int n1_online =
                    ((now_ms - s_n1_last_ms) <= NODE_OFFLINE_MS) ? 1 : 0;
                ESP_LOGI(TAG, "CAN st=%s tx=%" PRIu32 " rx=%" PRIu32
                              " fail=%" PRIu32 " miss=%" PRIu32
                              " te=%" PRIu32 " re=%" PRIu32
                              " berr=%" PRIu32 " stuff=%" PRIu32
                              " form=%" PRIu32
                              " ack=%" PRIu32 " arb=%" PRIu32
                              " | n1=%" PRIu32 " online=%d",
                         can_state_str(d.state), d.tx_frames, d.rx_frames,
                         d.tx_failed, d.rx_missed, d.tx_error, d.rx_error,
                         d.bit_errors, d.stuff_errors, d.form_errors,
                         d.ack_errors, d.arb_lost,
                         s_n1_frames, n1_online);
            }
        }

        /* --- 运行信息（1 s 更新一次，给仪表盘用） --- */
        if ((xTaskGetTickCount() - last_info_tick) >= pdMS_TO_TICKS(INFO_TICK_MS)) {
            last_info_tick = xTaskGetTickCount();
            SGUI_DEMO_SetSystemInfo(
                (uint32_t)(esp_timer_get_time() / 1000000LL),
                esp_get_free_heap_size() / 1024u);
        }

        SGUI_DEMO_Process();
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_TICK_MS));
    }
}
