/**
 * @file  proto_id.c
 * @brief 帧定义表查询实现。表本身在 proto_id.h 中（单一定义源）。
 */
#include "proto_id.h"

#include <string.h>

const proto_frame_info_t *proto_frame_by_id(uint16_t id)
{
    for (size_t i = 0; i < PROTO_FRAME_COUNT; ++i) {
        if (PROTO_FRAME_TABLE[i].id == id) {
            return &PROTO_FRAME_TABLE[i];
        }
    }
    return NULL; /* 未定义的 ID —— 调用方必须丢弃该帧，而不是猜测长度 */
}

const proto_frame_info_t *proto_frame_by_name(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < PROTO_FRAME_COUNT; ++i) {
        if (strcmp(PROTO_FRAME_TABLE[i].name, name) == 0) {
            return &PROTO_FRAME_TABLE[i];
        }
    }
    return NULL;
}
