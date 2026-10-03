#!/usr/bin/env bash
# =============================================================================
# 跨平台编译验证：PC(MSVC/GCC) + Cortex-M3(arm-none-eabi-gcc)
#
# 为什么需要这个脚本
# ------------------
# PC 侧测试（ctest）只能证明"逻辑在 x86 上对"。而嵌入式有一类缺陷是
# **换编译器才炸**的，本项目已经踩到两次：
#
#   1. PROTO_STATIC_ASSERT 的单层宏拼接：PC 侧 MSVC 走 C11 分支，
#      永远不触发；只有用 GCC + C99 + -Wpedantic 才会暴露
#      "redefinition of typedef"。
#   2. 隐式窄转换、-Wpedantic 下的非标准扩展：MSVC /W4 与 GCC 的
#      告警集合并不相同，某些问题只有 GCC 能看见。
#
# 所以"能在 PC 上跑通"不等于"能烧进板子"。本脚本把交叉编译纳入
# 常规检查，代价是几秒时间。
#
# 用法
# ----
#   bash tools/check_portable.sh          # 自动探测工具链
#   ARM_GCC=/path/to/arm-none-eabi-gcc bash tools/check_portable.sh
#
# 退出码：0 = 全部通过；1 = 有失败
# =============================================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PROTO_INC="firmware/common/protocol/include"
NODE1_INC="firmware/node1/include"
NODE1_SRC="firmware/node1/src"
INCLUDES="-I${PROTO_INC} -I${NODE1_INC} -I${NODE1_SRC}"

# 只检查零 HAL 依赖的源码 —— 这些是"PC 与 MCU 共用同一份"的部分。
# port/stm32f103 依赖 STM32 HAL（未随仓库提供），故不在此列。
SOURCES=(
  firmware/common/protocol/src/proto_id.c
  firmware/common/protocol/src/proto_codec.c
  firmware/common/protocol/src/proto_seq.c
  firmware/common/protocol/src/proto_crc.c
  firmware/common/protocol/src/proto_node.c
  firmware/node1/src/node1_app.c
  firmware/node1/src/node1_sched.c
)

# 严格程度说明：
#   -Wall -Wextra -Wpedantic  —— 项目视为必须通过的门槛
#   -Werror                   —— 告警即失败，不留"以后再说"
#   -ffreestanding            —— 不假设有标准库（MCU 侧的真实约束）
STRICT="-Wall -Wextra -Wpedantic -Werror -Os -ffreestanding"

fail=0

# ---------------------------------------------------------------------------
# 1. Cortex-M3（真实目标）
# ---------------------------------------------------------------------------
ARM_GCC="${ARM_GCC:-}"
if [ -z "$ARM_GCC" ]; then
  if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    ARM_GCC="$(command -v arm-none-eabi-gcc)"
  else
    echo "[SKIP] 未找到 arm-none-eabi-gcc —— 跳过 Cortex-M3 交叉编译检查"
    echo "       安装 STM32CubeCLT 或设置 ARM_GCC 环境变量后重试"
    ARM_GCC=""
  fi
fi

if [ -n "$ARM_GCC" ]; then
  echo "== Cortex-M3 交叉编译（零 HAL 层）=="
  echo "   工具链: $ARM_GCC ($("$ARM_GCC" -dumpversion 2>/dev/null))"
  for f in "${SOURCES[@]}"; do
    out=$("$ARM_GCC" -mcpu=cortex-m3 -mthumb -std=c99 $STRICT $INCLUDES \
           -c "$f" -o /dev/null 2>&1)
    if [ -z "$out" ]; then
      printf "  [ ok ] %s\n" "$f"
    else
      printf "  [FAIL] %s\n" "$f"
      echo "$out" | head -10 | sed 's/^/         /'
      fail=1
    fi
  done
  echo
fi

# ---------------------------------------------------------------------------
# 1.5 板级配置头（引脚分配）自洽性
# ---------------------------------------------------------------------------
# 为什么单独查：#error 只有在头文件被包含时才会触发。板级配置里的
#   - 任务栈预算超限（20KB SRAM 是硬约束）
#   - 舵机角度超出协议允许范围
# 这类编译期断言若没人包含该头文件，就**永远不会生效** —— 等于没写。
# 这里用一个一次性 TU 把两个节点的配置头都包含进来，强制触发它们的 #error。
CONFIG_INCLUDES="-I${PROTO_INC} -I${NODE1_INC} -Ifirmware/node2/include"
CONFIG_TU="$(mktemp --suffix=.c)"
trap 'rm -f "$CONFIG_TU"' EXIT
cat > "$CONFIG_TU" <<'EOF'
/* 仅用于触发板级配置头的编译期校验（#error 与宏算术） */
#include "node1_config.h"
#include "node2_config.h"
int main(void) { return 0; }
EOF

if [ -n "$ARM_GCC" ]; then
  echo "== 板级配置头自洽性（Cortex-M3）=="
  out=$("$ARM_GCC" -mcpu=cortex-m3 -mthumb -std=c99 -Wall -Wextra -Wpedantic \
        -Werror -Os $CONFIG_INCLUDES -c "$CONFIG_TU" -o /dev/null 2>&1)
  if [ -z "$out" ]; then
    printf "  [ ok ] node1_config.h + node2_config.h\n"
  else
    printf "  [FAIL] 板级配置头\n"
    echo "$out" | head -10 | sed 's/^/         /'
    fail=1
  fi
  echo
fi

# ---------------------------------------------------------------------------
# 2. PC 原生（宿主编译器，若有 gcc/clang 也查一遍告警集合）
# ---------------------------------------------------------------------------
HOST_CC="${HOST_CC:-}"
if [ -z "$HOST_CC" ]; then
  for c in cc gcc clang; do
    if command -v "$c" >/dev/null 2>&1; then HOST_CC="$(command -v "$c")"; break; fi
  done
fi

if [ -n "$HOST_CC" ]; then
  echo "== PC 原生编译（宿主编译器告警集合）=="
  echo "   编译器: $HOST_CC ($("$HOST_CC" --version 2>/dev/null | head -1))"
  for f in "${SOURCES[@]}"; do
    out=$("$HOST_CC" -std=c99 $STRICT $INCLUDES -c "$f" -o /dev/null 2>&1)
    if [ -z "$out" ]; then
      printf "  [ ok ] %s\n" "$f"
    else
      printf "  [FAIL] %s\n" "$f"
      echo "$out" | head -10 | sed 's/^/         /'
      fail=1
    fi
  done
  echo
fi

# ---------------------------------------------------------------------------
# 3. 汇总
# ---------------------------------------------------------------------------
if [ "$fail" -eq 0 ]; then
  echo "可移植性检查通过：零 HAL 层在所有已配置工具链上零告警。"
  exit 0
fi
echo "可移植性检查**失败**：见上方 [FAIL] 项。"
exit 1
