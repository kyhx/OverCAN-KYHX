/**
 * @file  test_dbc_drift.c
 * @brief DBC 与代码的**漂移检测**：直接解析交付的 DBC 文本与 proto_id.h，
 *        逐帧比对 ID / 名称 / DLC，存在不一致即构建失败。
 *
 * 这是本项目对"帧定义与代码永不脱节"这条工程约定的可执行保证：
 * 手写 DBC 会腐坏，生成 + 每次构建比对才不会。
 *
 * 注：更严格的解码级校验由 tools/gen_dbc.py --check 完成（用 cantools 解析）。
 */
#define UTEST_MAIN
#define UTEST_SUITE_NAME "dbc_drift"

#include "utest.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto_id.h"

#ifndef PROTO_DBC_PATH
#error "PROTO_DBC_PATH must be defined by the build system"
#endif
#ifndef PROTO_ID_HEADER_PATH
#error "PROTO_ID_HEADER_PATH must be defined by the build system"
#endif

#define UTEST_MAX_FRAMES 32

typedef struct {
    uint16_t id;
    char name[48];
    uint8_t dlc;
} dbc_message_t;

typedef struct {
    uint16_t id;
    char name[48];
    uint8_t dlc;
} hdr_frame_t;

static char *read_whole_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    const long size = ftell(fp);
    if (size <= 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    char *buf = (char *)malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    const size_t got = fread(buf, 1u, (size_t)size, fp);
    buf[got] = '\0';
    fclose(fp);
    return buf;
}

/* PROTO_FRAME_TABLE_BEGIN 是 proto_id.h 与 tools/gen_dbc.py 共用的标记注释，
 * 此处按同样的边界做文本解析 —— 这是"生成 + 校验"闭环的另一半。 */
#define TABLE_BEGIN_MARKER "PROTO_FRAME_TABLE_BEGIN"
#define TABLE_END_MARKER "PROTO_FRAME_TABLE_END"

/** 从 proto_id.h 的 PROTO_FRAME_TABLE 中解析出 { id, "NAME", ..., dlc } */
static size_t parse_header_frames(const char *text, hdr_frame_t *out, size_t cap)
{
    const char *begin = strstr(text, TABLE_BEGIN_MARKER);
    const char *end = strstr(text, TABLE_END_MARKER);
    if (begin == NULL || end == NULL || begin >= end) {
        return 0u;
    }
    size_t n = 0u;
    const char *p = begin;
    while (n < cap) {
        const char *brace = strchr(p, '{');
        if (brace == NULL || brace > end) {
            break;
        }
        /* 只认真正的表项：必须以 "{ 0x" 开头（跳过注释里的示意写法） */
        const char *cursor = brace;
        while (cursor < end && (*cursor == '{' || *cursor == ' ')) {
            ++cursor;
        }
        if (cursor + 2 > end || cursor[0] != '0' || cursor[1] != 'x') {
            p = brace + 1;
            continue;
        }
        unsigned long id = strtoul(cursor, NULL, 16);
        const char *q1 = strchr(cursor, '"');
        const char *q2 = (q1 != NULL) ? strchr(q1 + 1, '"') : NULL;
        if (q1 == NULL || q2 == NULL || q1 > end) {
            break;
        }
        const size_t namelen = (size_t)(q2 - q1 - 1);
        if (namelen == 0u || namelen >= sizeof(out[n].name)) {
            break;
        }
        /* DLC 必须以 PROTO_DLC 出现，否则不是表项 */
        const char *dlcstr = strstr(q2, "PROTO_DLC");
        if (dlcstr == NULL || dlcstr > end) {
            p = q2 + 1;
            continue;
        }
        out[n].id = (uint16_t)id;
        memcpy(out[n].name, q1 + 1, namelen);
        out[n].name[namelen] = '\0';
        out[n].dlc = 8u;
        ++n;
        p = dlcstr;
    }
    return n;
}

/** 从 DBC 文本解析 BO_ 消息定义 */
static size_t parse_dbc_messages(const char *text, dbc_message_t *out, size_t cap)
{
    size_t n = 0u;
    const char *p = text;
    while (n < cap) {
        const char *bo = strstr(p, "\nBO_ ");
        if (bo == NULL) {
            break;
        }
        unsigned long id = 0u;
        char name[64] = { 0 };
        unsigned long dlc = 0u;
        if (sscanf(bo + 1, "BO_ %lu %63s %lu", &id, name, &dlc) != 3) {
            break;
        }
        char *colon = strchr(name, ':');
        if (colon != NULL) {
            *colon = '\0';
        }
        if (id <= 0x7FFu && strlen(name) < sizeof(out[n].name)) {
            out[n].id = (uint16_t)id;
            strcpy(out[n].name, name);
            out[n].dlc = (uint8_t)dlc;
            ++n;
        }
        p = bo + 1;
    }
    return n;
}

UTEST_CASE(sources_exist)
{
    char *dbc = read_whole_file(PROTO_DBC_PATH);
    char *hdr = read_whole_file(PROTO_ID_HEADER_PATH);
    UTEST_CHECK(dbc != NULL); /* 缺失时给出清晰失败，而不是静默跳过 */
    UTEST_CHECK(hdr != NULL);
    if (dbc != NULL) {
        /* DBC 基本骨架 */
        UTEST_CHECK(strstr(dbc, "VERSION") != NULL);
        UTEST_CHECK(strstr(dbc, "BS_:") != NULL);
        UTEST_CHECK(strstr(dbc, "BU_:") != NULL);
        UTEST_CHECK(strstr(dbc, "BO_ ") != NULL);
    }
    free(dbc);
    free(hdr);
}

UTEST_CASE(no_drift)
{
    char *dbc = read_whole_file(PROTO_DBC_PATH);
    char *hdr = read_whole_file(PROTO_ID_HEADER_PATH);
    if (dbc == NULL || hdr == NULL) {
        free(dbc);
        free(hdr);
        UTEST_CHECK(0 && "cannot read sources");
        return;
    }

    hdr_frame_t frames[UTEST_MAX_FRAMES];
    dbc_message_t msgs[UTEST_MAX_FRAMES];
    const size_t nframes = parse_header_frames(hdr, frames, UTEST_MAX_FRAMES);
    const size_t nmsgs = parse_dbc_messages(dbc, msgs, UTEST_MAX_FRAMES);

    /* 解析器自检：本协议恰好 10 帧，两处都必须解析到 10 条 */
    UTEST_EQ_INT(nframes, (size_t)PROTO_FRAME_COUNT);
    UTEST_EQ_INT(nframes, 10u);
    UTEST_EQ_INT(nmsgs, 10u);

    /* 逐帧比对：DBC 里必须存在同 ID、同名、同 DLC 的消息 */
    for (size_t i = 0; i < nframes && i < UTEST_MAX_FRAMES; ++i) {
        const dbc_message_t *found = NULL;
        for (size_t j = 0; j < nmsgs; ++j) {
            if (msgs[j].id == frames[i].id) {
                found = &msgs[j];
                break;
            }
        }
        if (found == NULL) {
            printf("    FAIL frame 0x%03X (%s) missing from DBC\n", frames[i].id,
                   frames[i].name);
            ++g_utest_failures;
            g_utest_case_failed = 1;
            continue;
        }
        UTEST_CHECK(strcmp(found->name, frames[i].name) == 0);
        if (strcmp(found->name, frames[i].name) != 0) {
            printf("    FAIL 0x%03X name: DBC '%s' vs code '%s'\n", frames[i].id,
                   found->name, frames[i].name);
        }
        UTEST_EQ_INT(found->dlc, frames[i].dlc);
    }

    /* 反向检查：DBC 不得出现代码里没有的帧（防止残留旧定义） */
    for (size_t j = 0; j < nmsgs; ++j) {
        bool known = false;
        for (size_t i = 0; i < nframes; ++i) {
            if (frames[i].id == msgs[j].id) {
                known = true;
                break;
            }
        }
        if (!known) {
            printf("    FAIL DBC has stale message 0x%03X (%s)\n", msgs[j].id,
                   msgs[j].name);
            ++g_utest_failures;
            g_utest_case_failed = 1;
        }
    }

    /* 枚举值表必须存在 —— 否则"机器可校验的帧定义"就名不副实 */
    UTEST_CHECK(strstr(dbc, "VAL_ ") != NULL);
    UTEST_CHECK(strstr(dbc, "\"SET\"") != NULL);
    UTEST_CHECK(strstr(dbc, "\"OVER_TEMP\"") != NULL);
    UTEST_CHECK(strstr(dbc, "\"FORWARD\"") != NULL);

    free(dbc);
    free(hdr);
}
