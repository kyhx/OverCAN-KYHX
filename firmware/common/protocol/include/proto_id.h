/**
 * @file    proto_id.h
 * @brief   can_distributed 协议 v1.0 —— 帧 ID 编码与帧定义表【单一定义源】
 *
 * 本文件是协议的**唯一机器可读定义源**：
 *   - can/can_distributed.dbc            由 PROTO_FRAME_TABLE 生成（tools/gen_dbc.py）
 *   - tests/test_dbc_drift.c             每次构建自动比对 DBC 与本表是否脱节
 *   - docs/protocol.md                   人类可读规范（§3 与本表一一对应）
 *
 * 约束：纯 C99、零 HAL 依赖、无 malloc、无浮点、不使用 C 位域。
 */
#ifndef PROTO_ID_H
#define PROTO_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* 编译期断言：MSVC 与 GCC/Clang 都能用                                        */
/* ------------------------------------------------------------------------- */
#if defined(__cplusplus) && (__cplusplus >= 201103L)
#define PROTO_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define PROTO_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
/* C99 回退路径（本项目 CMAKE_C_STANDARD=99，实际走的就是这条）。
 *
 * ⚠️ 这里必须是**双重宏展开**，不是一层：
 *   单层 `typedef char x_##__LINE__[...]` —— `##` 会阻止 `__LINE__` 展开，
 *   所有断言共用同一个 typedef 名 → GCC 报 "redefinition of typedef"，
 *   行为上是未定义（不同编译器表现不一）。
 *   两层（SA_CAT 先展开实参，再交给 SA_CAT_ 做 ## 拼接）才能得到
 *   `x_113` / `x_124` 这样的唯一名字。
 *
 * 同样不能改用 `_Static_assert`：它在 C99 下虽被GCC/Clang 支持为扩展，
 * 但 `-Wpedantic` 会报 "ISO C99 does not support"，而本项目把
 * -Wpedantic/-Werror 视为必须通过的门槛。
 *
 * 这个问题是用 arm-none-eabi-gcc 做 Cortex-M3 交叉编译验证时暴露的：
 * PC 侧 MSVC 走C11 分支，**永远不会触发** —— 典型的"换编译器才炸"。
 */
#define PROTO_STATIC_ASSERT_CAT_(a, b) a##b
#define PROTO_STATIC_ASSERT_CAT(a, b)  PROTO_STATIC_ASSERT_CAT_(a, b)
#define PROTO_STATIC_ASSERT(cond, msg) \
    typedef char PROTO_STATIC_ASSERT_CAT(proto_static_assert_, __LINE__)[(cond) ? 1 : -1]
#endif

/* ------------------------------------------------------------------------- */
/* ID 编码：category(3) | node(4) | type(4)                                  */
/* ------------------------------------------------------------------------- */

/** 类别：占据 ID[10:8]，决定仲裁优先级（ID 越小越优先）。 */
typedef enum {
    PROTO_CAT_EVENT = 0,           /**< 事件：最高优先级，报警须抢占一切        */
    PROTO_CAT_COMMAND = 1,         /**< 命令：控制指令                          */
    PROTO_CAT_TELEMETRY = 2,       /**< 遥测：1 Hz 周期数据                     */
    PROTO_CAT_HEARTBEAT_ACK = 3,   /**< 心跳 / ACK：可等待，最低优先级          */
    PROTO_CAT_COUNT = 4
} proto_category_t;

/** 帧类型：占据 ID[3:0]。统一编号，语义全类别一致，防止"type 字段语义漂移"。 */
typedef enum {
    PROTO_TYPE_NORMAL = 0, /**< 常规帧（命令 / 遥测 / 心跳 / ACK） */
    PROTO_TYPE_EVENT = 1,  /**< 事件帧（仅事件类别）               */
    PROTO_TYPE_RESERVED_FIRST = 2, /**< 2~15 保留，禁止复用已废弃值 */
    PROTO_TYPE_MAX = 15
} proto_frame_type_t;

/** 节点号：占据 ID[7:4]。0 = 主站 / 广播，1~15 = 从节点。 */
typedef enum {
    PROTO_NODE_MASTER = 0,   /**< 主站 / 广播地址 */
    PROTO_NODE_1 = 1,
    PROTO_NODE_2 = 2,
    PROTO_NODE_MIN = 1,      /**< 从节点号下界 */
    PROTO_NODE_MAX = 15      /**< 协议上限 15 个从节点 */
} proto_node_t;

/** 按编码规则组装 11 位 ID。参数越界时返回 0（0 不是任何合法帧 ID）。 */
static inline uint16_t proto_make_id(proto_category_t cat, uint8_t node,
                                     proto_frame_type_t type)
{
    if ((unsigned)cat >= (unsigned)PROTO_CAT_COUNT ||
        node > (unsigned)PROTO_NODE_MAX ||
        (unsigned)type > (unsigned)PROTO_TYPE_MAX) {
        return 0u;
    }
    /* 手写移位掩码，不使用位域 */
    return (uint16_t)((((uint16_t)cat & 0x7u) << 8) |
                      (((uint16_t)node & 0xFu) << 4) |
                      ((uint16_t)type & 0xFu));
}

static inline proto_category_t proto_id_category(uint16_t id)
{
    return (proto_category_t)((id >> 8) & 0x7u);
}

static inline uint8_t proto_id_node(uint16_t id)
{
    return (uint8_t)((id >> 4) & 0xFu);
}

static inline proto_frame_type_t proto_id_type(uint16_t id)
{
    return (proto_frame_type_t)(id & 0xFu);
}

/* ------------------------------------------------------------------------- */
/* 10 个帧 ID（全部已定义，与 docs/protocol.md §3.2 一一对应）                  */
/* ------------------------------------------------------------------------- */
enum {
    PROTO_ID_NODE1_EVENT        = 0x011, /**< 节点一 → 主站  事件            */
    PROTO_ID_NODE2_EVENT        = 0x021, /**< 节点二 → 主站  事件            */
    PROTO_ID_CMD_BROADCAST      = 0x100, /**< 主站 → 全网    广播命令        */
    PROTO_ID_CMD_NODE1          = 0x110, /**< 主站 → 节点一  单播命令        */
    PROTO_ID_CMD_NODE2          = 0x120, /**< 主站 → 节点二  单播命令        */
    PROTO_ID_TELEMETRY_NODE1    = 0x210, /**< 节点一 → 主站  遥测            */
    PROTO_ID_TELEMETRY_NODE2    = 0x220, /**< 节点二 → 主站  遥测            */
    PROTO_ID_MASTER_HEARTBEAT   = 0x300, /**< 主站 → 全网    心跳            */
    PROTO_ID_ACK_NODE1          = 0x310, /**< 节点一 → 主站  ACK             */
    PROTO_ID_ACK_NODE2          = 0x320  /**< 节点二 → 主站  ACK             */
};

/* 自洽性：编码规则必须能还原出上面每个常量 */
PROTO_STATIC_ASSERT(PROTO_ID_NODE1_EVENT == 0x011, "id rule");
PROTO_STATIC_ASSERT(PROTO_ID_NODE2_EVENT == 0x021, "id rule");
PROTO_STATIC_ASSERT(PROTO_ID_CMD_BROADCAST == 0x100, "id rule");
PROTO_STATIC_ASSERT(PROTO_ID_CMD_NODE1 == 0x110, "id rule");
PROTO_STATIC_ASSERT(PROTO_ID_CMD_NODE2 == 0x120, "id rule");
PROTO_STATIC_ASSERT(PROTO_ID_TELEMETRY_NODE1 == 0x210, "id rule");
PROTO_STATIC_ASSERT(PROTO_ID_TELEMETRY_NODE2 == 0x220, "id rule");
PROTO_STATIC_ASSERT(PROTO_ID_MASTER_HEARTBEAT == 0x300, "id rule");
PROTO_STATIC_ASSERT(PROTO_ID_ACK_NODE1 == 0x310, "id rule");
PROTO_STATIC_ASSERT(PROTO_ID_ACK_NODE2 == 0x320, "id rule");

/* 数值升序必须等于紧急度降序（这是协议的核心设计主张，用编译期断言焊死） */
PROTO_STATIC_ASSERT(PROTO_ID_NODE1_EVENT < PROTO_ID_NODE2_EVENT, "priority order");
PROTO_STATIC_ASSERT(PROTO_ID_NODE2_EVENT < PROTO_ID_CMD_BROADCAST, "priority order");
PROTO_STATIC_ASSERT(PROTO_ID_CMD_BROADCAST < PROTO_ID_CMD_NODE1, "broadcast first");
PROTO_STATIC_ASSERT(PROTO_ID_CMD_NODE1 < PROTO_ID_CMD_NODE2, "priority order");
PROTO_STATIC_ASSERT(PROTO_ID_CMD_NODE2 < PROTO_ID_TELEMETRY_NODE1, "priority order");
PROTO_STATIC_ASSERT(PROTO_ID_TELEMETRY_NODE1 < PROTO_ID_TELEMETRY_NODE2, "priority order");
PROTO_STATIC_ASSERT(PROTO_ID_TELEMETRY_NODE2 < PROTO_ID_MASTER_HEARTBEAT, "priority order");
PROTO_STATIC_ASSERT(PROTO_ID_MASTER_HEARTBEAT < PROTO_ID_ACK_NODE1, "priority order");
PROTO_STATIC_ASSERT(PROTO_ID_ACK_NODE1 < PROTO_ID_ACK_NODE2, "priority order");

/**
 * 校验 ID 是否符合"类别 / 帧型 / 节点号"的语义规则：
 *   1. 11 位内（id <= 0x7FF）且非 0
 *   2. 类别合法，且能被 proto_make_id 无损还原
 *   3. 帧型：事件类别必须是 PROTO_TYPE_EVENT；其它类别必须是 PROTO_TYPE_NORMAL
 *   4. 来源：
 *      - 事件 / 遥测 / ACK 必定来自**从节点**（node >= 1）
 *      - 命令的 node = 0 表示广播，node >= 1 表示单播
 *      - 心跳必定来自**主站**（node = 0）
 *
 * ⚠️ 心跳与 ACK 共用 HEARTBEAT_ACK 类别，因此**不能**把"必须来自从节点"
 * 这条规则套在整个类别上 —— 那样会把主站自己的心跳 0x300 判成非法
 * （本仓库首版即踩此坑，被 tests/test_codec.c 的 id_rules 用例抓出）。
 */
static inline bool proto_id_is_valid(uint16_t id)
{
    if (id == 0u || id > 0x7FFu) {
        return false;
    }
    const proto_category_t cat = proto_id_category(id);
    const proto_frame_type_t type = proto_id_type(id);
    const uint8_t node = proto_id_node(id);

    if ((unsigned)cat >= (unsigned)PROTO_CAT_COUNT) {
        return false;
    }
    if ((unsigned)proto_make_id(cat, node, type) != (unsigned)id) {
        return false;
    }
    if (cat == PROTO_CAT_EVENT) {
        if (type != PROTO_TYPE_EVENT) {
            return false;
        }
        /* 注意：node 是 uint8_t，比较时不能写成 node < PROTO_NODE_MIN ——
         * 那会把两边提升为 int，而 node=0（广播）会因此被误判为"小于下界"。
         * 必须显式转成无符号再比较。 */
        if ((unsigned)node < (unsigned)PROTO_NODE_MIN) {
            return false; /* 事件必须来自某个从节点 */
        }
        return true;
    }
    if (type != PROTO_TYPE_NORMAL) {
        return false;
    }

    switch (cat) {
    case PROTO_CAT_TELEMETRY:
        /* 遥测只能来自从节点 */
        return node != (uint8_t)PROTO_NODE_MASTER;
    case PROTO_CAT_HEARTBEAT_ACK:
        /* 心跳（0x300）来自主站；ACK（0x310/0x320）来自从节点 */
        if (id == PROTO_ID_MASTER_HEARTBEAT) {
            return node == (uint8_t)PROTO_NODE_MASTER;
        }
        return node != (uint8_t)PROTO_NODE_MASTER;
    case PROTO_CAT_COMMAND:
    default:
        /* 命令：node=0 为广播，1~15 为单播；两种都合法 */
        return true;
    }
}

/** 类别 → 人可读名（用于日志；协议库不依赖 printf）。 */
static inline const char *proto_category_name(proto_category_t cat)
{
    switch (cat) {
    case PROTO_CAT_EVENT: return "EVENT";
    case PROTO_CAT_COMMAND: return "COMMAND";
    case PROTO_CAT_TELEMETRY: return "TELEMETRY";
    case PROTO_CAT_HEARTBEAT_ACK: return "HEARTBEAT_ACK";
    default: return "INVALID";
    }
}

/* ------------------------------------------------------------------------- */
/* 帧定义表（机器可读）—— DBC 生成器与漂移测试的输入                            */
/* ------------------------------------------------------------------------- */

/** 数据场长度：全部 8 字节。 */
#define PROTO_DLC 8u

/** 方向标注（仅用于文档与 DBC 注释，不影响编码）。 */
typedef enum {
    PROTO_DIR_NODE_TO_MASTER = 0, /**< 从节点 → 主站 */
    PROTO_DIR_MASTER_TO_ALL = 1,  /**< 主站 → 全网（广播） */
    PROTO_DIR_MASTER_TO_NODE = 2  /**< 主站 → 单节点 */
} proto_direction_t;

typedef struct {
    uint16_t id;             /**< 11 位帧 ID                          */
    const char *name;        /**< DBC 消息名（大写字母/数字/下划线）   */
    proto_direction_t dir;   /**< 方向                                */
    uint8_t dlc;             /**< 数据场长度（恒 8）                   */
    uint16_t cycle_ms;       /**< 周期（ms）；0 = 事件/非周期驱动       */
    const char *comment;     /**< 用途说明                            */
} proto_frame_info_t;

/* PROTO_FRAME_TABLE_BEGIN/END 是被 tools/gen_dbc.py 与 tests/test_dbc_drift.c
 * 用作解析边界的两行标记注释 —— 不要删除或改写这两行的文字。
 *
 * 表项写法（与下方各行一致，仅用文字描述，以免解析器误认）：
 *   一对大括号，依次是：16 进制帧 ID + u、带引号的大写名称、
 *   PROTO_DIR_* 方向、PROTO_DLC、周期毫秒数 + u、带引号的用途说明，
 *   行尾逗号。
 * 修改本表后必须重新生成 DBC：python tools/gen_dbc.py --write
 * 这里只放协议库需要的元数据；每个字节的字段级定义在 proto_codec.h 与
 * tools/gen_dbc.py 的 SIGNALS 表中维护，并由 docs/protocol.md §5 描述。 */
/* PROTO_FRAME_TABLE_BEGIN */
static const proto_frame_info_t PROTO_FRAME_TABLE[] = {
    { 0x011u, "NODE1_EVENT",      PROTO_DIR_NODE_TO_MASTER, PROTO_DLC,    0u, "node1 event: over-temp / stall / fault" },
    { 0x021u, "NODE2_EVENT",      PROTO_DIR_NODE_TO_MASTER, PROTO_DLC,    0u, "node2 event: IR trigger / light out of range" },
    { 0x100u, "CMD_BROADCAST",    PROTO_DIR_MASTER_TO_ALL,  PROTO_DLC,    0u, "broadcast command (each node ACKs separately)" },
    { 0x110u, "CMD_NODE1",        PROTO_DIR_MASTER_TO_NODE, PROTO_DLC,    0u, "unicast command to node1" },
    { 0x120u, "CMD_NODE2",        PROTO_DIR_MASTER_TO_NODE, PROTO_DLC,    0u, "unicast command to node2" },
    { 0x210u, "TELEMETRY_NODE1",  PROTO_DIR_NODE_TO_MASTER, PROTO_DLC, 1000u, "node1 telemetry: temperature + motor state" },
    { 0x220u, "TELEMETRY_NODE2",  PROTO_DIR_NODE_TO_MASTER, PROTO_DLC, 1000u, "node2 telemetry: light + IR + servo" },
    { 0x300u, "MASTER_HEARTBEAT", PROTO_DIR_MASTER_TO_ALL,  PROTO_DLC, 1000u, "master heartbeat: online bitmap + protocol version" },
    { 0x310u, "ACK_NODE1",        PROTO_DIR_NODE_TO_MASTER, PROTO_DLC,    0u, "node1 ACK: echoed command seq + result" },
    { 0x320u, "ACK_NODE2",        PROTO_DIR_NODE_TO_MASTER, PROTO_DLC,    0u, "node2 ACK: echoed command seq + result" }
};
/* PROTO_FRAME_TABLE_END */

#define PROTO_FRAME_COUNT (sizeof(PROTO_FRAME_TABLE) / sizeof(PROTO_FRAME_TABLE[0]))
PROTO_STATIC_ASSERT(sizeof(PROTO_FRAME_TABLE) / sizeof(PROTO_FRAME_TABLE[0]) == 10u,
                    "protocol defines exactly 10 frames");

/** 协议版本（心跳 b5 上报；与固件版本完全分离，见 docs/protocol.md §10）。 */
#define PROTO_PROTOCOL_VERSION 1u

/** 按 ID 查表；未定义返回 NULL。 */
const proto_frame_info_t *proto_frame_by_id(uint16_t id);

/** 按 DBC 名查表；未定义返回 NULL。 */
const proto_frame_info_t *proto_frame_by_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_ID_H */
