/**
 * @file  proto_crc.c
 * @brief CRC-16/CCITT-FALSE 与 CRC-32/ISO-HDLC 实现。
 *
 * CRC-16 用逐位实现（Flash 占用小，块级校验的数据量不大）；
 * CRC-32 用运行期惰性生成的查表（整包 64 KB 逐位算太慢，约 0.5 MB/s 量级）。
 */
#include "proto_crc.h"

/* ------------------------------------------------------------------------- */
/* CRC-16/CCITT-FALSE：poly 0x1021 / init 0xFFFF / 不反射 / 无异或              */
/* ------------------------------------------------------------------------- */

uint16_t proto_crc16_ccitt_update(uint16_t seed, const uint8_t *data, size_t len)
{
    uint16_t crc = seed;
    if (data == NULL) {
        return crc;
    }
    for (size_t i = 0; i < len; ++i) {
        crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)data[i] << 8));
        for (unsigned bit = 0; bit < 8u; ++bit) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ PROTO_CRC16_CCITT_POLY);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint16_t proto_crc16_ccitt(const uint8_t *data, size_t len)
{
    return proto_crc16_ccitt_update(PROTO_CRC16_CCITT_INIT, data, len);
}

/* ------------------------------------------------------------------------- */
/* CRC-32/ISO-HDLC：poly 0x04C11DB7 / init 0xFFFFFFFF / 反射 / 输出异或          */
/* ------------------------------------------------------------------------- */

/** 512 项半字节表：Flash 占用小（1 KB），速度约为逐位实现的 4 倍。 */
static uint32_t s_crc32_nibble[16];
static uint8_t s_crc32_table_ready = 0u;

/**
 * 反射 CRC-32（poly 反射形式 0xEDB88320）。
 *
 * 注意：ISO-HDLC / zlib 的 CRC-32 是**反射**算法，实现上必须使用反射多项式
 * 0xEDB88320 并配合右移。若误用 0x04C11DB7 右移，结果会与标准值不符
 * （这正是本仓库首次实现时踩到的坑，由 "123456789" 的校验值抓出来）。
 */
#define PROTO_CRC32_POLY_REFLECTED 0xEDB88320u

static uint32_t crc32_bitwise(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)data[i];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (PROTO_CRC32_POLY_REFLECTED & mask);
        }
    }
    return crc;
}

static void crc32_build_table(void)
{
    for (uint32_t i = 0; i < 16u; ++i) {
        uint32_t crc = i;
        for (unsigned bit = 0; bit < 4u; ++bit) {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (PROTO_CRC32_POLY_REFLECTED & mask);
        }
        s_crc32_nibble[i] = crc;
    }
    s_crc32_table_ready = 1u;
}

uint32_t proto_crc32_update(uint32_t seed, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0u) {
        return seed;
    }
    if (!s_crc32_table_ready) {
        crc32_build_table();
    }
    uint32_t crc = seed;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        /* 反射算法：先处理低半字节，再处理高半字节（等价于右移 8 位） */
        crc = (crc >> 4) ^ s_crc32_nibble[(crc ^ (uint32_t)b) & 0x0Fu];
        crc = (crc >> 4) ^ s_crc32_nibble[(crc ^ ((uint32_t)b >> 4)) & 0x0Fu];
    }
    return crc;
}

uint32_t proto_crc32(const uint8_t *data, size_t len)
{
    return proto_crc32_update(PROTO_CRC32_INIT, data, len) ^ PROTO_CRC32_XOROUT;
}

/* 供单元测试对比"逐位实现"与"查表实现"是否一致（防止表生成写错）。 */
uint32_t proto_crc32_bitwise_ref(const uint8_t *data, size_t len)
{
    return crc32_bitwise(PROTO_CRC32_INIT, data, len) ^ PROTO_CRC32_XOROUT;
}
