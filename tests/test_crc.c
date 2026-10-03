/**
 * @file  test_crc.c
 * @brief CRC 回归测试：标准校验值、流式与一次性等价、查表与逐位实现一致。
 *
 * PC 侧交叉验证：PROTO_CRC32_CHECK 必须等于 zlib.crc32(b"123456789")
 * （tools/check_crc.py 会再独立核对一次，防止"实现与期望值一起写错"）。
 */
#define UTEST_MAIN
#define UTEST_SUITE_NAME "proto_crc"

#include "utest.h"

#include "proto_crc.h"

/* 定义在 proto_crc.c：逐位参考实现，用于交叉校验查表实现 */
extern uint32_t proto_crc32_bitwise_ref(const uint8_t *data, size_t len);

static const uint8_t kCheck[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };

UTEST_CASE(ccitt_standard)
{
    /* CRC-16/CCITT-FALSE("123456789") = 0x29B1 */
    UTEST_EQ_INT(proto_crc16_ccitt(kCheck, sizeof(kCheck)), PROTO_CRC16_CCITT_CHECK);
    UTEST_EQ_INT(proto_crc16_ccitt(kCheck, sizeof(kCheck)), 0x29B1u);

    /* 空输入返回 init（不是 0）—— 这一点常被写错 */
    UTEST_EQ_INT(proto_crc16_ccitt(kCheck, 0u), PROTO_CRC16_CCITT_INIT);
    UTEST_EQ_INT(proto_crc16_ccitt(NULL, 5u), PROTO_CRC16_CCITT_INIT);

    /* 单字节变化必须改变校验值（敏感性） */
    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    const uint16_t a = proto_crc16_ccitt(buf, sizeof(buf));
    buf[7] ^= 0x01u;
    const uint16_t b = proto_crc16_ccitt(buf, sizeof(buf));
    UTEST_CHECK(a != b);
}

UTEST_CASE(crc32_standard)
{
    /* CRC-32/ISO-HDLC("123456789") = 0xCBF43926 */
    UTEST_EQ_INT(proto_crc32(kCheck, sizeof(kCheck)), PROTO_CRC32_CHECK);
    UTEST_EQ_INT(proto_crc32(kCheck, sizeof(kCheck)), 0xCBF43926u);

    /* zlib.crc32(b"") == 0 */
    UTEST_EQ_INT(proto_crc32(kCheck, 0u), 0u);
    UTEST_EQ_INT(proto_crc32(NULL, 3u), 0u);

    /* 查表实现必须与逐位参考实现逐字节一致 */
    uint8_t buf[257];
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = (uint8_t)(i * 31u + 7u);
    }
    for (size_t len = 0; len <= 64u; ++len) {
        UTEST_EQ_INT(proto_crc32(buf, len), proto_crc32_bitwise_ref(buf, len));
    }
    UTEST_EQ_INT(proto_crc32(buf, sizeof(buf)),
                 proto_crc32_bitwise_ref(buf, sizeof(buf)));
}

/** 流式累加必须与一次性计算完全一致（固件升级边收边算的前提）。 */
UTEST_CASE(streaming)
{
    uint8_t image[1024];
    for (size_t i = 0; i < sizeof(image); ++i) {
        image[i] = (uint8_t)(i ^ (i >> 3));
    }

    const uint16_t c16_once = proto_crc16_ccitt(image, sizeof(image));
    const uint32_t c32_once = proto_crc32(image, sizeof(image));

    /* 按 128 B 分块流式累加 */
    uint16_t c16 = PROTO_CRC16_CCITT_INIT;
    uint32_t c32 = PROTO_CRC32_INIT;
    for (size_t off = 0; off < sizeof(image); off += 128u) {
        c16 = proto_crc16_ccitt_update(c16, &image[off], 128u);
        c32 = proto_crc32_update(c32, &image[off], 128u);
    }
    UTEST_EQ_INT(c16, c16_once);
    UTEST_EQ_INT(c32 ^ PROTO_CRC32_XOROUT, c32_once);

    /* 任意分块切分都应得到同一结果（含 1 字节粒度） */
    uint16_t c16b = PROTO_CRC16_CCITT_INIT;
    uint32_t c32b = PROTO_CRC32_INIT;
    for (size_t i = 0; i < sizeof(image); ++i) {
        c16b = proto_crc16_ccitt_update(c16b, &image[i], 1u);
        c32b = proto_crc32_update(c32b, &image[i], 1u);
    }
    UTEST_EQ_INT(c16b, c16_once);
    UTEST_EQ_INT(c32b ^ PROTO_CRC32_XOROUT, c32_once);

    /* 零长度分块不改变结果 */
    UTEST_EQ_INT(proto_crc16_ccitt_update(c16, NULL, 0u), c16);
    UTEST_EQ_INT(proto_crc32_update(c32, NULL, 0u), c32);
}

/**
 * 块级 + 整包的分工验证：
 * 单块损坏 → 块级 CRC 能定位；整包 CRC 也必然变化。
 * 这一步验证"升级链路两套校验都必要"的判断。
 */
UTEST_CASE(edge_cases)
{
    uint8_t image[512];
    for (size_t i = 0; i < sizeof(image); ++i) {
        image[i] = (uint8_t)i;
    }
    const uint32_t whole = proto_crc32(image, sizeof(image));

    /* 在第 2 块内翻转 1 bit */
    uint8_t corrupted[512];
    memcpy(corrupted, image, sizeof(image));
    corrupted[256 + 5] ^= 0x08u;

    const uint32_t block0_before = proto_crc32(&image[0], 256u);
    const uint32_t block0_after = proto_crc32(&corrupted[0], 256u);
    const uint32_t block1_before = proto_crc32(&image[256], 256u);
    const uint32_t block1_after = proto_crc32(&corrupted[256], 256u);

    UTEST_CHECK(block0_before == block0_after);       /* 未损坏块不受影响 */
    UTEST_CHECK(block1_before != block1_after);       /* 损坏块被检出 → 只重传该块 */
    UTEST_CHECK(whole != proto_crc32(corrupted, sizeof(corrupted))); /* 整包也被检出 */
}
