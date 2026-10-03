/**
 * @file  utest.h
 * @brief 极简单元测试框架（零外部依赖，可在 PC 上直接跑）。
 *
 * 为什么自己写而不是引入 Unity/CMocka：协议库的价值之一是"无依赖、可移植"，
 * 测试框架若需要联网下载，CI 与同事的机器就跑不起来。这里有 60 行就够了。
 *
 * 设计：**自注册**。每个测试用例把自己的运行函数登记进一个链接器段，
 * 因此新增用例只需写 `UTEST_CASE(name){...}`，无需维护任何列表——
 * 忘记登记就是用例静默不跑，这是这类框架最经典的坑。
 *
 * 用法：
 *     UTEST_CASE(telemetry_roundtrip) { UTEST_CHECK(expr); UTEST_EQ_INT(a, b); }
 * 一个文件里使用 `#define UTEST_MAIN` 后即可自动生成 main()。
 */
#ifndef UTEST_H
#define UTEST_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* 自注册机制：MSVC 用 .CRT$XCU，GCC/Clang 用 constructor 属性                  */
/* ------------------------------------------------------------------------- */
typedef void (*utest_runner_fn)(void);

typedef struct {
    const char *name;
    utest_runner_fn run;
} utest_entry_t;

static utest_entry_t g_utest_entries[256];
static int g_utest_entry_count = 0;
static int g_utest_checks = 0;
static int g_utest_failures = 0;
static int g_utest_case_failed = 0;

static void utest_register(const char *name, utest_runner_fn run)
{
    if (g_utest_entry_count < (int)(sizeof(g_utest_entries) /
                                    sizeof(g_utest_entries[0]))) {
        g_utest_entries[g_utest_entry_count].name = name;
        g_utest_entries[g_utest_entry_count].run = run;
        ++g_utest_entry_count;
    }
}

#if defined(_MSC_VER)
#define UTEST_REGISTER(name, run)                                            \
    static void utest_reg_##name(void) { utest_register(#name, run); }       \
    __pragma(section(".CRT$XCU", read))                                      \
    __declspec(allocate(".CRT$XCU")) static void (*utest_reg_ptr_##name)(void) = utest_reg_##name;
#elif defined(__GNUC__) || defined(__clang__)
#define UTEST_REGISTER(name, run)                                            \
    __attribute__((constructor)) static void utest_reg_##name(void)          \
    {                                                                        \
        utest_register(#name, run);                                          \
    }
#else
#error "utest: unsupported compiler; add a self-registration mechanism"
#endif

/* ------------------------------------------------------------------------- */
/* 用例定义                                                                  */
/* ------------------------------------------------------------------------- */

/* 展开为：运行函数（自动登记）+ 用例函数本体。
 * 运行函数中的 utest_case_##name 调用位于用例函数定义**之后**，因此不需要
 * 前置声明即无隐式声明警告。 */
#define UTEST_CASE(name)                                                     \
    static void utest_case_##name(void);                                     \
    static void utest_run_##name(void)                                       \
    {                                                                        \
        g_utest_case_failed = 0;                                             \
        utest_case_##name();                                                 \
        if (!g_utest_case_failed) {                                          \
            printf("  [ ok ] %s\n", #name);                                  \
        }                                                                    \
    }                                                                        \
    UTEST_REGISTER(name, utest_run_##name)                                   \
    static void utest_case_##name(void)

#define UTEST_CHECK(expr)                                                  \
    do {                                                                   \
        ++g_utest_checks;                                                  \
        if (!(expr)) {                                                     \
            ++g_utest_failures;                                            \
            g_utest_case_failed = 1;                                       \
            printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #expr);      \
        }                                                                  \
    } while (0)

#define UTEST_EQ_INT(actual, expected)                                       \
    do {                                                                     \
        ++g_utest_checks;                                                    \
        const long long utest_a = (long long)(actual);                       \
        const long long utest_e = (long long)(expected);                     \
        if (utest_a != utest_e) {                                            \
            ++g_utest_failures;                                              \
            g_utest_case_failed = 1;                                         \
            printf("  [FAIL] %s:%d  %s == %s (got %lld, want %lld)\n",       \
                   __FILE__, __LINE__, #actual, #expected, utest_a,          \
                   utest_e);                                                 \
        }                                                                    \
    } while (0)

/** 逐字节比对数据场（用于"黄金报文"回归测试）。 */
#define UTEST_EQ_BYTES(actual, expected, len)                                \
    do {                                                                     \
        ++g_utest_checks;                                                    \
        if (memcmp((actual), (expected), (len)) != 0) {                      \
            ++g_utest_failures;                                              \
            g_utest_case_failed = 1;                                         \
            printf("  [FAIL] %s:%d  bytes differ\n      got :", __FILE__,    \
                   __LINE__);                                                \
            for (size_t utest_i = 0; utest_i < (size_t)(len); ++utest_i) {   \
                printf(" %02X", ((const uint8_t *)(actual))[utest_i]);       \
            }                                                                \
            printf("\n      want:");                                         \
            for (size_t utest_i = 0; utest_i < (size_t)(len); ++utest_i) {   \
                printf(" %02X", ((const uint8_t *)(expected))[utest_i]);     \
            }                                                                \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------------- */
/* 测试主程序（每个测试文件恰好定义一次 UTEST_MAIN 与 UTEST_SUITE_NAME）        */
/* ------------------------------------------------------------------------- */
#ifdef UTEST_MAIN
#ifndef UTEST_SUITE_NAME
#define UTEST_SUITE_NAME "tests"
#endif

int main(void)
{
    printf("== %s ==\n", UTEST_SUITE_NAME);
    for (int i = 0; i < g_utest_entry_count; ++i) {
        g_utest_entries[i].run();
    }
    printf("-- %s: %d cases, %d checks, %d failures --\n", UTEST_SUITE_NAME,
           g_utest_entry_count, g_utest_checks, g_utest_failures);
    if (g_utest_entry_count == 0) {
        printf("ERROR: no test case was registered (framework bug)\n");
        return 1;
    }
    return (g_utest_failures == 0) ? 0 : 1;
}
#endif

#endif /* UTEST_H */
