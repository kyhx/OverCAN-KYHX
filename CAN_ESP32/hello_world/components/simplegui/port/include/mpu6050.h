/*****************************************************************************
** File: mpu6050.h
** Description: MPU6050 六轴传感器（陀螺仪 + 加速度计 + 温度）驱动接口
**      平台：ESP32-S3 / ESP-IDF v6.x，**复用 SSD1306 已建立的 I2C 总线**
**      接线：与 OLED 共用 SDA=GPIO1 / SCL=GPIO2（docs/引脚分配.md §2.1）
**      地址：7 位 0x68（AD0 接地）/ 0x69（AD0 接 VCC）
**
** 为什么单独一个驱动而不是塞进 demo：
**   陀螺仪是核心机的姿态输入，后续还要喂给姿态解算与运动控制；
**   让显示层自己去读寄存器会把"传感器访问"和"界面绘制"耦合在一起。
**
** 免责声明（与 docs/求职项目陈述.md 的叙事红线一致）：
**   这里只做**原始值读取与量程换算**，不做姿态解算（欧拉角/四元数）。
**   界面上显示的是角速度（°/s），不是"姿态角"。要出角度必须再做
**   互补滤波/卡尔曼，且需要加速度计参与并标定零偏。
*****************************************************************************/
#ifndef __MPU6050_H__
#define __MPU6050_H__

#include <stdbool.h>
#include <stdint.h>

/* 7 位从机地址。AD0 接 GND 时为 0x68（模块默认）。 */
#define MPU6050_I2C_ADDR          (0x68)

/* WHO_AM_I(0x75) 的期望值。上电先验它，可挡掉翻新/山寨模块
 * （本项目的采购防坑清单里就有一条：MPU6050 翻新货多）。 */
#define MPU6050_WHO_AM_I_VALUE    (0x68)

/* 陀螺仪与加速度计量程（初始化时写入）。 */
typedef enum {
    MPU6050_GYRO_FS_250 = 0,   /* ±250 °/s  → 131  LSB/(°/s) */
    MPU6050_GYRO_FS_500,       /* ±500 °/s  → 65.5 LSB/(°/s) */
    MPU6050_GYRO_FS_1000,      /* ±1000 °/s → 32.8 LSB/(°/s) */
    MPU6050_GYRO_FS_2000       /* ±2000 °/s → 16.4 LSB/(°/s) */
} mpu6050_gyro_fs_t;

typedef enum {
    MPU6050_ACCEL_FS_2G = 0,   /* ±2g  → 16384 LSB/g */
    MPU6050_ACCEL_FS_4G,       /* ±4g  → 8192  LSB/g */
    MPU6050_ACCEL_FS_8G,       /* ±8g  → 4096  LSB/g */
    MPU6050_ACCEL_FS_16G       /* ±16g → 2048  LSB/g */
} mpu6050_accel_fs_t;

/* 一次采样的结果。角度量用整数毫单位表示，避免在 ISR/低内存环境下用浮点，
 * 也便于定点显示与后续定点滤波：
 *   gyro_mdps[i]  = 角速度，单位 0.001 °/s
 *   accel_mg[i]   = 加速度，单位 0.001 g
 *   temp_mdeg_c   = 温度，  单位 0.001 ℃
 * 数组下标：0 = X，1 = Y，2 = Z。 */
typedef struct {
    int32_t gyro_mdps[3];
    int32_t accel_mg[3];
    int32_t temp_mdeg_c;
} mpu6050_sample_t;

/**
 * @brief 在**已建立的 I2C 总线上**添加 MPU6050 设备。
 *
 * @param bus 由 SSD1306_GetBus() 取得的总线句柄（不可为 NULL）
 * @return 0 成功；非 0 为 ESP-IDF esp_err_t；-1 表示入参为空/未初始化
 *
 * @note 只挂设备、不做寄存器配置；配置见 mpu6050_init()。
 */
int mpu6050_bus_add(void *bus_handle);

/**
 * @brief 复位并配置 MPU6050（唤醒、量程、数字低通滤波），成功后校验 WHO_AM_I。
 *
 * @param gyro_fs  陀螺仪量程
 * @param accel_fs 加速度计量程
 * @return 0 成功；非 0 失败（含 WHO_AM_I 不匹配）
 */
int mpu6050_init(mpu6050_gyro_fs_t gyro_fs, mpu6050_accel_fs_t accel_fs);

/** @return true = 已成功初始化（I2C 能应答） */
bool mpu6050_present(void);

/**
 * @brief 返回实测到的 WHO_AM_I(0x75) 原始值（未初始化时返回 0）。
 *
 * 为什么要暴露它：山寨/翻新 MPU6050 的该寄存器常见非 0x68 的取值，
 * 但它们读数是正常的，因此驱动不会因此拒绝初始化。把真实值显示到界面上，
 * 现场就能一眼判断"是不是兼容片"，而不用去连串口看日志。
 */
uint8_t mpu6050_who_am_i(void);

/** @return 最近一次 I2C 操作的错误码（ESP-IDF esp_err_t；0 = 无错） */
int mpu6050_last_error(void);

/**
 * @brief 读一次全部数据（加速度 6 字节 + 温度 2 字节 + 角速度 6 字节）。
 *
 * 寄存器 0x3B..0x48 连续，因此**一次 I2C 事务**读完 14 字节，
 * 避免分多次读导致三个轴不是同一时刻的采样（快速转动时会出现轴间错位）。
 *
 * @param out 输出；不可为 NULL
 * @return 0 成功，非 0 失败
 */
int mpu6050_read(mpu6050_sample_t *out);

#endif /* __MPU6050_H__ */
