/**
 * @file  proto_bytes.h
 * @brief 小端字节序读写辅助。
 *
 * 为什么存在：协议内统一小端、且**禁止使用 C 位域**（位域布局为实现定义，
 * 跨编译器不可靠）。所有多字节字段与位域操作都必须经这里，禁止在业务代码里
 * 手写裸移位，避免端序与符号扩展在不同平台出现分歧。
 *
 * 纯 C99、零 HAL 依赖、无 malloc。
 */
#ifndef PROTO_BYTES_H
#define PROTO_BYTES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------- 无符号 16 位（小端） --------------------------- */

static inline void proto_put_u16_le(uint8_t *buf, uint16_t v)
{
    buf[0] = (uint8_t)(v & 0xFFu);
    buf[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t proto_get_u16_le(const uint8_t *buf)
{
    return (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
}

/* --------------------------- 有符号 16 位（小端） --------------------------- */
/* 实现方式：先按无符号收字节，再显式做补码解释。
 * 不用 (int16_t)u 直接转换，是为了让"符号扩展"这件事在代码里可见、可测。 */

static inline void proto_put_i16_le(uint8_t *buf, int16_t v)
{
    proto_put_u16_le(buf, (uint16_t)v);
}

static inline int16_t proto_get_i16_le(const uint8_t *buf)
{
    const uint16_t u = proto_get_u16_le(buf);
    if (u & 0x8000u) {
        return (int16_t)((int32_t)u - 0x10000); /* 负温等负值必须正确还原 */
    }
    return (int16_t)u;
}

/* --------------------------------- 位域 ---------------------------------- */
/* 约定：位编号 bit0 = LSB。位域只在单字节内使用，跨字节一律用整数。 */

static inline void proto_set_bit(uint8_t *byte, uint8_t bit, bool value)
{
    if (bit > 7u) {
        return; /* 越界静默忽略：位号是编译期常量，运行期不应发生 */
    }
    const uint8_t mask = (uint8_t)(1u << bit);
    *byte = value ? (uint8_t)(*byte | mask) : (uint8_t)(*byte & (uint8_t)~mask);
}

static inline bool proto_get_bit(uint8_t byte, uint8_t bit)
{
    if (bit > 7u) {
        return false;
    }
    return (byte & (uint8_t)(1u << bit)) != 0u;
}

/** 取单字节内的位段 [hi:lo]（含端点，hi >= lo）。 */
static inline uint8_t proto_get_bits8(uint8_t byte, uint8_t hi, uint8_t lo)
{
    if (hi > 7u || lo > hi) {
        return 0u;
    }
    const uint8_t width = (uint8_t)(hi - lo + 1u);
    const uint8_t mask = (uint8_t)((1u << width) - 1u);
    return (uint8_t)((byte >> lo) & mask);
}

/** 写单字节内的位段 [hi:lo]（含端点）。 */
static inline uint8_t proto_set_bits8(uint8_t byte, uint8_t hi, uint8_t lo,
                                      uint8_t value)
{
    if (hi > 7u || lo > hi) {
        return byte;
    }
    const uint8_t width = (uint8_t)(hi - lo + 1u);
    const uint8_t mask = (uint8_t)(((1u << width) - 1u) << lo);
    return (uint8_t)((byte & (uint8_t)~mask) |
                     (uint8_t)((value << lo) & mask));
}

#ifdef __cplusplus
}
#endif

#endif /* PROTO_BYTES_H */
