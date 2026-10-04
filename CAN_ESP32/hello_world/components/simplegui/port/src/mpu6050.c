/*****************************************************************************
** File: mpu6050.c
** Description: MPU6050 六轴传感器驱动（ESP-IDF v6.x 新版 I2C master API）
**
** 设计要点
**   1. **复用 SSD1306 的总线**：两者在同一对引脚上，各自 new_master_bus 会
**      因端口占用失败。调用方按顺序：SSD1306_I2C_Init() → mpu6050_bus_add(bus)。
**   2. **一次事务读完 14 字节**（0x3B..0x48 连续）：分三次读加速度/温度/角速度
**      会让三个轴不在同一时刻，快速转动时数据自相矛盾。
**   3. **全程整数定点**：原始 → 物理量用 64 位乘除，避免浮点。
**      输出单位取"毫"级（mdps / mg / mdegC），既有 0.001 分辨率，
**      又便于定点滤波与显示，不引入 float 依赖。
**   4. WHO_AM_I 校验：挡掉翻新/山寨模块（本项目采购防坑清单里的一条）。
**
** 量程与灵敏度（数据手册 MPU-6000/6050 Register Map）
**   陀螺仪：±250→131、±500→65.5、±1000→32.8、±2000→16.4  LSB/(°/s)
**   加速度：±2→16384、±4→8192、±8→4096、±16→2048        LSB/g
**   温度  ：T[℃] = raw/340 + 36.53  → 这里换算成 0.001 ℃：
**           mdegC = raw*1000/340 + 36530
*****************************************************************************/
#include "mpu6050.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

/* ------------------------------- 寄存器 ---------------------------------- */
#define REG_SMPLRT_DIV       (0x19)
#define REG_CONFIG           (0x1A)
#define REG_GYRO_CONFIG      (0x1B)
#define REG_ACCEL_CONFIG     (0x1C)
#define REG_ACCEL_XOUT_H     (0x3B)   /* 数据区起始：AX_H ... GZ_L, TEMP */
#define REG_PWR_MGMT_1       (0x6B)
#define REG_PWR_MGMT_2       (0x6C)
#define REG_WHO_AM_I         (0x75)

#define DATA_LEN             (14)     /* AX/AY/AZ(6) + TEMP(2) + GX/GY/GZ(6) */

/* 要扫描的 7 位地址（AD0 电平决定）。
 * ⭐ 为什么必须扫描：GY-521 这类模块**板上给 AD0 加了上拉到 VCC 的电阻**，
 *    实际地址是 0x69 而不是数据手册默认的 0x68。只认 0x68 时会得到
 *    ESP_ERR_INVALID_STATE(264) "device not found"，而且从错误码上完全看不出
 *    是地址问题 —— 现场表现为"陀螺仪一直没数值"。 */
static const uint8_t s_scan_addrs[] = { 0x68, 0x69 };

#if defined(ESP_PLATFORM)
static const char *TAG = "mpu6050";

static i2c_master_dev_handle_t s_dev      = NULL;
static i2c_master_bus_handle_t s_bus      = NULL;   /* 供扫地址时换设备用 */
static uint8_t                 s_addr     = 0;      /* 实际命中的 7 位地址 */
static bool                    s_present  = false;
static int                     s_last_err = 0;
static uint8_t                 s_who_am_i = 0;   /* 实测 WHO_AM_I，供界面显示 */
static int32_t                 s_gyro_lsb_per_dps_x1000 = 0;  /* 131 → 131000 */
static int32_t                 s_accel_lsb_per_g        = 0;  /* 16384        */

/* 陀螺仪灵敏度，单位 0.001 LSB/(°/s)：除以 1000 得整灵敏度的 131 等值 */
static int32_t gyro_sens_x1000(mpu6050_gyro_fs_t fs)
{
    switch (fs) {
    case MPU6050_GYRO_FS_500:  return 65500;   /* 65.5  */
    case MPU6050_GYRO_FS_1000: return 32800;   /* 32.8  */
    case MPU6050_GYRO_FS_2000: return 16400;   /* 16.4  */
    case MPU6050_GYRO_FS_250:
    default:                   return 131000;  /* 131   */
    }
}

static int32_t accel_sens_lsb_per_g(mpu6050_accel_fs_t fs)
{
    switch (fs) {
    case MPU6050_ACCEL_FS_4G:  return 8192;
    case MPU6050_ACCEL_FS_8G:  return 4096;
    case MPU6050_ACCEL_FS_16G: return 2048;
    case MPU6050_ACCEL_FS_2G:
    default:                   return 16384;
    }
}

static int i2c_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
    if (err != ESP_OK) {
        s_last_err = (int)err;
    }
    return (int)err;
}

static int i2c_read_regs(uint8_t reg, uint8_t *dst, size_t len)
{
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, dst, len, 100);
    if (err != ESP_OK) {
        s_last_err = (int)err;
    }
    return (int)err;
}

int mpu6050_last_error(void)
{
    return s_last_err;
}

bool mpu6050_present(void)
{
    return s_present;
}

uint8_t mpu6050_who_am_i(void)
{
    return s_who_am_i;
}

int mpu6050_bus_add(void *bus_handle)
{
    if (NULL == bus_handle) {
        s_last_err = -1;
        return -1;
    }
    /* 只保存总线句柄；具体用哪个地址在 mpu6050_init() 里扫描决定
     * （因为要先能通信才能读 WHO_AM_I，而地址本身就是要探测的对象）。 */
    s_bus = (i2c_master_bus_handle_t)bus_handle;
    return 0;
}

/* 在指定 7 位地址上挂设备。成功返回 0 并填好 s_dev/s_addr。 */
static int attach_device(uint8_t addr7)
{
    i2c_device_config_t cfg = {0};
    esp_err_t           err;

    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = addr7;
    cfg.scl_speed_hz    = 400000;

    err = i2c_master_bus_add_device(s_bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        return (int)err;
    }
    s_addr = addr7;
    return 0;
}

/* 探测一个地址上的设备是否应答：读 WHO_AM_I(0x75)。
 * 能读回任意字节就说明有器件应答（内容对不对是后面的事）。 */
static int probe_addr(uint8_t addr7, uint8_t *out_who)
{
    uint8_t reg = REG_WHO_AM_I;
    uint8_t val = 0;
    esp_err_t err;

    if (attach_device(addr7) != 0) {
        return -1;
    }
    err = i2c_master_transmit_receive(s_dev, &reg, 1, &val, 1, 100);
    if (err != ESP_OK) {
        /* 不应答 → 摘掉设备，换下一个地址试 */
        (void)i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return (int)err;
    }
    if (out_who != NULL) {
        *out_who = val;
    }
    return 0;
}

int mpu6050_init(mpu6050_gyro_fs_t gyro_fs, mpu6050_accel_fs_t accel_fs)
{
    uint8_t who = 0;
    int     rc;
    size_t  i;

    if (NULL == s_bus) {
        s_last_err = -1;
        return -1;   /* 没调用 mpu6050_bus_add() */
    }

    /* 0. 扫描地址：AD0 接地 = 0x68，AD0 接 VCC（GY-521 模块板载上拉常如此）= 0x69。
     *    只认 0x68 会让"地址不同"和"没接上"看起来是一回事（都是 INVALID_STATE）。 */
    s_dev = NULL;
    rc = -1;
    for (i = 0; i < sizeof(s_scan_addrs) / sizeof(s_scan_addrs[0]); i++) {
        rc = probe_addr(s_scan_addrs[i], &who);
        if (0 == rc) {
            ESP_LOGI(TAG, "found device at 0x%02X (WHO_AM_I=0x%02X)",
                     s_scan_addrs[i], who);
            break;
        }
    }
    if (0 != rc || NULL == s_dev) {
        ESP_LOGE(TAG, "no MPU6050 on I2C bus (tried 0x68 and 0x69): %s",
                 esp_err_to_name((esp_err_t)rc));
        s_last_err = rc;
        return rc;
    }

    /* 1. 复位（PWR_MGMT_1 bit7 DEVICE_RESET）。
     *    延时给足 200 ms：数据手册要求的复位时间在 100 ms 量级，但翻新模块
     *    常常更慢；复位没完成就写的配置会被丢掉，表现为"读数恒为 0"。 */
    (void)i2c_write_reg(REG_PWR_MGMT_1, 0x80);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 2. 唤醒：退出睡眠，时钟源选 PLL(PLL_X_GYRO, 0x01)。
     *    默认内部 8MHz 振荡器精度差，会直接体现在角速度零偏上。 */
    rc = i2c_write_reg(REG_PWR_MGMT_1, 0x01);
    if (rc != 0) { return rc; }
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 3. 再读一次身份（复位后重新确认）。
     *
     * ⚠️ 这里**只记录、不再拒绝**。原因：山寨/翻新 MPU6050 的 WHO_AM_I 常见
     *    非 0x68 的取值（0x70 / 0x72 / 0x73 / 0x98 等），但它们**读数是正常的**。
     *    早先的实现在这里 return，结果传感器整个不可用，屏幕上陀螺仪永远是
     *    "----" —— 典型的"防卫过度把功能挡死了"。 */
    rc = i2c_read_regs(REG_WHO_AM_I, &who, 1);
    if (rc != 0) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s (addr 0x%02X)",
                 esp_err_to_name((esp_err_t)rc), s_addr);
        return rc;
    }
    s_who_am_i = who;
    if (who != MPU6050_WHO_AM_I_VALUE) {
        /* 日志文本刻意用**纯 ASCII**：串口终端/日志抓取工具对中文与破折号的
         * 编码处理并不一致（实机在 COM4 上抓到的这行就是乱码）。
         * 日志是排错用的，可读性优先于"美观"，不值得为中文冒编码风险。 */
        ESP_LOGW(TAG, "WHO_AM_I=0x%02X (datasheet 0x%02X) - clone/compatible chip, continuing",
                 who, MPU6050_WHO_AM_I_VALUE);
    }

    /* 4. 采样率：SMPLRT_DIV=9 → 1kHz/(1+9) = 100Hz，与我们的显示节奏匹配 */
    (void)i2c_write_reg(REG_SMPLRT_DIV, 0x09);

    /* 5. 数字低通滤波 DLPF_CFG=3 → 约 44Hz 带宽、1kHz 陀螺采样。
     *    机械振动（电机/舵机）很脏，开滤波能显著改善读数稳定性。 */
    (void)i2c_write_reg(REG_CONFIG, 0x03);

    /* 6. 量程：陀螺 FS_SEL 与加速度 AFS_SEL 都放在各自寄存器的高 3 位 */
    rc = i2c_write_reg(REG_GYRO_CONFIG, (uint8_t)((uint8_t)gyro_fs << 3));
    if (rc != 0) { return rc; }
    rc = i2c_write_reg(REG_ACCEL_CONFIG, (uint8_t)((uint8_t)accel_fs << 3));
    if (rc != 0) { return rc; }

    /* 7. 确保所有轴都使能（PWR_MGMT_2 = 0） */
    (void)i2c_write_reg(REG_PWR_MGMT_2, 0x00);

    s_gyro_lsb_per_dps_x1000 = gyro_sens_x1000(gyro_fs);
    s_accel_lsb_per_g        = accel_sens_lsb_per_g(accel_fs);
    s_present                = true;

    ESP_LOGI(TAG, "ready: addr=0x%02X WHO_AM_I=0x%02X gyro_fs=%d accel_fs=%d",
             s_addr, who, (int)gyro_fs, (int)accel_fs);
    return 0;
}

int mpu6050_read(mpu6050_sample_t *out)
{
    uint8_t buf[DATA_LEN];
    int     rc;
    int     i;

    if (NULL == out || NULL == s_dev) {
        return -1;
    }

    /* 一次读完 0x3B..0x48：保证三轴同一时刻，避免快速转动时轴间错位 */
    rc = i2c_read_regs(REG_ACCEL_XOUT_H, buf, DATA_LEN);
    if (rc != 0) {
        return rc;
    }

    /* 每个轴都是 big-endian 有符号 16 位 */
    for (i = 0; i < 3; i++) {
        const int32_t raw = (int32_t)(int16_t)(((uint16_t)buf[i * 2] << 8) |
                                                (uint16_t)buf[i * 2 + 1]);
        /* raw[LSB] ÷ (LSB/(°/s)) × 1000 → 0.001 °/s
         * sens 是 0.001 单位，故乘 1000 再除 sens，等价于 ×1000000/sens_x1000 */
        out->accel_mg[i] =
            (int32_t)(((int64_t)raw * 1000) / s_accel_lsb_per_g);
    }

    {
        const int32_t raw_t = (int32_t)(int16_t)(((uint16_t)buf[6] << 8) |
                                                  (uint16_t)buf[7]);
        /* T[℃] = raw/340 + 36.53 → 0.001 ℃ */
        out->temp_mdeg_c = (int32_t)(((int64_t)raw_t * 1000) / 340 + 36530);
    }

    for (i = 0; i < 3; i++) {
        const int32_t raw = (int32_t)(int16_t)(((uint16_t)buf[8 + i * 2] << 8) |
                                                (uint16_t)buf[8 + i * 2 + 1]);
        out->gyro_mdps[i] =
            (int32_t)(((int64_t)raw * 1000000) / s_gyro_lsb_per_dps_x1000);
    }
    return 0;
}

#else  /* 非 ESP-IDF：占位，保证 PC 侧静态检查能过 */

int  mpu6050_bus_add(void *b) { (void)b; return -1; }
int  mpu6050_init(mpu6050_gyro_fs_t g, mpu6050_accel_fs_t a) { (void)g; (void)a; return -1; }
bool mpu6050_present(void) { return false; }
uint8_t mpu6050_who_am_i(void) { return 0; }
int  mpu6050_last_error(void) { return -1; }
int  mpu6050_read(mpu6050_sample_t *o) { (void)o; return -1; }

#endif /* ESP_PLATFORM */
