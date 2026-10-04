/*****************************************************************************
** File: ssd1306.h
** Description: SSD1306 OLED 控制器驱动接口
**      目标平台：ESP32-S3-WROOM-1（ESP-IDF v6.x，新版 i2c_master 驱动）
**      面板    ：128 x 64 单色 OLED
**      I2C     ：SDA = GPIO1，SCL = GPIO2，400 kHz，7 位从机地址 0x3C
**
** 移植说明：本头文件同时被 ESP32 与 STM32 两个移植版使用，
**   因此**不再包含任何平台头**（原版包含 stm32f1xx_hal.h，会把 HAL 拖进来）。
**   平台相关部分留在 .c 里，用 ESP_PLATFORM 宏区分。
*****************************************************************************/
#ifndef __SSD1306_H__
#define __SSD1306_H__

#include <stdint.h>

/* I2C 从机地址：**7 位**。多数 0.96"/1.3" 模块为 0x3C，少数为 0x3D。
 * 注意：ESP-IDF 的 i2c_device_config_t.device_address 收 7 位地址，
 *       不要像 STM32 HAL 那样左移一位。 */
#ifndef SSD1306_I2C_ADDRESS
#define SSD1306_I2C_ADDRESS        (0x3C)
#endif

/* 面板几何 */
#define SSD1306_WIDTH              (128)
#define SSD1306_HEIGHT             (64)
#define SSD1306_PAGES              (SSD1306_HEIGHT / 8)    /* 8 页 */

/* 控制字节：bit6 = Co（续传），bit5 = D/C#（0=命令，1=数据） */
#define SSD1306_CTRL_CMD           (0x00)
#define SSD1306_CTRL_DATA          (0x40)

/* I2C 单次事务超时（ms） */
#define SSD1306_I2C_TIMEOUT        (100)

/* 单次 I2C 事务最大字节数：控制字节 + 一整页 */
#define SSD1306_TX_BUFFERSIZE      (SSD1306_WIDTH + 1)

void SSD1306_Init(void);
void SSD1306_WriteCommand(uint8_t uiCmd);
void SSD1306_WriteData(uint8_t uiData);
void SSD1306_SetPosition(uint8_t uiColumn, uint8_t uiPage);
void SSD1306_WritePage(const uint8_t* pucData, uint8_t uiPage);
void SSD1306_WriteDataSegment(const uint8_t* pucData, uint16_t uiLen);
void SSD1306_FillAll(uint8_t uiByte);
void SSD1306_Clear(void);
void SSD1306_DrawHLine(uint8_t uiY, uint8_t uiByte);
void SSD1306_DisplayOn(void);
void SSD1306_DisplayOff(void);

/* 以下三个是 ESP32 移植版新增的（STM32 版没有），用于把"I2C 到底通没通"
 * 变成一个可读的状态，而不是一块无从下手的黑屏：
 *   SSD1306_I2C_Init()  单独建立总线并探测从机；可先于 Init 调用以便定位问题
 *   SSD1306_Present()   从机是否应答过（1 = 通）
 *   SSD1306_LastError() 最近一次 I2C 错误（esp_err_t；0 表示无错）
 * 在非 ESP-IDF 平台上，这三个返回失败/0，保证接口一致。 */
int  SSD1306_I2C_Init(void);
int  SSD1306_Present(void);
int  SSD1306_LastError(void);

#if defined(ESP_PLATFORM)
#include "driver/i2c_master.h"
/* 暴露已建立的 I2C 主总线句柄，供同总线上的其它从机复用。
 * 本项目里是 MPU6050（陀螺仪）：它与 OLED 共用 GPIO1/2 这一条 I2C 总线，
 * 若各自去 i2c_new_master_bus() 会因端口已被占用而失败。
 * 返回 NULL 表示总线尚未建立（未调用 SSD1306_I2C_Init）。 */
i2c_master_bus_handle_t SSD1306_GetBus(void);
#endif

#endif /* __SSD1306_H__ */
