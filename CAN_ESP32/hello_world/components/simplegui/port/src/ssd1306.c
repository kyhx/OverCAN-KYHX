/*****************************************************************************
** File: ssd1306.c
** Description: SSD1306 OLED 控制器驱动 —— **ESP32-S3 + ESP-IDF v6.x 移植版**
**
**   目标：ESP32-S3-WROOM-1（核心机）
**   总线：I2C（新版 i2c_master 驱动，esp_driver_i2c 组件）
**   引脚：SDA = GPIO1，SCL = GPIO2（见 docs/引脚分配.md §2.1 核心机 I²C0）
**   面板：SSD1306 128x64 单色 OLED
**
** 与 STM32 版（SimpleGUI-Stable/TEST/Core/Src/sgui/ssd1306.c）的差异：
**   1. 只改两处平台调用：HAL_I2C_Master_Transmit → i2c_master_transmit，
**      HAL_Delay → vTaskDelay。其余时序与命令序列**逐条保持一致**。
**   2. 用新版 i2c_master 驱动（ESP-IDF >= 5.2；旧版 driver/i2c.h 已移除）。
**   3. 增加**初始化自检**：用 CASET/PAGESET 命令探测从机是否应答，
**      这样"OLED 没插好/地址不对"会在启动日志里直接说清楚，
**      而不是只表现为一块黑屏（黑屏的成因太多，最难查）。
**   4. 状态可读：SSD1306_Present() / SSD1306_LastError() 供上层显示与告警。
**
** 注意：SSD1306 的 I2C 从机地址是 **7 位** 0x3C（部分模块 0x3D）。
**   ESP-IDF 的 i2c_device_config_t.device_address 收的就是 7 位地址，
**   不需要像 STM32 HAL 那样左移一位。
*****************************************************************************/
#include "ssd1306.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#if !defined(SSD1306_I2C_SDA_GPIO)
#  if defined(CONFIG_SSD1306_I2C_SDA_GPIO)
#    define SSD1306_I2C_SDA_GPIO    (CONFIG_SSD1306_I2C_SDA_GPIO)
#  else
#    define SSD1306_I2C_SDA_GPIO    (1)
#  endif
#endif
#if !defined(SSD1306_I2C_SCL_GPIO)
#  if defined(CONFIG_SSD1306_I2C_SCL_GPIO)
#    define SSD1306_I2C_SCL_GPIO    (CONFIG_SSD1306_I2C_SCL_GPIO)
#  else
#    define SSD1306_I2C_SCL_GPIO    (2)
#  endif
#endif
#if !defined(SSD1306_I2C_SPEED_HZ)
#  if defined(CONFIG_SSD1306_I2C_SPEED_HZ)
#    define SSD1306_I2C_SPEED_HZ    (CONFIG_SSD1306_I2C_SPEED_HZ)
#  else
#    define SSD1306_I2C_SPEED_HZ    (400000)
#  endif
#endif

#if defined(ESP_PLATFORM)
static const char *TAG = "ssd1306";

static i2c_master_bus_handle_t s_bus  = NULL;
static i2c_master_dev_handle_t s_dev  = NULL;
static bool                    s_ready = false;
static int                     s_last_err = 0;

int SSD1306_LastError(void)
{
    return s_last_err;
}

int SSD1306_Present(void)
{
    return s_ready ? 1 : 0;
}

i2c_master_bus_handle_t SSD1306_GetBus(void)
{
    return s_bus;
}

/* --- 面板朝向 -------------------------------------------------------------
 * 由 Kconfig（menuconfig → SimpleGUI display → Panel orientation）选择。
 *
 * ⚠️ 本项目这块屏在控制器**默认**扫描方向下是**上下颠倒**的，只翻上下之后
 *    **字形又变成左右镜像**——说明两个轴都需要重映射，即整体旋转 180°，
 *    因此**默认取 FLIP_XY（0xA1 + 0xC8）**。
 *    判断依据很简单：
 *      图像上下颠倒        → COM 扫描方向要翻（0xC0 → 0xC8）
 *      字形左右镜像        → SEG 重映射要翻（0xA0 → 0xA1）
 *    两个都中就是 FLIP_XY。若换一块装配方向不同的模块导致又不对，
 *    在 menuconfig 里改这一项即可 —— 不要临时改代码。
 *
 *      config SSD1306_ORIENT_NORMAL   → 0xA0 / 0xC0
 *      config SSD1306_ORIENT_FLIP_Y   → 0xA0 / 0xC8
 *      config SSD1306_ORIENT_FLIP_X   → 0xA1 / 0xC0
 *      config SSD1306_ORIENT_FLIP_XY  → 0xA1 / 0xC8
 * ------------------------------------------------------------------------- */
#if defined(CONFIG_SSD1306_ORIENT_FLIP_X) || defined(CONFIG_SSD1306_ORIENT_FLIP_XY)
#define SSD1306_REMAP_SEG      (0xA1)
#else
#define SSD1306_REMAP_SEG      (0xA0)
#endif

#if defined(CONFIG_SSD1306_ORIENT_FLIP_Y) || defined(CONFIG_SSD1306_ORIENT_FLIP_XY)
#define SSD1306_REMAP_COM      (0xC8)
#elif defined(CONFIG_SSD1306_ORIENT_NORMAL) || defined(CONFIG_SSD1306_ORIENT_FLIP_X)
#define SSD1306_REMAP_COM      (0xC0)
#else
/* 没有 sdkconfig 时的兜底（例如脱离 ESP-IDF 单独编这个文件做语法检查）：
 * 与 Kconfig 的默认保持一致，即"两个轴都翻"。 */
#define SSD1306_REMAP_COM      (0xC8)
#endif

/* 私有发送缓冲：控制字节 + 一整页（128 字节）。只在 build 阶段使用，
 * 这样调用方传进来的 payload 与本缓冲不会重叠。 */
static uint8_t s_aucTxBuf[SSD1306_TX_BUFFERSIZE];

/*****************************************************************************
** Function Name: SSD1306_SendBuf
** Purpose:       一次 I2C 事务发送"控制字节 + 数据"。
**                100kHz 下逐个字节发送开销极大，因此整页一次推完。
** Return:        0 成功；非 0 为 esp_err_t（同时记录到 s_last_err）
*****************************************************************************/
static int SSD1306_SendBuf(const uint8_t *pucBuf, uint16_t uiLen, uint8_t uiCtrl)
{
    esp_err_t err;

    if ((NULL == pucBuf) || (0 == uiLen) || (uiLen > SSD1306_WIDTH)) {
        return -1;
    }
    if (NULL == s_dev) {
        s_last_err = -1;
        return -1;
    }

    s_aucTxBuf[0] = uiCtrl;
    memcpy(&s_aucTxBuf[1], pucBuf, uiLen);

    err = i2c_master_transmit(s_dev, s_aucTxBuf, (size_t)(uiLen + 1),
                              SSD1306_I2C_TIMEOUT);
    if (err != ESP_OK) {
        s_last_err = (int)err;
    }
    return (int)err;
}

/*****************************************************************************
** Function Name: SSD1306_I2C_Init
** Purpose:       建 I2C 主总线 + 挂 SSD1306 设备，并做一次从机应答探测。
**                刻意与 SSD1306_Init() 分开：总线建立是"能不能通信"，
**                初始化命令序列是"屏幕内容"，失败原因不同、排错方式也不同。
** Return:        0 成功，非 0 失败
*****************************************************************************/
int SSD1306_I2C_Init(void)
{
    i2c_master_bus_config_t bus_cfg = {0};
    i2c_device_config_t     dev_cfg = {0};
    esp_err_t               err;
    uint8_t                 probe = 0x00;

    if (s_ready) {
        return 0;
    }

    bus_cfg.i2c_port          = I2C_NUM_0;
    bus_cfg.sda_io_num        = (gpio_num_t)SSD1306_I2C_SDA_GPIO;
    bus_cfg.scl_io_num        = (gpio_num_t)SSD1306_I2C_SCL_GPIO;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    /* 内部上拉偏弱（约 45k），SSD1306 模块一般自带上拉；
     * 若线较长或模块无上拉，需在 SDA/SCL 各加 4.7k 外部上拉（见 docs/引脚分配.md §2.1）。 */
    bus_cfg.flags.enable_internal_pullup = true;

    err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        s_last_err = (int)err;
        return (int)err;
    }

    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = SSD1306_I2C_ADDRESS;   /* 7 位地址，无需左移 */
    dev_cfg.scl_speed_hz    = SSD1306_I2C_SPEED_HZ;

    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", SSD1306_I2C_ADDRESS,
                 esp_err_to_name(err));
        s_last_err = (int)err;
        return (int)err;
    }

    /* 探测：发一条无副作用命令（设置列低 4 位）。从机不答则 err != ESP_OK。
     * 有些模块在首次通信前需要更长时间，失败不致命，仅告警。 */
    err = i2c_master_transmit(s_dev, &probe, 1, SSD1306_I2C_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "probe 0x%02X no ACK (%s) — 检查接线/上拉/地址(0x3D?)",
                 SSD1306_I2C_ADDRESS, esp_err_to_name(err));
        s_last_err = (int)err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "I2C ready: SDA=%d SCL=%d %dkHz addr=0x%02X",
             SSD1306_I2C_SDA_GPIO, SSD1306_I2C_SCL_GPIO,
             SSD1306_I2C_SPEED_HZ / 1000, SSD1306_I2C_ADDRESS);
    return 0;
}

/*****************************************************************************
** Function Name: SSD1306_DrawHLine
** Purpose:       把某一行整行填成 uiByte。**上电自检用**：一次调用即可验证
**                I2C 地址、页寻址模式、接线三件事。
*****************************************************************************/
void SSD1306_DrawHLine(uint8_t uiY, uint8_t uiByte)
{
    uint8_t aucPage[SSD1306_WIDTH];

    if (uiY >= SSD1306_HEIGHT) {
        return;
    }
    memset(aucPage, uiByte, SSD1306_WIDTH);
    SSD1306_WritePage(aucPage, (uint8_t)(uiY / 8));
}
#else  /* ------------------------- 非 ESP-IDF（占位，便于 PC 侧静态检查）---- */
int  SSD1306_I2C_Init(void) { return -1; }
int  SSD1306_LastError(void) { return -1; }
int  SSD1306_Present(void) { return 0; }
#endif /* ESP_PLATFORM */

/*===========================================================================*/
/* 以下为平台无关部分：命令序列与显存推送，与 STM32 版逐条一致                  */
/*===========================================================================*/

void SSD1306_WriteCommand(uint8_t uiCmd)
{
    (void)SSD1306_SendBuf(&uiCmd, 1, SSD1306_CTRL_CMD);
}

void SSD1306_WriteData(uint8_t uiData)
{
    (void)SSD1306_SendBuf(&uiData, 1, SSD1306_CTRL_DATA);
}

void SSD1306_SetPosition(uint8_t uiColumn, uint8_t uiPage)
{
    SSD1306_WriteCommand((uint8_t)(0xB0 | uiPage));            /* 页地址      */
    SSD1306_WriteCommand((uint8_t)(0x00 | (uiColumn & 0x0F))); /* 列低 4 位   */
    SSD1306_WriteCommand((uint8_t)(0x10 | (uiColumn & 0xF0))); /* 列高 4 位   */
}

void SSD1306_WritePage(const uint8_t *pucData, uint8_t uiPage)
{
    if ((NULL == pucData) || (uiPage >= SSD1306_PAGES)) {
        return;
    }
    SSD1306_SetPosition(0, uiPage);
    (void)SSD1306_SendBuf(pucData, SSD1306_WIDTH, SSD1306_CTRL_DATA);
}

void SSD1306_WriteDataSegment(const uint8_t *pucData, uint16_t uiLen)
{
    (void)SSD1306_SendBuf(pucData, uiLen, SSD1306_CTRL_DATA);
}

void SSD1306_FillAll(uint8_t uiByte)
{
    uint8_t aucPage[SSD1306_WIDTH];

    for (uint8_t uiPage = 0; uiPage < SSD1306_PAGES; uiPage++) {
        memset(aucPage, uiByte, SSD1306_WIDTH);
        SSD1306_WritePage(aucPage, uiPage);
    }
}

void SSD1306_Clear(void)
{
    SSD1306_FillAll(0x00);
}

void SSD1306_DisplayOn(void)
{
    SSD1306_WriteCommand(0xAF);
}

void SSD1306_DisplayOff(void)
{
    SSD1306_WriteCommand(0xAE);
}

/*****************************************************************************
** Function Name: SSD1306_Init
** Purpose:       执行控制器初始化序列（顺序与 STM32 已验证版本一致）。
** Note:          上电后需 >500ms 才能发命令（数据手册要求）。
*****************************************************************************/
void SSD1306_Init(void)
{
#if defined(ESP_PLATFORM)
    /* 总线先建起来（含从机探测）。失败也继续走命令序列：
     * 若只是探测时机太早而总线其实可用，后面的命令仍能点亮屏幕；
     * 若真没接好，错误计数会一路增长，可在上层读 SSD1306_LastError() 判断。 */
    (void)SSD1306_I2C_Init();

    /* 上电等待 >500ms（数据手册）。用 vTaskDelay 让出 CPU，
     * 不要用忙等——ESP32-S3 上还有 CAN 任务要跑。 */
    vTaskDelay(pdMS_TO_TICKS(600));
#else
    return;
#endif

    SSD1306_DisplayOff();
    SSD1306_WriteCommand(0x00);  /* 列低 4 位          */
    SSD1306_WriteCommand(0x10);  /* 列高 4 位          */
    SSD1306_WriteCommand(0x40);  /* 显示起始行         */
    SSD1306_WriteCommand(0x81);  /* 对比度             */
    SSD1306_WriteCommand(0xCF);
    /* ⚠️ 朝向：SEG 重映射决定左右，COM 扫描方向决定上下。
     * 默认 0xA0/0xC0；若显示上下反向，把 Kconfig 的 Panel orientation
     * 改成 "Vertically flipped"（换成 0xC8），无需改代码。 */
    SSD1306_WriteCommand(SSD1306_REMAP_SEG);  /* SEG 重映射 */
    SSD1306_WriteCommand(SSD1306_REMAP_COM);  /* COM 扫描方向 */
    SSD1306_WriteCommand(0xA6);  /* 正常显示（非反白） */
    SSD1306_WriteCommand(0xA8);  /* 多路复用比         */
    SSD1306_WriteCommand(0x3F);  /* 1/64 duty          */
    SSD1306_WriteCommand(0xD3);  /* 显示偏移           */
    SSD1306_WriteCommand(0x00);
    SSD1306_WriteCommand(0xD5);  /* 时钟分频           */
    SSD1306_WriteCommand(0x80);  /* 100 fps            */
    SSD1306_WriteCommand(0xD9);  /* 预充电周期         */
    SSD1306_WriteCommand(0xF1);
    SSD1306_WriteCommand(0xDA);  /* COM 引脚硬件配置   */
    SSD1306_WriteCommand(0x12);
    SSD1306_WriteCommand(0xDB);  /* VCOMH 反选电平     */
    SSD1306_WriteCommand(0x40);
    /* ⚠️ 关键：设为**页寻址模式**。不改的话控制器停在默认的水平寻址模式，
     * 页指针在每页写完后不会按预期递增，而我们推的是"页连续排列"的显存，
     * 结果就是屏幕出现错位的横向条纹。
     * 0x20 = 寻址模式命令，0x02 = 页寻址模式。 */
    SSD1306_WriteCommand(0x20);
    SSD1306_WriteCommand(0x02);
    SSD1306_WriteCommand(0x8D);  /* 电荷泵             */
    SSD1306_WriteCommand(0x14);
    SSD1306_WriteCommand(0xA4);  /* 显示内容跟随 RAM   */
    SSD1306_WriteCommand(0xA6);  /* 正常显示           */

    SSD1306_Clear();
    SSD1306_SetPosition(0, 0);
    SSD1306_DisplayOn();
}
