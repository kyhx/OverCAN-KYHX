/**
 * @file  proto_crc.h
 * @brief CRC 实现：固件升级链路的块级校验与整包校验。
 *
 * 参数（标准取值，必须与 PC 侧工具一致）：
 *   - **CRC-16/CCITT-FALSE**：poly 0x1021，init 0xFFFF，无反射，无输出异或
 *     → 用于**块级**校验（每个传输块 128 B 量级），错误即重传该块
 *   - **CRC-32/ISO-HDLC**（= zlib crc32）：poly 0x04C11DB7，init 0xFFFFFFFF，
 *     反射输入输出，输出异或 0xFFFFFFFF
 *     → 用于**整包**校验（元数据里携带，升级结束前重算比对）
 *
 * 为什么块级 + 整包都要：块级保证"这一块对了"（可只重传该块，代价小），
 * 整包保证"最终一致"（防止块级都通过但顺序/总数出错）。
 *
 * 流式 API 的用途：STM32 只有 20 KB SRAM，装不下 64 KB 镜像，
 * 必须"边收边写 flash"，所以校验和也必须能流式累加。
 *
 * 纯 C99、零 HAL 依赖、无 malloc。
 */
#ifndef PROTO_CRC_H
#define PROTO_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------- CRC-16/CCITT-FALSE -------------------------- */

#define PROTO_CRC16_CCITT_POLY 0x1021u
#define PROTO_CRC16_CCITT_INIT 0xFFFFu
/** "123456789" 的标准校验值（单元测试锚点）。 */
#define PROTO_CRC16_CCITT_CHECK 0x29B1u

/** 一次性计算。 */
uint16_t proto_crc16_ccitt(const uint8_t *data, size_t len);

/** 流式：从 seed 开始继续累加（首块用 PROTO_CRC16_CCITT_INIT）。 */
uint16_t proto_crc16_ccitt_update(uint16_t seed, const uint8_t *data, size_t len);

/* ----------------------------- CRC-32/ISO-HDLC --------------------------- */

#define PROTO_CRC32_POLY 0x04C11DB7u
#define PROTO_CRC32_INIT 0xFFFFFFFFu
#define PROTO_CRC32_XOROUT 0xFFFFFFFFu
/** "123456789" 的标准校验值（= zlib.crc32(b"123456789")）。 */
#define PROTO_CRC32_CHECK 0xCBF43926u

/** 一次性计算。等价于 zlib crc32，便于 PC 侧交叉验证。 */
uint32_t proto_crc32(const uint8_t *data, size_t len);

/**
 * 流式：从 seed 开始继续累加。
 * @note 首块必须传 PROTO_CRC32_INIT。真正的 init/xorout 只在整个流的
 *       第一次与最后一次体现；因此这里的 seed 语义是"上一次的返回值"。
 */
uint32_t proto_crc32_update(uint32_t seed, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_CRC_H */
